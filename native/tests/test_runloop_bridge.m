#include "cfnetwork_bridge.h"
#import <Foundation/Foundation.h>
#include <assert.h>
#include <sys/mman.h>
static id objects[32];static unsigned count;
uint32_t objc_bridge32_guest_object(void *raw){id o=raw;if(!o)return 0;for(unsigned i=0;i<count;++i)if(objects[i]==o)return i+1;assert(count<32);objects[count++]=[o retain];return count;}
void *objc_bridge32_host_object(uint32_t token){return token&&token<=count?objects[token-1]:nil;}
uint64_t compat_runtime32_return_double(double x){uint64_t w;memcpy(&w,&x,8);return w;}
static unsigned retains,releases,fires;
uint32_t compat_runtime32_call(uint32_t function,const uint32_t *a,uint32_t n){
    if(function==1){assert(n==1&&(a[0]==42||a[0]==43));++retains;return 43;}
    if(function==2){assert(n==1&&a[0]==43);++releases;return 0;}
    assert(function==3&&n==2&&a[1]==43&&objc_bridge32_host_object(a[0]));++fires;return 0;
}
static uint64_t call(const char *name,uint32_t *a){uint64_t out;assert(cfnetwork_bridge32_dispatch(name,a,&out));return out;}
int main(void){@autoreleasepool {
    uint32_t *memory=mmap((void *)0x10000000,4096,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE|MAP_FIXED,-1,0);assert(memory==(void *)0x10000000);
    uint32_t ctx[]={0,42,1,2,0};memcpy(memory,ctx,sizeof(ctx));
    uint32_t a[9]={0};double fire=CFAbsoluteTimeGetCurrent()+0.01;memcpy(a+1,&fire,8);a[7]=3;a[8]=0x10000000;
    uint32_t timer=(uint32_t)call("_CFRunLoopTimerCreate",a);assert(timer&&retains==1&&releases==0);
    a[0]=timer;a[1]=0x10000040;memory[21]=0xabcdef01;call("_CFRunLoopTimerGetContext",a);assert(memory[17]==43&&memory[21]==0xabcdef01);
    uint32_t loop=(uint32_t)call("_CFRunLoopGetCurrent",NULL),mode=objc_bridge32_guest_object((id)kCFRunLoopDefaultMode);
    a[0]=loop;a[1]=timer;a[2]=mode;call("_CFRunLoopAddTimer",a);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.03,false);assert(fires==1);
    a[0]=timer;call("_CFRunLoopTimerInvalidate",a);
    for(unsigned i=0;i<count;++i)[objects[i] release];assert(releases==retains);
    puts("Run-loop bridge PASS (timer callback, context canary, retain/release)");
}}
