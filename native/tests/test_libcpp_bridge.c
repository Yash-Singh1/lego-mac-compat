#include "../src/libcpp_bridge.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
static uint32_t cursor=0x30001000;
uint32_t compat_runtime32_allocate(size_t n,int clear){uint32_t p=cursor;cursor+=(n+15)&~15u;assert(cursor<0x30100000);if(clear)memset((void *)(uintptr_t)p,0,n);return p;}
void compat_runtime32_deallocate(uint32_t p){assert(p>=0x30001000 && p<cursor);}
static unsigned shared_destroyed,weak_destroyed;
uint32_t compat_runtime32_call(uint32_t function,const uint32_t *a,size_t count){
    assert(count==1&&a[0]==0x30000200);
    if(function==1)++shared_destroyed;else {assert(function==2);++weak_destroyed;}return 0;
}
static const char *prefix="__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE";
static uint32_t run(const char *suffix,uint32_t *a){char name[256];snprintf(name,sizeof(name),"%s%s",prefix,suffix);uint64_t r;assert(libcpp_bridge32_dispatch(name,a,&r));return r;}
static unsigned length(uint32_t *s){return s[0]&1?s[1]:*(uint8_t *)s>>1;}
static char *data(uint32_t *s){return s[0]&1?(void *)(uintptr_t)s[2]:(char *)s+1;}
int main(void){
    uint32_t *s=mmap((void *)0x30000000,0x100000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0);assert(s!=MAP_FAILED);
    char *input=(char *)s+128;memcpy(input,"abcdef",7);
    uint32_t a[8]={(uint32_t)(uintptr_t)s,(uint32_t)(uintptr_t)input,6,0};
    run("6__initEPKcm",a);assert(!(s[0]&1)&&length(s)==6&&!strcmp(data(s),"abcdef"));
    a[1]=(uint32_t)(uintptr_t)data(s);a[2]=6;run("6appendEPKcm",a);
    assert((s[0]&1)&&length(s)==12&&!strcmp(data(s),"abcdefabcdef"));
    a[1]=2;a[2]=UINT32_MAX;run("5eraseEmm",a);assert(length(s)==2&&!strcmp(data(s),"ab"));
    memcpy(input,"x\0y",3);a[1]=1;a[2]=(uint32_t)(uintptr_t)input;a[3]=3;run("6insertEmPKcm",a);
    assert(length(s)==5&&!memcmp(data(s),"ax\0yb\0",6));
    uint32_t *copy=s+8;a[0]=(uint32_t)(uintptr_t)copy;a[1]=(uint32_t)(uintptr_t)s;run("C1ERKS5_",a);
    assert(length(copy)==5&&!memcmp(data(copy),data(s),6));
    uint32_t *sub=s+12;a[0]=(uint32_t)(uintptr_t)sub;a[2]=1;a[3]=UINT32_MAX;
    run("C1ERKS5_mmRKS4_",a);assert(length(sub)==4&&!memcmp(data(sub),"x\0yb\0",5));
    run("D1Ev",a);a[2]=5;a[3]=12;run("C2ERKS5_mmRKS4_",a);assert(!length(sub)&&!data(sub)[0]);
    run("D1Ev",a);a[2]=0;a[3]=2;run("C1ERKS5_mmRKS4_",a);assert(length(sub)==2&&!strcmp(data(sub),"ax"));
    run("D1Ev",a);
    a[0]=(uint32_t)(uintptr_t)s;a[1]=7;a[2]='z';run("6resizeEmc",a);
    assert(length(s)==7&&!memcmp(data(s),"ax\0ybzz\0",8));assert(length(copy)==5);
    a[1]=200;run("7reserveEm",a);assert(length(s)==7&&(s[0]&~1u)>200);
    a[1]='!';run("9push_backEc",a);assert(length(s)==8&&data(s)[7]=='!'&&data(s)[8]==0);
    run("D1Ev",a);assert(!s[0]&&!s[1]&&!s[2]);
    a[0]=(uint32_t)(uintptr_t)copy;run("D1Ev",a);
    uint32_t *control=s+128,*vtable=s+144;control[0]=0x30000240;control[1]=0;control[2]=1;vtable[2]=1;vtable[4]=2;
    uint32_t shared_args[]={0x30000200};uint64_t result;
    assert(libcpp_bridge32_dispatch("__ZNSt3__119__shared_weak_count12__add_sharedEv",shared_args,&result)&&control[1]==1);
    assert(libcpp_bridge32_dispatch("__ZNSt3__119__shared_weak_count16__release_sharedEv",shared_args,&result)&&!shared_destroyed);
    assert(libcpp_bridge32_dispatch("__ZNSt3__119__shared_weak_count16__release_sharedEv",shared_args,&result)&&shared_destroyed==1&&!weak_destroyed&&control[2]==0);
    assert(libcpp_bridge32_dispatch("__ZNSt3__119__shared_weak_count14__release_weakEv",shared_args,&result)&&weak_destroyed==1);
    puts("libc++ shared count PASS (last strong/weak ownership and guest destructors)");
    puts("libc++ string PASS (short/long ABI, alias growth, embedded NUL, copy, resize, reserve, destruction)");
    return 0;
}
