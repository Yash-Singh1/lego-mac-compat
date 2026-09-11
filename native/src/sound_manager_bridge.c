/* Removed Carbon Sound Manager channels, backed by Audio Queue Services.
 * The guest owns i386 records; native queues never receive guest callbacks.
 * Command order and completion notifications survive muted test playback. */
#include "sound_manager_bridge.h"
#include "compat_runtime.h"
#include "game_profile.h"
#include <AudioToolbox/AudioToolbox.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#pragma pack(push, 2)
struct command32 { uint16_t command; int16_t param1; uint32_t param2; };
#pragma pack(pop)
struct pending {
    struct command32 command;
    struct pending *next;
    AudioQueueBufferRef buffer;
    bool submitted, done;
};
struct sound_channel {
    uint32_t guest, callback, volume, rate, multiplier, generation;
    atomic_bool closed;
    bool playing, paused, owns_guest, advancing;
    unsigned queued_buffers, immediate_buffers;
    dispatch_queue_t worker;
    AudioQueueRef audio;
    AudioStreamBasicDescription format;
    struct pending *head, *tail;
};
/* Keep callback contexts stable after disposal; late AudioQueue notifications
 * can still carry their address. The number of simultaneously created voices
 * is bounded, and disposed slots are not reused while callbacks may exist. */
