#include <stdio.h>
struct{
    struct _header *next;  // next idle block
    int start;
    int size;
    void* content;
}_header;
struct _header a = {
	.next = 0,
	.start = 0,
	.size = 0,
	.content = 0
};
int main(){
	printf("test\n");
	printf("%d", sizeof(a));
	return 0;
}
