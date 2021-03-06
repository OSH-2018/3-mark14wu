
#define _OSH_FS_VERSION 2018051000
#define FUSE_USE_VERSION 26
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fuse.h>
#include <sys/mman.h>
#define BLOCKSIZE 4096
struct filenode {
    char *filename;
    void *content;
    struct stat *st;
    struct filenode *next;
};

static const size_t size = 4 * 1024 * 1024 * (size_t)1024;
static void *mem[1024 * 1024];
static const size_t blocksize = BLOCKSIZE;
static const size_t blocknr = size / blocksize;

static struct filenode *root = NULL;

// malloc and realloc -------------------

struct _header{
    struct _header *next;  // next idle block
    int start;
    int size;
    void* content;
};

typedef struct _header Header;

// printf("sizeof header is %d", sizeof(Header));

// 使用 块 1 (block 1) 来存放其余文件的块指针
// block 1 is from 4096 to 8191

char header_used[BLOCKSIZE];

Header *header_malloc(){        // 我们把第一块用于存放指向块的指针
    static void *freep = mem + blocksize; // 指向空闲块指针
    void *p;
    for (p = freep; p <= (void*)(mem + 2 * blocksize - 1 - sizeof(Header)); p++){
        if (!*(header_used + (size_t)((void**)p - (mem + blocksize)))){      // 如果这一根指针没有用过
            freep = p+1;
            *(header_used + (size_t)((void**)p - (mem + blocksize))) = 1;    // 设置该指针为用过
            return (Header*) p;
        }
    }
    for(p = (void*)(mem + blocksize); p <= freep - 1; p++){    // 如果超过了8191，那么从4096重新开始扫描
        if (!*(header_used + (size_t)((void**)p - (mem + blocksize)))){
            freep = p+1;
            *(header_used + (size_t)((void**)p - (mem + blocksize))) = 1;
            return p;
        }
    }
    fprintf(stderr, "块指针已满\n");
    exit(-1);
    return NULL;
}

void header_free(Header* bp){
    header_used[(size_t)((void**)bp - (mem + blocksize))] = 0;
    memset(bp, 0, sizeof(Header));
}



static Header *freep = NULL;        // 空闲块开头
static Header *base = NULL;

