/* Device-free mixer checks: render Sound Manager channels offline and
 * inspect the mixed samples, completion order and streaming behavior. */
#include "sound_manager_bridge.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static unsigned completions;
static uint32_t repeat_header;
static uint32_t cursor=0x10080000;
uint32_t compat_runtime32_allocate(size_t size,int zero) {
    uint32_t p=cursor;cursor+=(uint32_t)(size+15)&~15u;
    assert(cursor<0x10100000);if(zero)memset((void *)(uintptr_t)p,0,size);return p;
}
void compat_runtime32_deallocate(uint32_t p) {(void)p;}
uint32_t compat_runtime32_call(uint32_t fn,const uint32_t *a,size_t count) {
    assert(fn==1 && count==2 && *(uint16_t *)(uintptr_t)a[1]==13);
    ++completions;
    if(repeat_header) {
        uint32_t *cmd=(void *)0x10000040;
        cmd[0]=81;cmd[1]=repeat_header;repeat_header=0;
        uint32_t args[]={a[0],0x10000040,0};uint64_t result=0;
        assert(sound_manager_bridge32_dispatch("_SndDoCommand",args,&result));
        assert(result==0);
    }
    return 0;
}
static uint64_t call(const char *name,uint32_t *a) {
    uint64_t result;assert(sound_manager_bridge32_dispatch(name,a,&result));return result;
}
static void put32(unsigned char *p,uint32_t n) {memcpy(p,&n,4);}
static void put16(unsigned char *p,uint16_t n) {memcpy(p,&n,2);}
static float left[16384],right[16384];
static void render(uint32_t frames) {
    assert(frames<=16384);sound_manager_bridge32_render(left,right,frames);
}
static uint32_t new_channel(void) {
    uint32_t create[]={0x10000000,5,0xc4,1};*(uint32_t *)0x10000000=0;
    assert(call("_SndNewChannel",create)==0);return *(uint32_t *)0x10000000;
}
static void sync_channel(uint32_t channel) {
    uint32_t *cmd=(void *)0x10000060;cmd[0]=47;cmd[1]=0x10000068; /* getVolumeCmd */
    uint32_t a[]={channel,0x10000060,0};assert(call("_SndDoImmediate",a)==0);
}
static void dispose(uint32_t channel) {uint32_t a[]={channel,1};assert(call("_SndDisposeChannel",a)==0);}
/* 8-bit mono at the 8 kHz test output rate: one source frame per output frame. */
static void header8(unsigned char *h,uint32_t data,uint32_t frames) {
    memset(h,0,64);put32(h,data);put32(h+4,frames);put32(h+8,8000u<<16);
}
static float level8(unsigned char byte) {return ((int)byte-128)/128.0f;}