static struct sound_channel channels[1024];
static unsigned channel_count;
static pthread_mutex_t channel_lock = PTHREAD_MUTEX_INITIALIZER;
static void advance(struct sound_channel *);
static uint32_t u32(const void *p) { uint32_t n; memcpy(&n,p,4);return n; }
static uint16_t u16(const void *p) { uint16_t n; memcpy(&n,p,2);return n; }
static void completed(void *raw, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    struct sound_channel *channel = raw;
    uint32_t generation = (uint32_t)(uintptr_t)buffer->mUserData;
    dispatch_async(channel->worker, ^{
        if (!channel->closed && channel->audio == queue) {
            if (generation == channel->generation) {
                bool queued = false;
                for (struct pending *p = channel->head; p; p = p->next) {
                    if (p->buffer != buffer) continue;
                    p->buffer = NULL;
                    p->done = true;
                    queued = true;
                    break;
                }
                if (queued && channel->queued_buffers) --channel->queued_buffers;
                else if (!queued && channel->immediate_buffers) --channel->immediate_buffers;
                channel->playing = channel->queued_buffers || channel->immediate_buffers;
            }
            AudioQueueFreeBuffer(queue, buffer);
            if (generation == channel->generation) advance(channel);
        }
    });
}
static void volume(struct sound_channel *c) {
    if (!c->audio) return;
    float left=(c->volume & 0xffff)/256.0f, right=(c->volume>>16)/256.0f;
    float level=getenv("LP32_MUTE_AUDIO")?0.0f:(left>right?left:right);
    AudioQueueSetParameter(c->audio,kAudioQueueParam_Volume,level);
    if (left+right>0) AudioQueueSetParameter(c->audio,kAudioQueueParam_Pan,(right-left)/(left+right));
}
static void playback_rate(struct sound_channel *c) {
    if(!c->audio)return;
    double ratio=c->rate/65536.0/c->format.mSampleRate*c->multiplier/65536.0;
    if(ratio<=0 || c->paused)AudioQueuePause(c->audio);
    else {
        AudioQueueSetParameter(c->audio,kAudioQueueParam_PlayRate,(Float32)ratio);
        if(c->playing)AudioQueueStart(c->audio,NULL);
    }
}
static int play(struct sound_channel *c, uint32_t address, AudioQueueBufferRef *queued) {
    if (!address) return -50;
    const unsigned char *h=(void *)(uintptr_t)address;
    uint32_t data=u32(h), count=u32(h+4), frames=count, bits=8, channels_count=1;
    double sample_rate=u32(h+8)/65536.0;
    unsigned offset=22; bool big_endian=false;
    if (h[20]==0xff || h[20]==0xfe) {
        channels_count=count;frames=u32(h+22);offset=64;
        bits=u16(h+(h[20]==0xff?48:62));
        /* COD4's i386 Bink backend supplies native little-endian PCM in an
           ExtSoundHeader. Treating it as classic Mac big-endian PCM turns
           decoded music/speech into static. Compressed headers below carry
           an explicit format tag; other titles retain their existing default. */
        enum lp32_title title = lp32_profile()->title;
        bool cod4 = title == LP32_TITLE_COD4 || title == LP32_TITLE_COD4_MP;
        big_endian = bits > 8 && !(h[20] == 0xff && cod4);
        if(h[20]==0xfe) {
            uint32_t format=u32(h+40);
            if(format==0x736f7774)big_endian=false; /* sowt */
            else if(format!=0x74776f73 && format!=0x4e4f4e45 && format!=0x72617720)return -206;
        }
    } else if(h[20]!=0) return -206;
    if(!data)data=address+offset;
    if(!channels_count || channels_count>8 || !frames || (bits!=8 && bits!=16 && bits!=24 && bits!=32) || sample_rate<1000 || sample_rate>384000)return -206;
    AudioStreamBasicDescription format={0};
    format.mSampleRate=sample_rate;format.mFormatID=kAudioFormatLinearPCM;
    format.mFormatFlags=kAudioFormatFlagIsPacked|(bits>8?kAudioFormatFlagIsSignedInteger:0)|(big_endian?kAudioFormatFlagIsBigEndian:0);
    format.mFramesPerPacket=1;format.mChannelsPerFrame=channels_count;format.mBitsPerChannel=bits;
    format.mBytesPerFrame=format.mBytesPerPacket=channels_count*(bits/8);
    uint64_t size=(uint64_t)frames*format.mBytesPerFrame;if(size>UINT32_MAX)return -206;
    if(c->audio && memcmp(&format,&c->format,sizeof(format))) {
        /* A different sample format is a queue barrier. Keep earlier audio
           alive until it drains instead of disposing a queue still in use. */
        if (c->playing) return queued ? 1 : -50;
        AudioQueueDispose(c->audio,true);c->audio=NULL;
    }
    OSStatus status=0;
    if(!c->audio) {
        status=AudioQueueNewOutput(&format,completed,c,NULL,NULL,0,&c->audio);
        if(status)return (int)status;
        c->format=format;volume(c);
        UInt32 enabled=1;
        AudioQueueSetProperty(c->audio,kAudioQueueProperty_EnableTimePitch,&enabled,sizeof(enabled));
        UInt32 algorithm=kAudioQueueTimePitchAlgorithm_Varispeed;
        AudioQueueSetProperty(c->audio,kAudioQueueProperty_TimePitchAlgorithm,&algorithm,sizeof(algorithm));
    }
    AudioQueueBufferRef buffer=NULL;
    status=AudioQueueAllocateBuffer(c->audio,(UInt32)size,&buffer);
    if(status)return (int)status;
    memcpy(buffer->mAudioData,(void *)(uintptr_t)data,(size_t)size);buffer->mAudioDataByteSize=(UInt32)size;
    buffer->mUserData=(void *)(uintptr_t)c->generation;
    c->rate=u32(h+8);
    status=AudioQueueEnqueueBuffer(c->audio,buffer,0,NULL);
    if(status){AudioQueueFreeBuffer(c->audio,buffer);return (int)status;}
    if (queued) { *queued = buffer; ++c->queued_buffers; }
    else ++c->immediate_buffers;
    c->playing=true;
    playback_rate(c);
    return (int)status;
}
static int command(struct sound_channel *c,struct command32 cmd) {
    switch(cmd.command & 0x7fff) {
    case 0: return 0;
    case 3:
        ++c->generation;
        if(c->audio)AudioQueueStop(c->audio,true);
        c->queued_buffers=c->immediate_buffers=0;c->playing=false;
        for(struct pending *p=c->head;p;p=p->next) {
            if(p->submitted){p->buffer=NULL;p->done=true;}
        }
        return 0;
    case 4:
        /* Queued buffers are already in Core Audio, so flushing must cancel
           them there too. Completion jobs carry a generation, not item pointers. */
        ++c->generation;
        if(c->audio)AudioQueueReset(c->audio);
        c->queued_buffers=c->immediate_buffers=0;c->playing=false;
        while(c->head){struct pending *p=c->head;c->head=p->next;free(p);}c->tail=NULL;return 0;
    case 11:c->paused=true;if(c->audio)AudioQueuePause(c->audio);return 0;
    case 12:c->paused=false;playback_rate(c);return 0;
    case 13:
        if(c->callback) {
            uint32_t record=compat_runtime32_allocate(sizeof(cmd),0);
            if(!record)return -108;
            memcpy((void *)(uintptr_t)record,&cmd,sizeof(cmd));
            uint32_t args[]={c->guest,record};compat_runtime32_call(c->callback,args,2);
            compat_runtime32_deallocate(record);
        }return 0;
    case 43:c->volume=(uint16_t)cmd.param1*0x10001u;volume(c);return 0;
    case 46:c->volume=cmd.param2;volume(c);return 0;
    case 47:if(cmd.param2)memcpy((void *)(uintptr_t)cmd.param2,&c->volume,4);return 0;
    case 81:return play(c,cmd.param2,NULL);
    case 82:c->rate=cmd.param2;playback_rate(c);return 0;
    case 85:if(cmd.param2)memcpy((void *)(uintptr_t)cmd.param2,&c->rate,4);return 0;
    case 86:if((int32_t)cmd.param2<0)return -50;c->multiplier=cmd.param2;playback_rate(c);return 0;
    case 87:if(cmd.param2)memcpy((void *)(uintptr_t)cmd.param2,&c->multiplier,4);return 0;
    default:fprintf(stderr,"compat32: unsupported Sound Manager command %u\n",cmd.command);return -50;
    }
}
/* Bink queues a buffer/callback pair for every ~40 ms of movie audio.
   Submit upcoming PCM immediately; waiting for the previous completion before
   enqueueing the next buffer leaves the hardware queue empty between blocks.
   Guest callbacks still execute in command order, after their preceding PCM. */
