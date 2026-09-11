#include "../src/sound_manager_bridge.c"
#include <math.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static struct lp32_game_profile profile = {.title = LP32_TITLE_COD4};
const struct lp32_game_profile *lp32_profile(void) { return &profile; }
static uint32_t cursor=0x10001000;
static atomic_uint completions, stream_completions;
uint32_t compat_runtime32_allocate(size_t size,int zero) {
    uint32_t p=cursor;cursor+=(uint32_t)(size+15)&~15u;
    assert(cursor<0x10100000);if(zero)memset((void *)(uintptr_t)p,0,size);return p;
}
void compat_runtime32_deallocate(uint32_t p){(void)p;}
uint32_t compat_runtime32_call(uint32_t fn,const uint32_t *a,size_t count) {
    if(fn==2) {
        assert(count==2 && *(uint16_t *)(uintptr_t)a[1]==13);
        unsigned index=*(uint32_t *)(uintptr_t)(a[1]+4);
        assert(atomic_fetch_add(&stream_completions,1)==index);
        return 0;
    }
    assert(fn==1 && count==2 && a[0]>=0x10001000);
    assert(*(uint16_t *)(uintptr_t)a[1]==13);
    assert(*(uint32_t *)(uintptr_t)(a[1]+4)==0xabc123);
    atomic_fetch_add(&completions,1);return 0;
}
static uint64_t call(const char *name,uint32_t *a){uint64_t result;assert(sound_manager_bridge32_dispatch(name,a,&result));return result;}
/* Exercise the actual queue's stream format, then decode representative PCM
   with Core Audio. Byte-order mistakes change both polarity and amplitude. */
static void check_pcm_endianness(enum lp32_title title, uint8_t encoding,
                                 uint32_t tag, bool big_endian) {
    profile.title = title;
    *(uint32_t *)0x10000000 = 0;
    uint32_t create[] = {0x10000000, 5, 0xc4, 0};
    assert(!call("_SndNewChannel", create));
    uint32_t args[] = {*(uint32_t *)0x10000000, 0x10000020, 0};
    struct command32 *cmd = (void *)0x10000020;
    *cmd = (struct command32){86, 0, 0}; /* queue without playing */
    assert(!call("_SndDoImmediate", args));
    uint8_t *header = (void *)0x10000100;
    memset(header, 0, 128);
    const int16_t samples[] = {0, 256, -256, 4096, -8192, 32767, -32768, 1234};
    uint32_t channels_count = 2, rate = 44100u << 16, frames = 4;
    uint16_t bits = 16;
    memcpy(header + 4, &channels_count, 4);
    memcpy(header + 8, &rate, 4);
    header[20] = encoding;
    memcpy(header + 22, &frames, 4);
    memcpy(header + 40, &tag, 4);
    memcpy(header + (encoding == 0xff ? 48 : 62), &bits, 2);
    for (unsigned i = 0; i < 8; ++i) {
        uint16_t value = (uint16_t)samples[i];
        header[64 + 2*i] = big_endian ? value >> 8 : value & 255;
        header[65 + 2*i] = big_endian ? value & 255 : value >> 8;
    }
    *cmd = (struct command32){81, 0, 0x10000100};
    assert(!call("_SndDoImmediate", args));
    AudioStreamBasicDescription input = {0};
    UInt32 size = sizeof(input);
    assert(!AudioQueueGetProperty(channels[channel_count-1].audio,
        kAudioQueueProperty_StreamDescription, &input, &size));
    assert(input.mBitsPerChannel == 16 && input.mChannelsPerFrame == 2);
    AudioStreamBasicDescription output = {44100, kAudioFormatLinearPCM,
        kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0};
    AudioConverterRef converter;
    assert(!AudioConverterNew(&input, &output, &converter));
    float decoded[8] = {0}; size = sizeof(decoded);
    assert(!AudioConverterConvertBuffer(converter, sizeof(samples), header+64,
                                       &size, decoded));
    assert(size == sizeof(decoded));
    for (unsigned i = 0; i < 8; ++i)
        assert(fabsf(decoded[i] - samples[i] / 32768.0f) < 0.00001f);
    assert(!AudioConverterDispose(converter));
    uint32_t dispose[] = {args[0], 1};
    assert(!call("_SndDisposeChannel", dispose));
}