static void check_streaming(void) {
    unsigned char *h=(void *)0x10000100,*data=(void *)0x10000400;
    header8(h,0x10000400,8000);memset(data,140,8000);
    uint32_t channel=new_channel(),*cmd=(void *)0x10000020;
    uint32_t a[]={channel,0x10000020,0};
    cmd[0]=81;cmd[1]=0x10000100;assert(call("_SndDoImmediate",a)==0);
    unsigned char *next_header=(void *)0x10000200,*next_data=(void *)0x10004000;
    header8(next_header,0x10004000,1600);memset(next_data,100,1600);
    repeat_header=0x10000200;
    cmd[0]=13;cmd[1]=0;assert(call("_SndDoCommand",a)==0);
    sync_channel(channel);
    render(320);
    for(unsigned i=0;i<320;++i)assert(left[i]==level8(140) && right[i]==level8(140));
    /* The guest writes the rest after submission; the mixer reads it live. */
    memset(data+320,180,7680);
    render(6879); /* the 100 ms lead is 800 frames */
    assert(left[0]==level8(180));
    sound_manager_bridge32_deliver_events();assert(!completions);
    render(1);sound_manager_bridge32_deliver_events();
    assert(completions==1); /* reported early; the callback queued segment 2 */
    sync_channel(channel);
    /* The guest may refill the reported segment at once; its tail is a copy. */
    memset(data+7200,60,800);
    render(800);
    for(unsigned i=0;i<800;++i)assert(left[i]==level8(180));
    render(4);
    for(unsigned i=0;i<4;++i)assert(left[i]==level8(100)); /* no gap */
    render(1596);sound_manager_bridge32_deliver_events();
    assert(completions==1); /* nothing queued after segment 2 */
    render(4);assert(left[0]==0 && left[3]==0);
    dispose(channel);completions=0;
    puts("Sound Manager streaming PASS (live data, protected tail, gapless next segment)");
}
static void check_short_effect(void) {
    unsigned char *h=(void *)0x10000100,*data=(void *)0x10000400;
    header8(h,0x10000400,160);memset(data,200,160);
    uint32_t channel=new_channel(),*cmd=(void *)0x10000020;
    uint32_t a[]={channel,0x10000020,0};
    cmd[0]=81;cmd[1]=0x10000100;assert(call("_SndDoImmediate",a)==0);
    cmd[0]=13;cmd[1]=0;assert(call("_SndDoCommand",a)==0);
    sync_channel(channel);
    render(159);sound_manager_bridge32_deliver_events();
    assert(!completions); /* no early report for short effects */
    render(1);sound_manager_bridge32_deliver_events();
    assert(completions==1);
    /* Halving the volume halves both sides; a 2x multiplier doubles speed. */
    cmd[0]=46;cmd[1]=0x00800080;assert(call("_SndDoImmediate",a)==0);
    cmd[0]=86;cmd[1]=0x20000;assert(call("_SndDoImmediate",a)==0);
    cmd[0]=81;cmd[1]=0x10000100;assert(call("_SndDoImmediate",a)==0);
    cmd[0]=13;cmd[1]=0;assert(call("_SndDoCommand",a)==0);
    sync_channel(channel);
    render(80);
    for(unsigned i=0;i<80;++i)assert(fabsf(left[i]-level8(200)/2)<1e-6f && left[i]==right[i]);
    sound_manager_bridge32_deliver_events();assert(completions==2);
    /* quietCmd stops at once and discards stale completion reports. */
    cmd[0]=86;cmd[1]=0x10000;assert(call("_SndDoImmediate",a)==0);
    cmd[0]=81;cmd[1]=0x10000100;assert(call("_SndDoImmediate",a)==0);
    cmd[0]=13;cmd[1]=0;assert(call("_SndDoCommand",a)==0);
    sync_channel(channel);render(20);
    cmd[0]=3;assert(call("_SndDoImmediate",a)==0);
    cmd[0]=4;assert(call("_SndDoImmediate",a)==0);
    sync_channel(channel);render(200);sound_manager_bridge32_deliver_events();
    assert(completions==2 && left[0]==0);
    dispose(channel);completions=0;
    puts("Sound Manager short effect PASS (completion after final sample, volume, rate, quiet)");
}
static void check_many_channels(void) {
    enum { kChannels = 48 };
    unsigned char *h=(void *)0x10000100,*data=(void *)0x10000400;
    header8(h,0x10000400,400);memset(data,129,400);
    uint32_t channels[kChannels],*cmd=(void *)0x10000020;
    for(unsigned i=0;i<kChannels;++i) {
        channels[i]=new_channel();
        uint32_t a[]={channels[i],0x10000020,0};
        cmd[0]=81;cmd[1]=0x10000100;assert(call("_SndDoImmediate",a)==0);
        cmd[0]=13;cmd[1]=0;assert(call("_SndDoCommand",a)==0);
        sync_channel(channels[i]);
    }
    render(400);sound_manager_bridge32_deliver_events();
    assert(completions==kChannels);
    assert(fabsf(left[0]-kChannels*level8(129))<1e-4f);
    for(unsigned i=0;i<kChannels;++i)dispose(channels[i]);
    completions=0;
    puts("Sound Manager many channels PASS (48 simultaneous voices mixed and completed)");
}
static void check(unsigned encode,unsigned bits,unsigned channels,uint32_t codec,int big,int muted,int external) {
    unsigned char *h=(void *)0x10000100;memset(h,0,256);
    unsigned frames=4,offset=encode?64:22,size=frames*channels*(bits/8);
    unsigned char *data=external?(void *)0x10000400:h+offset;
    if(external)put32(h,(uint32_t)(uintptr_t)data);
    put32(h+4,encode?channels:frames);put32(h+8,8000u<<16);h[20]=encode;
    if(encode){put32(h+22,frames);put16(h+(encode==0xff?48:62),bits);}
    if(encode==0xfe)put32(h+40,codec);
    for(unsigned i=0;i<size;++i)data[i]=(unsigned char)(i*31+7);
    if(muted)setenv("LP32_MUTE_AUDIO","1",1);else unsetenv("LP32_MUTE_AUDIO");
    uint32_t channel=new_channel();
    uint32_t *cmd=(void *)0x10000020;cmd[0]=81;cmd[1]=0x10000100;
    uint32_t a[]={channel,0x10000020,0};
    assert(call("_SndDoImmediate",a)==0);sync_channel(channel);
    render(frames);
    unsigned width=bits/8;
    for(unsigned f=0;f<frames;++f)for(unsigned side=0;side<2;++side) {
        const unsigned char *p=data+f*channels*width+(channels>1?side:0)*width;
        double expected;
        if(bits==8)expected=(p[0]-128)/128.0;
        else {
            int64_t v=0;
            for(unsigned b=0;b<width;++b)v|=(int64_t)p[big?b:width-1-b]<<(8*(width-1-b));
            if(v&((int64_t)1<<(bits-1)))v-=(int64_t)1<<bits;
            expected=v/(double)((int64_t)1<<(bits-1));
        }
        float got=side?right[f]:left[f];
        assert(fabs(got-(muted?0:expected))<1e-5);
    }
    dispose(channel);
}
int main(void) {
    assert(mmap((void *)0x10000000,0x100000,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE|MAP_FIXED,-1,0)==(void *)0x10000000);
    sound_manager_bridge32_use_test_output(8000);
    check_streaming();
    check_short_effect();
    check_many_channels();
    for(int muted=0;muted<=1;++muted)for(int external=0;external<=1;++external) {
        check(0,8,1,0,0,muted,external);
        check(0xff,8,2,0,0,muted,external);
        check(0xff,16,1,0,0,muted,external);
        check(0xff,16,2,0,0,muted,external);
        check(0xff,24,2,0,0,muted,external);
        check(0xff,32,2,0,0,muted,external);
        check(0xfe,16,2,0x736f7774,0,muted,external); /* sowt */
        check(0xfe,16,2,0x74776f73,1,muted,external); /* twos */
        check(0xfe,16,2,0x4e4f4e45,1,muted,external); /* NONE */
    }
    puts("Sound PCM PASS (native i386 samples, explicit endian codecs, inline/external data, muting; no audio device)");
}
