#include "../src/audio_queue_bridge.c"
#include <assert.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
static uint32_t cursor=0x20000000;static unsigned allocations,frees;static atomic_uint completions;
uint32_t compat_runtime32_allocate(size_t n,int clear){uint32_t p=cursor;cursor+=(n+15)&~15u;++allocations;if(clear)memset((void *)(uintptr_t)p,0,n);return p;}
void compat_runtime32_deallocate(uint32_t p){if(p)++frees;}
void *objc_bridge32_host_object(uint32_t token){assert(!token);return NULL;}
uint32_t compat_runtime32_call(uint32_t fn,const uint32_t *a,size_t n){
    assert(fn==123&&n==3&&a[0]==456&&a[1]>=0x7f090000);
    struct buffer32 *b=(void *)(uintptr_t)a[2];assert(b->capacity==960&&b->size==960&&b->data);
    atomic_fetch_add(&completions,1);return 0;
}
static uint64_t call(const char *name,uint32_t *a){uint64_t out=999;assert(audio_queue_bridge32_dispatch(name,a,&out));return out;}
int main(void){
    setenv("LP32_MUTE_AUDIO","1",1);
    assert(mmap((void *)0x20000000,1<<20,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)==(void *)0x20000000);
    uint32_t memory=compat_runtime32_allocate(256,1);AudioStreamBasicDescription *format=(void *)(uintptr_t)memory;
    *format=(AudioStreamBasicDescription){.mSampleRate=48000,.mFormatID=kAudioFormatLinearPCM,
        .mFormatFlags=kAudioFormatFlagIsSignedInteger|kAudioFormatFlagIsPacked,.mBytesPerPacket=2,.mFramesPerPacket=1,.mBytesPerFrame=2,.mChannelsPerFrame=1,.mBitsPerChannel=16};
    uint32_t a[]={memory,123,456,0,0,0,memory+64};assert(!call("_AudioQueueNewOutput",a));
    uint32_t token=*(uint32_t *)(uintptr_t)(memory+64);a[0]=token;a[1]=960;a[2]=memory+68;
    assert(!call("_AudioQueueAllocateBuffer",a));uint32_t buffer=*(uint32_t *)(uintptr_t)(memory+68);
    struct buffer32 *g=(void *)(uintptr_t)buffer;g->size=960;memset((void *)(uintptr_t)g->data,0x33,960);
    a[1]=kAudioQueueParam_Volume;float volume=1;memcpy(a+2,&volume,4);assert(!call("_AudioQueueSetParameter",a));
    a[2]=memory+72;assert(!call("_AudioQueueGetParameter",a));assert(*(float *)(uintptr_t)(memory+72)==0);
    a[1]=buffer;a[2]=0;a[3]=0;assert(!call("_AudioQueueEnqueueBuffer",a));
    a[1]=0;assert(!call("_AudioQueueStart",a));
    for(int i=0;i<2000&&!atomic_load(&completions);++i)usleep(1000);
    assert(atomic_load(&completions)==1);a[1]=1;assert(!call("_AudioQueueStop",a));
    assert(!call("_AudioQueueDispose",a));assert(call("_AudioQueueStart",a)==(uint32_t)paramErr);
    compat_runtime32_deallocate(memory);assert(allocations==frees);
    puts("AudioQueue bridge PASS (i386 buffers, real completion, enforced mute, disposal)");
}
