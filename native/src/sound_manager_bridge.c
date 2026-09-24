/* Removed Carbon Sound Manager channels, mixed in process.
 *
 * Every channel used to own an AudioQueue.  A burst of effects (breaking a
 * cantina chair drops dozens of studs) started, stopped and recreated many
 * queues at once.  AudioQueue serializes those calls through one connection
 * to the system audio service: stops took 95-515 ms, and the music channel's
 * buffer enqueues blocked behind them for up to 435 ms, so the music broke up.
 *
 * Now one output unit mixes all channels.  The render callback reads guest
 * PCM just in time, so streaming clients may keep writing the later part of
 * a buffer during playback.  Starting, stopping and pausing a sound only
 * changes mixer state.  The guest owns i386 records; guest callbacks run on
 * the channel's serial worker, never on the audio thread. */
#include "sound_manager_bridge.h"
#include "compat_runtime.h"
#include <AudioToolbox/AudioToolbox.h>
#include <dispatch/dispatch.h>
#include <math.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#pragma pack(push, 2)
struct command32 { uint16_t command; int16_t param1; uint32_t param2; };
#pragma pack(pop)
struct pending { struct command32 command; struct pending *next; };
struct pcm_stream {
    uint32_t data, frames, bytes_per_frame, rate;
    unsigned bits, channels;
    bool big_endian, gapless, notified;
    double position; /* source frames consumed */
    /* After an early completion, frames from tail_from on come from a copy,
       because the guest may already be refilling its buffer. */
    const unsigned char *tail;
    uint32_t tail_from;
    struct pcm_stream *next;
};
struct sound_channel {
    struct sound_channel *next_active;
    uint32_t guest, callback;
    atomic_bool closed;
    bool owns_guest, muted;
    bool playing; /* worker only: a play command is in progress */
    dispatch_queue_t worker;
    struct pending *head, *tail; /* worker only */
    /* Mixer state, guarded by mixer_lock. */
    uint32_t volume, rate, multiplier, generation;
    bool paused, mixing;
    struct pcm_stream *streams, *streams_tail, *retired;
    unsigned char *tail_buffer;
    atomic_uint event;
    uint32_t event_generation;
};

enum { kMixCapacity = 1024, kTailCapacity = 96 * 1024 };
/* Long segments report completion this far ahead of their end, so the
   guest's callback can queue the next segment before the mixer needs it. */
static const double kGaplessLeadSeconds = 0.1;

static struct sound_channel *active_channels;
static pthread_mutex_t channel_lock = PTHREAD_MUTEX_INITIALIZER;
static os_unfair_lock mixer_lock = OS_UNFAIR_LOCK_INIT;
static struct sound_channel *mix_channels[kMixCapacity];
static unsigned mix_count;
static double output_rate = 48000;
static dispatch_semaphore_t event_semaphore;
static bool test_output;
static pthread_once_t output_once = PTHREAD_ONCE_INIT;

static void advance(struct sound_channel *);
static void trace_sound(struct sound_channel *c,const char *format,...) {
    if(!getenv("LP32_TRACE_SOUND"))return;
    static atomic_uint lines;
    if(atomic_fetch_add(&lines,1)>=2048)return;
    char detail[256];va_list ap;va_start(ap,format);vsnprintf(detail,sizeof(detail),format,ap);va_end(ap);
    fprintf(stderr,"compat32: sound t=%.6f channel=%08x generation=%u %s\n",
        clock_gettime_nsec_np(CLOCK_UPTIME_RAW)/1e9,c->guest,c->generation,detail);
}
static uint32_t u32(const void *p) { uint32_t n; memcpy(&n,p,4);return n; }
static uint16_t u16(const void *p) { uint16_t n; memcpy(&n,p,2);return n; }

/* ---- Mixer (audio thread) ---- */