static void prime_buffers(struct sound_channel *c) {
    for (struct pending *p=c->head; p; p=p->next) {
        unsigned code=p->command.command & 0x7fff;
        if (code!=81 && code!=13 && code!=0) break;
        if (code!=81 || p->submitted) continue;
        int status=play(c,p->command.param2,&p->buffer);
        if (status==1) break; /* wait for a format change barrier */
        p->submitted=true;
        if(status) {
            p->done=true;
            fprintf(stderr,"compat32: Sound Manager command 81 failed: %d\n",status);
        }
    }
}
static void advance(struct sound_channel *c) {
    if(c->advancing || c->closed)return;
    c->advancing=true;
    prime_buffers(c);
    while(!c->closed && !c->paused && !c->immediate_buffers && c->head) {
        struct pending *p=c->head;
        unsigned code=p->command.command & 0x7fff;
        if(code==81 && !p->done)break;
        c->head=p->next;if(!c->head)c->tail=NULL;
        struct command32 value=p->command;free(p);
        int status=code==81 ? 0 : command(c,value);
        if(status)fprintf(stderr,"compat32: Sound Manager command %u failed: %d\n",value.command,status);
        prime_buffers(c);
    }
    c->advancing=false;
}

int sound_manager_bridge32_dispatch(const char *name,const uint32_t *a,uint64_t *out) {
    /* UPPs stay guest function addresses; callbacks enter via call32. */
    if(!strcmp(name,"_NewSndCallBackUPP")) { *out=a[0];return 1; }
    if(!strcmp(name,"_DisposeSndCallBackUPP")) { *out=0;return 1; }
    if(strncmp(name,"_Snd",4))return 0;
    if(!strcmp(name,"_SndNewChannel")) {
        if(!a[0] || a[1]!=5){*out=(uint32_t)-50;return 1;}
        pthread_mutex_lock(&channel_lock);
        struct sound_channel *c=channel_count<1024?&channels[channel_count++]:NULL;
        pthread_mutex_unlock(&channel_lock);
        if(!c){*out=(uint32_t)-108;return 1;}
        c->guest=*(uint32_t *)(uintptr_t)a[0];c->owns_guest=!c->guest;
        if(c->owns_guest)c->guest=compat_runtime32_allocate(1060,1);
        c->callback=a[3];c->volume=0x1000100;c->rate=c->multiplier=65536;
        if(!c->guest){*out=(uint32_t)-108;return 1;}
        uint32_t *guest=(void *)(uintptr_t)c->guest;guest[2]=a[3];((uint16_t *)guest)[15]=128;
        c->worker=dispatch_queue_create("compat32.sound-channel",DISPATCH_QUEUE_SERIAL);
        dispatch_queue_set_specific(c->worker,c,c,NULL);
        *(uint32_t *)(uintptr_t)a[0]=c->guest;*out=0;return 1;
    }
    if(strcmp(name,"_SndDisposeChannel") && strcmp(name,"_SndDoCommand") && strcmp(name,"_SndDoImmediate") && strcmp(name,"_SndChannelStatus"))return 0;
    pthread_mutex_lock(&channel_lock);struct sound_channel *c=NULL;
    for(unsigned i=0;i<channel_count;++i)if(channels[i].guest==a[0]&&!channels[i].closed){c=&channels[i];break;}
    pthread_mutex_unlock(&channel_lock);
    if(!c){*out=(uint32_t)-205;return 1;}
    if(!strcmp(name,"_SndChannelStatus")) {
        if(!a[2] || (int16_t)a[1]<24){*out=(uint32_t)-50;return 1;}
        dispatch_block_t query=^{
            unsigned char status[24]={0};
            status[12]=c->playing || c->head!=NULL;
            status[13]=c->closed;status[14]=c->paused;
            memcpy((void *)(uintptr_t)a[2],status,sizeof(status));
        };
        if(dispatch_get_specific(c))query();else dispatch_sync(c->worker,query);
        *out=0;return 1;
    }
    bool dispose=!strcmp(name,"_SndDisposeChannel"),immediate=!strcmp(name,"_SndDoImmediate");
    struct command32 value={0};if(!dispose){if(!a[1]){*out=(uint32_t)-50;return 1;}memcpy(&value,(void *)(uintptr_t)a[1],8);}
    __block int status=0;
    dispatch_block_t work=^{
        if(c->closed){status=-205;return;}
        if(dispose) {
            c->closed=true;
            if(c->audio){AudioQueueDispose(c->audio,true);c->audio=NULL;}
            while(c->head){struct pending *p=c->head;c->head=p->next;free(p);}c->tail=NULL;
            if(c->owns_guest)compat_runtime32_deallocate(c->guest);
        } else if(immediate) {
            status=command(c,value);
            /* quietCmd is commonly followed by flushCmd during Bink teardown.
               Do not run canceled refill callbacks in that interval. */
            if(!status && (value.command & 0x7fff)!=3)advance(c);
        }
        else {
            struct pending *p=calloc(1,sizeof(*p));if(!p){status=-108;return;}
            p->command=value;if(c->tail)c->tail->next=p;else c->head=p;c->tail=p;advance(c);
        }
    };
    if (!dispose && !immediate) dispatch_async(c->worker,work);
    else if(dispatch_get_specific(c))work();else dispatch_sync(c->worker,work);
    *out=(uint32_t)status;return 1;
}