static void check_stream_queue(void) {
    profile.title=LP32_TITLE_COD4;
    *(uint32_t *)0x10000000=0;
    uint32_t create[]={0x10000000,5,0xc4,2};
    assert(!call("_SndNewChannel",create));
    uint32_t channel=*(uint32_t *)0x10000000;
    uint32_t args[]={channel,0x10000020,0};
    struct command32 *cmd=(void *)0x10000020;
    *cmd=(struct command32){86,0,0};assert(!call("_SndDoImmediate",args));
    struct sound_channel *c=&channels[channel_count-1];
    c->format=(AudioStreamBasicDescription){44100,kAudioFormatLinearPCM,
        kAudioFormatFlagIsSignedInteger|kAudioFormatFlagIsPacked,4,1,4,2,16,0};
    assert(!AudioQueueNewOutput(&c->format,completed,c,NULL,NULL,0,&c->audio));
    AudioStreamBasicDescription format={44100,kAudioFormatLinearPCM,
        kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked,8,1,8,2,32,0};
    assert(!AudioQueueSetOfflineRenderFormat(c->audio,&format,NULL));
    enum { blocks=4, frames=441, total=blocks*frames };
    int16_t expected[total*2];
    for(unsigned block=0;block<blocks;++block) {
        uint32_t address=0x10010000+block*0x4000;
        uint8_t *h=(void *)(uintptr_t)address;memset(h,0,64);
        *(uint32_t *)(h+4)=2;*(uint32_t *)(h+8)=44100u<<16;
        h[20]=0xff;uint32_t n=frames;memcpy(h+22,&n,4);*(uint16_t *)(h+48)=16;
        int16_t *pcm=(void *)(h+64);
        for(unsigned i=0;i<frames;++i){
            int16_t v=(int16_t)(1000+block*2000+i);
            pcm[2*i]=expected[(block*frames+i)*2]=v;
            pcm[2*i+1]=expected[(block*frames+i)*2+1]=-v;
        }
        *cmd=(struct command32){81,0,address};assert(!call("_SndDoCommand",args));
        *cmd=(struct command32){13,0,block};assert(!call("_SndDoCommand",args));
    }
    uint32_t query[]={channel,24,0x10000060};assert(!call("_SndChannelStatus",query));
    /* All blocks must be in the native queue before any playback/callback. */
    assert(c->queued_buffers==blocks && !atomic_load(&stream_completions));
    for(unsigned block=0;block<blocks;++block)
        memset((void *)(uintptr_t)(0x10010000+block*0x4000+64),0xcc,frames*4);
    AudioQueueBufferRef rendered;assert(!AudioQueueAllocateBuffer(c->audio,total*8,&rendered));
    AudioTimeStamp time={.mSampleTime=0,.mFlags=kAudioTimeStampSampleTimeValid};
    assert(!AudioQueueOfflineRender(c->audio,&time,rendered,0));
    assert(!AudioQueueStart(c->audio,NULL));
    assert(!AudioQueueOfflineRender(c->audio,&time,rendered,total));
    assert(rendered->mAudioDataByteSize==total*8);
    float *pcm=rendered->mAudioData;
    for(unsigned i=0;i<total*2;++i)assert(fabsf(pcm[i]-expected[i]/32768.0f)<0.0001f);
    time.mSampleTime=total;
    assert(!AudioQueueOfflineRender(c->audio,&time,rendered,frames));
    for(unsigned i=0;i<1000 && atomic_load(&stream_completions)<blocks;++i)usleep(1000);
    assert(atomic_load(&stream_completions)==blocks);
    assert(!AudioQueueFreeBuffer(c->audio,rendered));
    /* Teardown must cancel PCM already submitted to Core Audio and suppress
       its queued refill callbacks, including late native completion jobs. */
    *cmd=(struct command32){11,0,0};assert(!call("_SndDoImmediate",args));
    for(unsigned i=0;i<2;++i) {
        *cmd=(struct command32){81,0,0x10010000};assert(!call("_SndDoCommand",args));
        *cmd=(struct command32){13,0,blocks+i};assert(!call("_SndDoCommand",args));
    }
    assert(!call("_SndChannelStatus",query));assert(c->queued_buffers==2);
    *cmd=(struct command32){3,0,0};assert(!call("_SndDoImmediate",args));
    *cmd=(struct command32){4,0,0};assert(!call("_SndDoImmediate",args));
    *cmd=(struct command32){12,0,0};assert(!call("_SndDoImmediate",args));
    assert(!call("_SndChannelStatus",query));
    assert(!c->head && !c->playing && !c->queued_buffers);
    assert(atomic_load(&stream_completions)==blocks);
    uint32_t dispose[]={channel,1};assert(!call("_SndDisposeChannel",dispose));
}