static float decode(const unsigned char *p,unsigned bits,bool big) {
    switch(bits) {
    case 8: return ((int)p[0]-128)/128.0f;
    case 16: {
        int16_t v=(int16_t)(big?(p[0]<<8|p[1]):(p[1]<<8|p[0]));
        return v/32768.0f;
    }
    case 24: {
        int32_t v=big?(p[0]<<16|p[1]<<8|p[2]):(p[2]<<16|p[1]<<8|p[0]);
        if(v&0x800000)v-=0x1000000;
        return v/8388608.0f;
    }
    default: {
        uint32_t v=big?((uint32_t)p[0]<<24|p[1]<<16|p[2]<<8|p[3]):
                       ((uint32_t)p[3]<<24|p[2]<<16|p[1]<<8|p[0]);
        return (int32_t)v/2147483648.0f;
    }
    }
}
static const unsigned char *frame_at(const struct pcm_stream *s,uint32_t frame) {
    if(s->tail && frame>=s->tail_from)
        return s->tail+(size_t)(frame-s->tail_from)*s->bytes_per_frame;
    return (const unsigned char *)(uintptr_t)(s->data+(size_t)frame*s->bytes_per_frame);
}
static void read_frame(const struct pcm_stream *s,uint32_t frame,float *left,float *right) {
    const unsigned char *p=frame_at(s,frame);
    unsigned width=s->bits/8;
    *left=decode(p,s->bits,s->big_endian);
    *right=s->channels>1?decode(p+width,s->bits,s->big_endian):*left;
}
static void signal_event(struct sound_channel *c,bool *any) {
    c->event_generation=c->generation;
    atomic_store_explicit(&c->event,1,memory_order_release);
    *any=true;
}
static void mix_channel(struct sound_channel *c,float *left,float *right,uint32_t frames,bool *any) {
    if(!c->mixing || c->paused || !c->streams)return;
    float gain_left=c->muted?0:(c->volume&0xffff)/256.0f;
    float gain_right=c->muted?0:(c->volume>>16)/256.0f;
    for(uint32_t i=0;i<frames;++i) {
        struct pcm_stream *s=c->streams;
        if(!s)break;
        double step=c->rate/65536.0*(c->multiplier/65536.0)/output_rate;
        if(!(step>0))break;
        uint32_t frame=(uint32_t)s->position;
        double fraction=s->position-frame;
        float l0,r0,l1,r1;
        read_frame(s,frame,&l0,&r0);
        if(fraction>0 && frame+1<s->frames) {
            read_frame(s,frame+1,&l1,&r1);
            l0+=(l1-l0)*(float)fraction;r0+=(r1-r0)*(float)fraction;
        }
        left[i]+=l0*gain_left;right[i]+=r0*gain_right;
        s->position+=step;
        if(s->gapless && !s->notified && !s->next) {
            double remaining=s->frames-s->position;
            double lead=kGaplessLeadSeconds*output_rate*step;
            uint32_t from=(uint32_t)s->position;
            size_t bytes=(size_t)(s->frames-from)*s->bytes_per_frame;
            if(remaining<=lead && c->tail_buffer && bytes<=kTailCapacity) {
                memcpy(c->tail_buffer,(const void *)(uintptr_t)(s->data+(size_t)from*s->bytes_per_frame),bytes);
                s->tail=c->tail_buffer;s->tail_from=from;s->notified=true;
                signal_event(c,any);
            }
        }
        if(s->position>=s->frames) {
            double carry=s->position-s->frames;
            c->streams=s->next;if(!c->streams)c->streams_tail=NULL;
            s->next=c->retired;c->retired=s;
            /* Like bufferCmd, each buffer plays at its own header rate
               unless a later rateCmd changes it. */
            if(c->streams){c->streams->position=carry;c->rate=c->streams->rate;}
            else if(!s->notified)signal_event(c,any);
        }
    }
}
static void mix(float *left,float *right,uint32_t frames) {
    memset(left,0,frames*sizeof(float));memset(right,0,frames*sizeof(float));
    bool any=false;
    os_unfair_lock_lock(&mixer_lock);
    for(unsigned i=0;i<mix_count;++i)mix_channel(mix_channels[i],left,right,frames,&any);
    os_unfair_lock_unlock(&mixer_lock);
    for(uint32_t i=0;i<frames;++i) {
        left[i]=fminf(1.0f,fmaxf(-1.0f,left[i]));
        right[i]=fminf(1.0f,fmaxf(-1.0f,right[i]));
    }
    if(any && event_semaphore)dispatch_semaphore_signal(event_semaphore);
}
static OSStatus render(void *context,AudioUnitRenderActionFlags *flags,
                       const AudioTimeStamp *time,UInt32 bus,UInt32 frames,
                       AudioBufferList *io) {
    (void)context;(void)flags;(void)time;(void)bus;
    if(io->mNumberBuffers<2)return noErr;
    mix(io->mBuffers[0].mData,io->mBuffers[1].mData,frames);
    return noErr;
}

