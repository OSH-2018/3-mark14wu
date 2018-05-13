/*
 * Copyright © 2018 Zitian Li <ztlizitian@gmail.com>
 * This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details.
 */
#define _OSH_FS_VERSION 2018051000
#define FUSE_USE_VERSION 26
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fuse.h>
#include <sys/mman.h>

struct filenode {
    char *filename;
    void *content;
    struct stat *st;
    struct filenode *next;
};

static const size_t size = 4 * 1024 * 1024 * (size_t)1024;
static void *mem[1024 * 1024];
static const size_t blocksize = 4096;
static const size_t blocknr = size / blocksize;

static struct filenode *root = NULL;

// malloc and realloc -------------------

// 对齐部分，按照8字节为最小单位对齐（取double的长度）
// 这里巧妙地运用了 C 语言中 union 的语言特性来对齐
typedef double Align;

union header{
    struct{
        union header *ptr;  // next idle block
        unsigned size;
    }s;
    Align x;
};

typedef union header Header;

static Header base;     // 链表头
static Header *freep = NULL;        // 空闲块开头

void *my_malloc(size_t nbytes){
    Header *p, *prevp;
    Header *my_morecore(size_t);
    unsigned nunits;

    nunits = (nbytes+sizeof(Header)-1)/sizeof(Header) + 1;  // 向上取整，以满足对齐要求
    if ((prevp = freep) == NULL) {              // 初始化空闲块
        base.s.ptr = freep = prevp = &base;
        base.s.size = 0;
    }
    // 这里采用了“首次分配”的分配策略
    for (p = prevp->s.ptr; ; prevp = p, p = p->s.ptr) {         // 遍历空闲块链表
        if (p->s.size >= nunits) {          // 空闲块足够大时
            if (p->s.size == nunits)        // 刚好一样大时
                prevp->s.ptr = p->s.ptr;
            else {
                // 将该块切为两块，将尾部的新块分配出去
                p->s.size -= nunits;
                p += p->s.size;
                p->s.size = nunits;
            }
            freep = prevp;
            return (void *)(p+1);
        }
        if (p == freep) /* wrapped around free list */
            if ((p = my_morecore(nunits)) == NULL)
                return NULL;
    /* none left */
    }
}

Header  *my_morecore(size_t nu){
    return NULL;
}
// malloc and realloc finished ----------

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
    size_t blocksize = 4096;
    size_t blocknr = size / blocksize;
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
    // Not Implemented
    return 0;
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
