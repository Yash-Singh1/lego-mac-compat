#include "../src/blocks_bridge.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
static uint32_t cursor=0x30001000;static unsigned freed,copied,disposed;
uint32_t compat_runtime32_allocate(size_t n,int clear){uint32_t p=cursor;cursor+=(n+15)&~15u;assert(cursor<0x30100000);if(clear)memset((void *)(uintptr_t)p,0,n);return p;}
void compat_runtime32_deallocate(uint32_t p){assert(p>=0x30001000 && p<cursor);freed++;}
uint64_t compat_runtime32_dispatch_import(const char *n,const uint32_t *a){(void)n;(void)a;assert(!"unexpected object capture");return 0;}
uint32_t compat_runtime32_call(uint32_t fn,const uint32_t *a,size_t n){
 uint64_t r;uint32_t *b=(void *)(uintptr_t)a[0];
 if(fn==1){assert(n==1);return *(uint32_t *)(uintptr_t)(b[5]+16);}
 if(fn==2){assert(n==2);uint32_t *src=(void *)(uintptr_t)a[1];uint32_t x[]={a[0]+20,src[5],8};assert(blocks_bridge32_dispatch("__Block_object_assign",x,&r));copied++;return 0;}
 if(fn==3){assert(n==1);uint32_t x[]={b[5],8};assert(blocks_bridge32_dispatch("__Block_object_dispose",x,&r));disposed++;return 0;}
 assert(!"unknown callback");return 0;
}
int main(void){
 uint32_t *m=mmap((void *)0x30000000,0x100000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0);assert(m!=MAP_FAILED);
 uint32_t block=(uint32_t)(uintptr_t)m,desc=block+128,byref=block+256;
 uint32_t d[]={0,24,2,3};memcpy((void *)(uintptr_t)desc,d,sizeof(d));
 uint32_t b[]={0,1u<<25,0,1,desc,byref};memcpy(m,b,sizeof(b));
 uint32_t ref[]={0,byref,0,20,42};memcpy((void *)(uintptr_t)byref,ref,sizeof(ref));
 uint32_t heap=blocks_bridge32_copy(block);assert(heap!=block&&copied==1);
 assert(blocks_bridge32_invoke(heap)==42);assert(*(uint32_t *)(uintptr_t)(byref+4)!=byref);
 assert(blocks_bridge32_copy(heap)==heap);blocks_bridge32_release(heap);assert(!freed);
 blocks_bridge32_release(heap);assert(freed==1&&disposed==1);
 uint32_t a[]={byref,8};uint64_t r;assert(blocks_bridge32_dispatch("__Block_object_dispose",a,&r));assert(freed==2);
 m[1]=1u<<28;assert(blocks_bridge32_copy(block)==block);blocks_bridge32_release(block);assert(freed==2);
 puts("Blocks PASS (copy/dispose helpers, heap references, byref forwarding, global lifetime)");
 return 0;
}