/* ---- Completion events (worker) ---- */

static void free_retired(struct sound_channel *c) {
    os_unfair_lock_lock(&mixer_lock);
    struct pcm_stream *list=c->retired;c->retired=NULL;
    os_unfair_lock_unlock(&mixer_lock);
    while(list){struct pcm_stream *next=list->next;free(list);list=next;}
}
/* A channel is done when nothing unreported remains in its stream list. */
static void on_event(struct sound_channel *c,uint32_t generation) {
    free_retired(c);
    if(c->closed)return;
    os_unfair_lock_lock(&mixer_lock);
    bool done=generation==c->generation;
    for(struct pcm_stream *s=c->streams;s&&done;s=s->next)if(!s->notified)done=false;
    if(done && !c->streams)c->mixing=false;
    os_unfair_lock_unlock(&mixer_lock);
    if(!done || !c->playing)return;
    trace_sound(c,"stream-complete");
    c->playing=false;
    advance(c);
}
static void dispatch_events(bool synchronous) {
    struct sound_channel *ready[kMixCapacity];uint32_t generations[kMixCapacity];unsigned count=0;
    os_unfair_lock_lock(&mixer_lock);
    for(unsigned i=0;i<mix_count;++i) {
        struct sound_channel *c=mix_channels[i];
        if(atomic_exchange_explicit(&c->event,0,memory_order_acquire)) {
            ready[count]=c;generations[count++]=c->event_generation;
        }
    }
    os_unfair_lock_unlock(&mixer_lock);
    for(unsigned i=0;i<count;++i) {
        struct sound_channel *c=ready[i];uint32_t generation=generations[i];
        dispatch_block_t work=^{on_event(c,generation);};
        if(synchronous)dispatch_sync(c->worker,work);else dispatch_async(c->worker,work);
    }
}
static void *event_loop(void *unused) {
    (void)unused;
    for(;;) {
        dispatch_semaphore_wait(event_semaphore,DISPATCH_TIME_FOREVER);
        dispatch_events(false);
    }
    return NULL;
}

/* ---- Output ---- */

static void *null_output(void *unused) {
    (void)unused;
    enum { kFrames = 512 };
    static float left[kFrames], right[kFrames];
    for(;;) {
        mix(left,right,kFrames);
        usleep((useconds_t)(kFrames/output_rate*1e6));
    }
    return NULL;
}
static bool start_audio_unit(void) {
    AudioComponentDescription description={kAudioUnitType_Output,kAudioUnitSubType_DefaultOutput,
                                           kAudioUnitManufacturer_Apple,0,0};
    AudioComponent component=AudioComponentFindNext(NULL,&description);
    AudioUnit unit=NULL;
    if(!component || AudioComponentInstanceNew(component,&unit))return false;
    AudioStreamBasicDescription device={0};UInt32 size=sizeof(device);
    if(!AudioUnitGetProperty(unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,0,&device,&size) &&
       device.mSampleRate>=8000)output_rate=device.mSampleRate;
    AudioStreamBasicDescription format={.mSampleRate=output_rate,.mFormatID=kAudioFormatLinearPCM,
        .mFormatFlags=kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked|kAudioFormatFlagIsNonInterleaved,
        .mBytesPerPacket=4,.mFramesPerPacket=1,.mBytesPerFrame=4,.mChannelsPerFrame=2,.mBitsPerChannel=32};
    AURenderCallbackStruct callback={render,NULL};
    if(AudioUnitSetProperty(unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&format,sizeof(format)) ||
       AudioUnitSetProperty(unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&callback,sizeof(callback)) ||
       AudioUnitInitialize(unit) || AudioOutputUnitStart(unit)) {
        AudioComponentInstanceDispose(unit);return false;
    }
    return true;
}
static void start_output(void) {
    event_semaphore=dispatch_semaphore_create(0);
    if(test_output)return;
    pthread_t thread;
    pthread_create(&thread,NULL,event_loop,NULL);pthread_detach(thread);
    if(!start_audio_unit()) {
        /* Without an output device, keep time so completions still arrive. */
        fprintf(stderr,"compat32: Sound Manager output unavailable; mixing to silence\n");
        pthread_create(&thread,NULL,null_output,NULL);pthread_detach(thread);
    }
}
void sound_manager_bridge32_use_test_output(double rate) {
    test_output=true;output_rate=rate;
    pthread_once(&output_once,start_output);
}
void sound_manager_bridge32_render(float *left,float *right,uint32_t frames) {
    mix(left,right,frames);
}
void sound_manager_bridge32_deliver_events(void) {
    dispatch_events(true);
}