int main(void) {
    assert(mmap((void *)0x10000000,0x100000,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE|MAP_FIXED,-1,0)==(void *)0x10000000);
    setenv("LP32_MUTE_AUDIO","1",1);
    uint32_t callback[]={1};assert(call("_NewSndCallBackUPP",callback)==1);
    uint32_t *out=(void *)0x10000000;out[1]=0xaabbccdd;
    uint32_t create[]={0x10000000,5,0xc4,1};assert(call("_SndNewChannel",create)==0 && out[1]==0xaabbccdd);
    uint32_t channel=out[0];assert(*(uint32_t *)(uintptr_t)(channel+8)==1);
    uint32_t query[]={channel,24,0x10000060};
    memset((void *)0x10000060,0xcc,28);
    assert(call("_SndChannelStatus",query)==0);
    assert(!*(unsigned char *)0x1000006c && *(uint32_t *)0x10000078==0xcccccccc);
    query[1]=23;assert((int32_t)call("_SndChannelStatus",query)==-50);query[1]=24;
    uint32_t *command=(void *)0x10000020;
    uint32_t args[]={channel,0x10000020,0};
    command[0]=86;command[1]=0;assert(call("_SndDoImmediate",args)==0);
    unsigned char *header=(void *)0x10000100;
    *(uint32_t *)(header+4)=800;*(uint32_t *)(header+8)=8000u<<16;header[20]=0;
    memset(header+22,128,800);
    command[0]=81;command[1]=0x10000100;assert(call("_SndDoCommand",args)==0);
    command[0]=13;command[1]=0xabc123;assert(call("_SndDoCommand",args)==0);
    usleep(200000);assert(!atomic_load(&completions));
    assert(call("_SndChannelStatus",query)==0 && *(unsigned char *)0x1000006c==1);
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
    assert(call("_SndChannelStatus",query)==0 && !*(unsigned char *)0x1000006c);
    assert(call("_DisposeSndCallBackUPP",callback)==0);
    uint32_t dispose[]={channel,1};assert(call("_SndDisposeChannel",dispose)==0);
    check_pcm_endianness(LP32_TITLE_COD4, 0xff, 0, false);
    check_pcm_endianness(LP32_TITLE_COD4_MP, 0xff, 0, false);
    check_pcm_endianness(LP32_TITLE_UNKNOWN, 0xff, 0, true);
    check_pcm_endianness(LP32_TITLE_COD4, 0xfe, 0x736f7774, false); /* sowt */
    check_pcm_endianness(LP32_TITLE_COD4, 0xfe, 0x74776f73, true); /* twos */
    check_pcm_endianness(LP32_TITLE_COD4, 0xfe, 0x4e4f4e45, true); /* NONE */
    check_stream_queue();
    puts("Sound Manager PASS (i386 channel, queued muted PCM, native little/big-endian decoding, gapless queued PCM, ordered completion, volume, disposal)");
}