Header *block_malloc(size_t nbytes){

    if (base == NULL)
        base = header_malloc();     // 分配链表头
    if (base == NULL){
        fprintf(stderr, "空闲链表头分配失败！\n");
        exit(-1);
        return NULL;
    }

    Header *p, *prevp;
    unsigned nblocks;

    nblocks = (nbytes - 1) / blocksize + 1;  // 向上取整，以满足对齐要求(以block的整数倍分配空间)
    if ((prevp = freep) == NULL) {              // 初始化空闲块
        base->next = freep = prevp = base;
        base->start = 0;
        base->size = blocknr;
    }
    // 这里采用了“首次分配”的分配策略
    // 为了方便分块存储，顺便找出剩余空闲块中的最大值
    Header *max_block = header_malloc();
    max_block->size = 0;
    for (p = prevp->next; ; prevp = p, p = p->next) {         // 遍历空闲块链表
        if (p->size > max_block->size)
            max_block = p;
        if (p->size  >= nblocks) {          // 空闲块足够大时
            if (p->size  == nblocks){        // 刚好一样大时
                prevp->next = p->next;
                p->next = NULL;
                p->content = mmap(NULL, p->size * blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                memset(p->content, 0, p->size * blocksize);
                freep = prevp;                      // 每次寻找空闲块都从最近一次找到的空闲块开始
                return p;
            }
            else {
                // 将该块切为两块，将尾部的新块分配出去
                p->size -= nblocks;
                Header *q = header_malloc();
                q->start = p->start + p->size;
                q->size = nblocks;
                q->next = NULL;
                q->content = mmap(NULL, q->size * blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                memset(q->content, 0, q->size * blocksize);
                freep = prevp;
                return q;
            }
        }
        if (p == freep){ // 遍历整个空闲块链表都没有发现足够大的单独空闲块，必须分块存储
            p = max_block;
            if (max_block == 0){    // 空闲块已经用完了
                fprintf(stderr, "空闲块不足！\n");
                exit(-1);
                return NULL;
            }
            p->content = mmap(NULL, p->size * blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            memset(p->content, 0, p->size * blocksize);
            p->next = block_malloc(nbytes - (size_t)max_block->size * blocksize);
        }
    }
}

void block_free(Header *bp){

    Header *p;

    if (bp->next != NULL){
        block_free(bp->next);
        bp->next = NULL;
    }

    // 释放掉 bp 占用的内存映射
    munmap(bp, bp->size * blocksize);

    // 将 bp 插入到 p 和 p->next 之间
    for (p = freep; !(bp > p && bp < p->next); p = p->next)
        if(p >= p->next && (bp > p || bp < p->next))
            break;  // 特判，当 p 和 p->next 包含区间的末尾

    // 将 bp 与 p->next 相连接
    // 如果 bp 和 p->next 能合并
    if (bp + bp->size == p->next){
        bp->size += p->next->size;
        bp->next = p->next->next;
        header_free(p->next);
    }
    else    // 不能合并
        bp->next = p->next;

    // 将 p 与 bp 相连接
    // 如果 p 与 bp 能够合并
    if(p + p->size == bp){
        p->size += bp->size;
        p->next = bp->next;
        header_free(bp);
    }
    else
        p->next = bp;
    freep = p;
}
// malloc and realloc finished

// fuse system calls start from here

static struct filenode *get_filenode(const char *name)
{
    struct filenode *node = root;
    while(node) {
        if(strcmp(node->filename, name + 1) != 0)
            node = node->next;
        else
            return node;
    }
    return NULL;
}

static void create_filenode(const char *filename, const struct stat *st)
{
    struct filenode *new = (struct filenode *)malloc(sizeof(struct filenode));
    new->filename = (char *)malloc(strlen(filename) + 1);
    memcpy(new->filename, filename, strlen(filename) + 1);
    new->st = (struct stat *)malloc(sizeof(struct stat));
    memcpy(new->st, st, sizeof(struct stat));
    new->next = root;
    new->content = NULL;
    root = new;
}

static void *oshfs_init(struct fuse_conn_info *conn)
{
    // size_t blocknr = sizeof(mem) / sizeof(mem[0]);
    // size_t blocksize = size / blocknr;

    // Demo 1
    for(int i = 0; i < blocknr; i++) {
        mem[i] = mmap(NULL, blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        memset(mem[i], 0, blocksize);
    }
    for(int i = 0; i < blocknr; i++) {
        munmap(mem[i], blocksize);
    }
    // Demo 2
    mem[0] = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for(int i = 0; i < blocknr; i++) {
        mem[i] = (char *)mem[0] + blocksize * i;
        memset(mem[i], 0, blocksize);
    }
    for(int i = 0; i < blocknr; i++) {
        munmap(mem[i], blocksize);
    }
    return NULL;
}

static int oshfs_getattr(const char *path, struct stat *stbuf)
{
    int ret = 0;
    struct filenode *node = get_filenode(path);
    if(strcmp(path, "/") == 0) {
        memset(stbuf, 0, sizeof(struct stat));
        stbuf->st_mode = S_IFDIR | 0755;
    } else if(node) {
        memcpy(stbuf, node->st, sizeof(struct stat));
    } else {
        ret = -ENOENT;
    }
    return ret;
}

static int oshfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
    struct filenode *node = root;
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    while(node) {
        filler(buf, node->filename, node->st, 0);
        node = node->next;
    }
    return 0;
}

static int oshfs_mknod(const char *path, mode_t mode, dev_t dev)
{
    struct stat st;
    st.st_mode = S_IFREG | 0644;
    st.st_uid = fuse_get_context()->uid;
    st.st_gid = fuse_get_context()->gid;
    st.st_nlink = 1;
    st.st_size = 0;
    create_filenode(path + 1, &st);
    return 0;
}

static int oshfs_open(const char *path, struct fuse_file_info *fi)
{
    // int retstat = 0;
    // int fd;
    // char fpath[PATH_MAX];

    // fullpath(fpath, path);
    
    // log_msg("bb_open(fpath\"%s\", fi=0x%08x)\n",
	//     fpath,  (int) fi);
    
    // fd = open(fpath, fi->flags);
    // if (fd < 0){
    //     retstat = -1;
    //     fprintf(stderr, "oshfs_open error!\n");
    // }

    // fi->fh = fd;
    // log_fi(fi);
    
    // return retstat;
}

static int oshfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    struct filenode *node = get_filenode(path);
    if(offset + size > node->st->st_size)
        node->st->st_size = offset + size;
    node->content = realloc(node->content, node->st->st_size);
    memcpy(node->content + offset, buf, size);
    return size;
}

static int oshfs_truncate(const char *path, off_t size)
{
    struct filenode *node = get_filenode(path);
    node->st->st_size = size;
    node->content = realloc(node->content, size);
    return 0;
}

static int oshfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    struct filenode *node = get_filenode(path);
    int ret = size;
    if(offset + size > node->st->st_size)
        ret = node->st->st_size - offset;
    memcpy(buf, node->content + offset, ret);
    return ret;
}

static int oshfs_unlink(const char *path)
{
    // Not Implemented
    return 0;
}

static const struct fuse_operations op = {
    .init = oshfs_init,
    .getattr = oshfs_getattr,
    .readdir = oshfs_readdir,
    .mknod = oshfs_mknod,
    .open = oshfs_open,
    .write = oshfs_write,
    .truncate = oshfs_truncate,
    .read = oshfs_read,
    .unlink = oshfs_unlink,
};

int main(int argc, char *argv[])
{
    return fuse_main(argc, argv, &op, NULL);
}