/* ---- Commands (worker) ---- */

struct sound_data {
    uint32_t data, rate, frames, bits, channels;
    bool big_endian;
};
static int parse_sound_data(uint32_t address, struct sound_data *out) {
    if (!address) return -50;
    const unsigned char *h=(void *)(uintptr_t)address;
    uint32_t data=u32(h), count=u32(h+4), frames=count, bits=8, channels_count=1;
    double sample_rate=u32(h+8)/65536.0;
    unsigned offset=22; bool big_endian=false;
    if (h[20]==0xff || h[20]==0xfe) {
        channels_count=count;frames=u32(h+22);offset=64;
        bits=u16(h+(h[20]==0xff?48:62));
        /* ExtSoundHeader describes the i386 guest's native PCM, not the
           big-endian bytes of an on-disk AIFF resource. CmpSoundHeader below
           carries an explicit codec/byte order and must retain that order. */
        big_endian=false;
        if(h[20]==0xfe) {
            uint32_t format=u32(h+40);
            big_endian=bits>8;
            if(format==0x736f7774)big_endian=false; /* sowt */
            else if(format!=0x74776f73 && format!=0x4e4f4e45 && format!=0x72617720)return -206;
        }
    } else if(h[20]!=0) return -206;
    if(!data)data=address+offset;
    if(!channels_count || channels_count>8 || !frames || (bits!=8 && bits!=16 && bits!=24 && bits!=32) || sample_rate<1000 || sample_rate>384000)return -206;
    if((uint64_t)frames*channels_count*(bits/8)>UINT32_MAX)return -206;
    *out=(struct sound_data){.data=data,.rate=u32(h+8),.frames=frames,.bits=bits,
                             .channels=channels_count,.big_endian=big_endian};
    return 0;
}
static int play(struct sound_channel *c, uint32_t address) {
    struct sound_data sound;
    int check=parse_sound_data(address,&sound);if(check)return check;
    double seconds=sound.frames/(sound.rate/65536.0);
    trace_sound(c,"play header=%08x data=%08x frames=%u rate=%.0f seconds=%.6f",address,sound.data,sound.frames,sound.rate/65536.0,seconds);
    struct pcm_stream *stream=calloc(1,sizeof(*stream));if(!stream)return -108;
    *stream=(struct pcm_stream){.data=sound.data,.frames=sound.frames,
        .bytes_per_frame=sound.channels*(sound.bits/8),.rate=sound.rate,.bits=sound.bits,.channels=sound.channels,
        .big_endian=sound.big_endian,.gapless=seconds>=0.75};
    if(stream->gapless && !c->tail_buffer)c->tail_buffer=malloc(kTailCapacity);
    free_retired(c);
    os_unfair_lock_lock(&mixer_lock);
    if(c->streams_tail)c->streams_tail->next=stream;
    else{c->streams=stream;c->rate=sound.rate;}
    c->streams_tail=stream;c->mixing=true;
    os_unfair_lock_unlock(&mixer_lock);
    c->playing=true;
    return 0;
}
static void quiet(struct sound_channel *c) {
    os_unfair_lock_lock(&mixer_lock);
    ++c->generation;
    if(c->streams_tail){c->streams_tail->next=c->retired;c->retired=c->streams;}
    c->streams=c->streams_tail=NULL;c->mixing=false;
    os_unfair_lock_unlock(&mixer_lock);
    free_retired(c);
    c->playing=false;
}
static void set_mixer_word(uint32_t *field,uint32_t value) {
    os_unfair_lock_lock(&mixer_lock);*field=value;os_unfair_lock_unlock(&mixer_lock);
}
static void set_paused(struct sound_channel *c,bool paused) {
    os_unfair_lock_lock(&mixer_lock);c->paused=paused;os_unfair_lock_unlock(&mixer_lock);
}
static int command(struct sound_channel *c,struct command32 cmd) {
    trace_sound(c,"execute cmd=%u param1=%d param2=%08x",cmd.command,cmd.param1,cmd.param2);
    switch(cmd.command & 0x7fff) {
    case 0: return 0;
    case 3: quiet(c);return 0;
    case 4:
        while(c->head){struct pending *p=c->head;c->head=p->next;free(p);}c->tail=NULL;return 0;
    case 11:set_paused(c,true);return 0;
    case 12:set_paused(c,false);return 0;
    case 13:
        if(c->callback) {
            uint32_t record=compat_runtime32_allocate(sizeof(cmd),0);
            if(!record)return -108;
            memcpy((void *)(uintptr_t)record,&cmd,sizeof(cmd));
            uint32_t args[]={c->guest,record};compat_runtime32_call(c->callback,args,2);
            compat_runtime32_deallocate(record);
        }return 0;
    case 43:set_mixer_word(&c->volume,(uint16_t)cmd.param1*0x10001u);return 0;
    case 46:set_mixer_word(&c->volume,cmd.param2);return 0;
    case 47:if(cmd.param2)memcpy((void *)(uintptr_t)cmd.param2,&c->volume,4);return 0;
    case 81:return play(c,cmd.param2);
    case 82:set_mixer_word(&c->rate,cmd.param2);return 0;
    case 85:if(cmd.param2)memcpy((void *)(uintptr_t)cmd.param2,&c->rate,4);return 0;
    case 86:if((int32_t)cmd.param2<0)return -50;set_mixer_word(&c->multiplier,cmd.param2);return 0;
    case 87:if(cmd.param2)memcpy((void *)(uintptr_t)cmd.param2,&c->multiplier,4);return 0;
    default:fprintf(stderr,"compat32: unsupported Sound Manager command %u\n",cmd.command);return -50;
    }
}
static void advance(struct sound_channel *c) {
    while(!c->closed && !c->playing && c->head) {
        os_unfair_lock_lock(&mixer_lock);bool paused=c->paused;os_unfair_lock_unlock(&mixer_lock);
        if(paused)break;
        struct pending *p=c->head;c->head=p->next;if(!c->head)c->tail=NULL;
        struct command32 value=p->command;free(p);
        int status=command(c,value);
        if(status)fprintf(stderr,"compat32: Sound Manager command %u failed: %d\n",value.command,status);
    }
}
int sound_manager_bridge32_dispatch(const char *name,const uint32_t *a,uint64_t *out) {
    if(strncmp(name,"_Snd",4))return 0;
    if(!strcmp(name,"_SndNewChannel")) {
        if(!a[0] || a[1]!=5){*out=(uint32_t)-50;return 1;}
        pthread_once(&output_once,start_output);
        struct sound_channel *c=calloc(1,sizeof(*c));
        if(!c){
            fprintf(stderr,"compat32: Sound Manager channel allocation failed\n");
            *out=(uint32_t)-108;return 1;
        }
        atomic_init(&c->closed,false);atomic_init(&c->event,0);
        c->guest=*(uint32_t *)(uintptr_t)a[0];c->owns_guest=!c->guest;
        if(c->owns_guest)c->guest=compat_runtime32_allocate(1060,1);
        c->callback=a[3];c->volume=0x1000100;c->rate=c->multiplier=65536;
        c->muted=getenv("LP32_MUTE_AUDIO")!=NULL;
        if(!c->guest){free(c);*out=(uint32_t)-108;return 1;}
        uint32_t *guest=(void *)(uintptr_t)c->guest;guest[2]=a[3];((uint16_t *)guest)[15]=128;
        c->worker=dispatch_queue_create("compat32.sound-channel",DISPATCH_QUEUE_SERIAL);
        dispatch_queue_set_specific(c->worker,c,c,NULL);
        os_unfair_lock_lock(&mixer_lock);
        bool room=mix_count<kMixCapacity;
        if(room)mix_channels[mix_count++]=c;
        os_unfair_lock_unlock(&mixer_lock);
        if(!room) {
            if(c->owns_guest)compat_runtime32_deallocate(c->guest);
            dispatch_release(c->worker);free(c);
            *out=(uint32_t)-108;return 1;
        }
        pthread_mutex_lock(&channel_lock);
        c->next_active=active_channels;
        active_channels=c;
        pthread_mutex_unlock(&channel_lock);
        *(uint32_t *)(uintptr_t)a[0]=c->guest;*out=0;return 1;
    }
    if(strcmp(name,"_SndDisposeChannel") && strcmp(name,"_SndDoCommand") && strcmp(name,"_SndDoImmediate"))return 0;
    pthread_mutex_lock(&channel_lock);struct sound_channel *c=NULL;
    for(struct sound_channel *item=active_channels;item;item=item->next_active)
        if(item->guest==a[0]&&!item->closed){c=item;break;}
    pthread_mutex_unlock(&channel_lock);
    if(!c){*out=(uint32_t)-205;return 1;}
    bool dispose=!strcmp(name,"_SndDisposeChannel"),immediate=!strcmp(name,"_SndDoImmediate");
    struct command32 value={0};if(!dispose){if(!a[1]){*out=(uint32_t)-50;return 1;}memcpy(&value,(void *)(uintptr_t)a[1],8);}
    unsigned kind=value.command & 0x7fff;
    bool async_play=immediate && kind==81;
    bool async_setter=immediate && (kind==43 || kind==46 || kind==82 || kind==86);
    bool async_control=immediate && (kind==3 || kind==4 || kind==11 || kind==12);
    if(async_setter && kind==86 && (int32_t)value.param2<0) {
        *out=(uint32_t)-50;return 1;
    }
    if(async_play) {
        struct sound_data sound;
        int check=parse_sound_data(value.param2,&sound);
        if(check){*out=(uint32_t)check;return 1;}
    }
    if(dispose) {
        /* Closing now makes later calls fail and lets the guest reuse the
           record at once; the mixer stops reading its PCM immediately. Channel
           contexts stay allocated for late worker blocks. */
        if(atomic_exchange(&c->closed,true)){*out=(uint32_t)-205;return 1;}
        pthread_mutex_lock(&channel_lock);
        for(struct sound_channel **slot=&active_channels;*slot;
            slot=&(*slot)->next_active) {
            if(*slot==c){*slot=c->next_active;break;}
        }
        pthread_mutex_unlock(&channel_lock);
        os_unfair_lock_lock(&mixer_lock);
        for(unsigned i=0;i<mix_count;++i)
            if(mix_channels[i]==c){mix_channels[i]=mix_channels[--mix_count];break;}
        c->mixing=false;
        os_unfair_lock_unlock(&mixer_lock);
        dispatch_block_t teardown=^{
            quiet(c);
            while(c->head){struct pending *p=c->head;c->head=p->next;free(p);}c->tail=NULL;
            free(c->tail_buffer);c->tail_buffer=NULL;
            if(c->owns_guest)compat_runtime32_deallocate(c->guest);
        };
        if(dispatch_get_specific(c))teardown();else dispatch_async(c->worker,teardown);
        *out=0;return 1;
    }
    __block int status=0;
    dispatch_block_t work=^{
        if(c->closed){status=-205;return;}
        if(immediate) {
            status=command(c,value);
            if(status && (async_play || async_setter || async_control))
                fprintf(stderr,"compat32: Sound Manager command %u failed: %d\n",kind,status);
        }
        else {
            trace_sound(c,"enqueue cmd=%u param1=%d param2=%08x",value.command,value.param1,value.param2);
            struct pending *p=calloc(1,sizeof(*p));if(!p){status=-108;return;}
            p->command=value;if(c->tail)c->tail->next=p;else c->head=p;c->tail=p;advance(c);
        }
    };
    /* Playback, setters and controls queue in order on the channel worker;
       getters stay synchronous so a read after a write sees the new value. */
    if (!immediate || async_play || async_setter || async_control)
        dispatch_async(c->worker,work);
    else if(dispatch_get_specific(c))work();else dispatch_sync(c->worker,work);
    *out=(uint32_t)status;return 1;
}
