#include "sound_manager_bridge.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static uint32_t cursor=0x10001000;
static atomic_uint completions;
uint32_t compat_runtime32_allocate(size_t size,int zero) {
    uint32_t p=cursor;cursor+=(uint32_t)(size+15)&~15u;
    assert(cursor<0x10100000);if(zero)memset((void *)(uintptr_t)p,0,size);return p;
}
void compat_runtime32_deallocate(uint32_t p){(void)p;}
uint32_t compat_runtime32_call(uint32_t fn,const uint32_t *a,size_t count) {
    assert(fn==1 && count==2 && a[0]>=0x10001000);
    assert(*(uint16_t *)(uintptr_t)a[1]==13);
    assert(*(uint32_t *)(uintptr_t)(a[1]+4)==0xabc123);
    atomic_fetch_add(&completions,1);return 0;
}
static uint64_t call(const char *name,uint32_t *a){uint64_t result;assert(sound_manager_bridge32_dispatch(name,a,&result));return result;}
int main(void) {
    assert(mmap((void *)0x10000000,0x100000,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE|MAP_FIXED,-1,0)==(void *)0x10000000);
    setenv("LP32_MUTE_AUDIO","1",1);
    uint32_t *out=(void *)0x10000000;out[1]=0xaabbccdd;
    uint32_t create[]={0x10000000,5,0xc4,1};assert(call("_SndNewChannel",create)==0 && out[1]==0xaabbccdd);
    uint32_t channel=out[0];assert(*(uint32_t *)(uintptr_t)(channel+8)==1);
    uint32_t *command=(void *)0x10000020;
    uint32_t args[]={channel,0x10000020,0};
    command[0]=86;command[1]=0;assert(call("_SndDoImmediate",args)==0);
    unsigned char *header=(void *)0x10000100;
    *(uint32_t *)(header+4)=800;*(uint32_t *)(header+8)=8000u<<16;header[20]=0;
    memset(header+22,128,800);
    command[0]=81;command[1]=0x10000100;assert(call("_SndDoCommand",args)==0);
    command[0]=13;command[1]=0xabc123;assert(call("_SndDoCommand",args)==0);
    usleep(200000);assert(!atomic_load(&completions));
    command[0]=86;command[1]=0x18000;assert(call("_SndDoImmediate",args)==0);
    command[0]=87;command[1]=0x10000040;*(uint32_t *)0x10000044=0xbeefcafe;
    assert(call("_SndDoImmediate",args)==0);
    assert(*(uint32_t *)0x10000040==0x18000 && *(uint32_t *)0x10000044==0xbeefcafe);
    for(unsigned i=0;i<3000&&!atomic_load(&completions);++i)usleep(1000);
    assert(atomic_load(&completions)==1);
    command[0]=46;command[1]=0x800040;assert(call("_SndDoImmediate",args)==0);
    command[0]=47;command[1]=0x10000040;*(uint32_t *)0x10000044=0xbeefcafe;
    assert(call("_SndDoImmediate",args)==0);
    assert(*(uint32_t *)0x10000040==0x800040 && *(uint32_t *)0x10000044==0xbeefcafe);
    uint32_t dispose[]={channel,1};assert(call("_SndDisposeChannel",dispose)==0);
    puts("Sound Manager PASS (i386 channel, queued muted PCM, completion, volume, disposal)");
}
