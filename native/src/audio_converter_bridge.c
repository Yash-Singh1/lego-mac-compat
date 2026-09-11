#include "audio_converter_bridge.h"
#include "compat_runtime.h"
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define CONVERTER_BASE 0x7f0a0000u
#define CONVERTER_COUNT 1024
#define BUFFER_COUNT 32
static AudioConverterRef converters[CONVERTER_COUNT];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
struct buffer32 { uint32_t channels, size, data; };
struct input_context { uint32_t token, function, user, scratch; };
static OSStatus input(AudioConverterRef converter, UInt32 *packets, AudioBufferList *data,
                       AudioStreamPacketDescription **descriptions, void *raw) {
    (void)converter;
    struct input_context *context=raw;
    if (!data || data->mNumberBuffers>BUFFER_COUNT) return kAudio_ParamError;
    uint32_t capacity=data->mNumberBuffers, requested=*packets;
    uint32_t *scratch=(void *)(uintptr_t)context->scratch;
    scratch[0]=requested;scratch[1]=0;scratch[2]=capacity;
    struct buffer32 *buffers=(void *)(scratch+3);
    for(uint32_t i=0;i<capacity;++i) buffers[i]=(struct buffer32){data->mBuffers[i].mNumberChannels,0,0};
    uint32_t args[]={context->token,context->scratch,context->scratch+8,
                     descriptions?context->scratch+4:0,context->user};
    OSStatus status=(OSStatus)compat_runtime32_call(context->function,args,5);
    if(scratch[2]>capacity || scratch[0]>requested)return kAudio_ParamError;
    *packets=scratch[0];data->mNumberBuffers=scratch[2];
    for(uint32_t i=0;i<scratch[2];++i){
        data->mBuffers[i].mNumberChannels=buffers[i].channels;
        data->mBuffers[i].mDataByteSize=buffers[i].size;
        data->mBuffers[i].mData=(void *)(uintptr_t)buffers[i].data;
    }
    if(descriptions)*descriptions=(void *)(uintptr_t)scratch[1];
    return status;
}
struct legacy_input_context { struct input_context guest; UInt32 packet_bytes, channels; };
static OSStatus input_legacy(AudioConverterRef converter, UInt32 *packets,
                            AudioBufferList *data, AudioStreamPacketDescription **descriptions, void *raw) {
    (void)converter;
    struct legacy_input_context *context=raw;
    if(!data || data->mNumberBuffers!=1 || *packets>UINT32_MAX/context->packet_bytes)return kAudio_ParamError;
    uint32_t *scratch=(void *)(uintptr_t)context->guest.scratch;
    scratch[0]=*packets*context->packet_bytes;scratch[1]=0;
    uint32_t args[]={context->guest.token,context->guest.scratch,context->guest.scratch+4,context->guest.user};
    OSStatus status=(OSStatus)compat_runtime32_call(context->guest.function,args,4);
    if(scratch[0]%context->packet_bytes || (scratch[0] && !scratch[1]))return kAudio_ParamError;
    *packets=scratch[0]/context->packet_bytes;
    data->mBuffers[0]=(AudioBuffer){context->channels,scratch[0],(void *)(uintptr_t)scratch[1]};
    if(descriptions)*descriptions=NULL;
    return status;
}
int audio_converter_bridge32_dispatch(const char *name,const uint32_t *a,uint64_t *out) {
    if(strncmp(name,"_AudioConverter",15))return 0;
    if(!strcmp(name,"_AudioConverterNew")){
        if(!a[0]||!a[1]||!a[2]){*out=(uint32_t)kAudio_ParamError;return 1;}
        AudioStreamBasicDescription source,destination;
        memcpy(&source,(void *)(uintptr_t)a[0],sizeof(source));
        memcpy(&destination,(void *)(uintptr_t)a[1],sizeof(destination));
        if (getenv("LP32_TRACE_CONVERTERS"))
            fprintf(stderr, "compat32: converter new src=%g/%08x/%x/%u/%u/%u/%u/%u dst=%g/%08x/%x/%u/%u/%u/%u/%u\n",
                source.mSampleRate, source.mFormatID, source.mFormatFlags, source.mBytesPerPacket, source.mFramesPerPacket, source.mBytesPerFrame, source.mChannelsPerFrame, source.mBitsPerChannel,
                destination.mSampleRate, destination.mFormatID, destination.mFormatFlags, destination.mBytesPerPacket, destination.mFramesPerPacket, destination.mBytesPerFrame, destination.mChannelsPerFrame, destination.mBitsPerChannel);
        AudioConverterRef converter=NULL;OSStatus status=AudioConverterNew(&source,&destination,&converter);
        *(uint32_t *)(uintptr_t)a[2]=0;
        if(!status && source.mFormatID==kAudioFormatLinearPCM &&
           destination.mFormatID==kAudioFormatLinearPCM &&
           source.mSampleRate!=destination.mSampleRate){
            /* The default macOS SRC can endlessly request one input packet
               when successive fills change size (COD4: 13,544 -> 44,100 Hz,
               470 then 471 frames). Minimum-phase SRC completes these fills
               while retaining filter history across audio callbacks. */
            UInt32 complexity=kAudioConverterSampleRateConverterComplexity_MinimumPhase;
            status=AudioConverterSetProperty(converter,kAudioConverterSampleRateConverterComplexity,
                                             sizeof(complexity),&complexity);
            if(status)AudioConverterDispose(converter);
        }
        if(!status){
            pthread_mutex_lock(&lock);unsigned slot;
            for(slot=0;slot<CONVERTER_COUNT && converters[slot];++slot){}
            if(slot<CONVERTER_COUNT){converters[slot]=converter;*(uint32_t *)(uintptr_t)a[2]=CONVERTER_BASE+16*slot;}
            pthread_mutex_unlock(&lock);
            if(slot==CONVERTER_COUNT){AudioConverterDispose(converter);status=kAudio_MemFullError;}
        }
        *out=(uint32_t)status;return 1;
    }
    if(strcmp(name,"_AudioConverterDispose") && strcmp(name,"_AudioConverterReset") &&
       strcmp(name,"_AudioConverterFillComplexBuffer") && strcmp(name,"_AudioConverterFillBuffer"))return 0;
    unsigned slot=(a[0]-CONVERTER_BASE)/16;
    pthread_mutex_lock(&lock);
    AudioConverterRef converter=a[0]>=CONVERTER_BASE && !(a[0]&15) && slot<CONVERTER_COUNT?converters[slot]:NULL;
    pthread_mutex_unlock(&lock);
    if(!converter){*out=(uint32_t)kAudio_ParamError;return 1;}
    if(!strcmp(name,"_AudioConverterReset")){*out=(uint32_t)AudioConverterReset(converter);return 1;}
    if(!strcmp(name,"_AudioConverterDispose")){
        OSStatus status=AudioConverterDispose(converter);
        if(!status){pthread_mutex_lock(&lock);converters[slot]=NULL;pthread_mutex_unlock(&lock);}
        *out=(uint32_t)status;return 1;
    }
    if(!a[1]||!a[3]||!a[4]){*out=(uint32_t)kAudio_ParamError;return 1;}
    if(!strcmp(name,"_AudioConverterFillBuffer")) {
        AudioStreamBasicDescription source,destination;UInt32 size=sizeof(source);
        OSStatus status=AudioConverterGetProperty(converter,kAudioConverterCurrentInputStreamDescription,&size,&source);
        size=sizeof(destination);
        if(!status)status=AudioConverterGetProperty(converter,kAudioConverterCurrentOutputStreamDescription,&size,&destination);
        if(status){*out=(uint32_t)status;return 1;}
        /* FillBuffer is a byte-count API for a single interleaved CBR buffer.
           Adapt it to the supported packet API; the host's deprecated entry
           point can terminate for modern converter implementations. */
        if(!source.mBytesPerPacket || !destination.mBytesPerPacket ||
           (source.mFormatFlags & kAudioFormatFlagIsNonInterleaved) ||
           (destination.mFormatFlags & kAudioFormatFlagIsNonInterleaved)) {
            *out=(uint32_t)kAudio_ParamError;return 1;
        }
        struct legacy_input_context context={{a[0],a[1],a[2],compat_runtime32_allocate(8,1)},source.mBytesPerPacket,source.mChannelsPerFrame};
        if(!context.guest.scratch){*out=(uint32_t)kAudio_MemFullError;return 1;}
        UInt32 *bytes=(void *)(uintptr_t)a[3],packets=*bytes/destination.mBytesPerPacket;
        AudioBufferList buffer={1,{{destination.mChannelsPerFrame,*bytes,(void *)(uintptr_t)a[4]}}};
        status=AudioConverterFillComplexBuffer(converter,input_legacy,&context,&packets,&buffer,NULL);
        *bytes=buffer.mBuffers[0].mDataByteSize;
        *out=(uint32_t)status;
        compat_runtime32_deallocate(context.guest.scratch);return 1;
    }
    uint32_t *list=(void *)(uintptr_t)a[4];uint32_t count=*list;
    if(!count||count>BUFFER_COUNT){*out=(uint32_t)kAudio_ParamError;return 1;}
    struct buffer32 *buffers=(void *)(list+1);
    AudioBufferList *host=calloc(1,offsetof(AudioBufferList,mBuffers)+count*sizeof(AudioBuffer));
    struct input_context context={a[0],a[1],a[2],compat_runtime32_allocate(12+BUFFER_COUNT*sizeof(struct buffer32),1)};
    if(!host||!context.scratch){free(host);if(context.scratch)compat_runtime32_deallocate(context.scratch);*out=(uint32_t)kAudio_MemFullError;return 1;}
    host->mNumberBuffers=count;
    for(uint32_t i=0;i<count;++i)host->mBuffers[i]=(AudioBuffer){buffers[i].channels,buffers[i].size,(void *)(uintptr_t)buffers[i].data};
    *out=(uint32_t)AudioConverterFillComplexBuffer(converter,input,&context,(UInt32 *)(uintptr_t)a[3],host,(void *)(uintptr_t)a[5]);
    if (host->mNumberBuffers > count) *out = (uint32_t)kAudio_ParamError;
    else {
        *list = host->mNumberBuffers;
        for(uint32_t i=0;i<host->mNumberBuffers;++i){buffers[i].channels=host->mBuffers[i].mNumberChannels;buffers[i].size=host->mBuffers[i].mDataByteSize;}
    }
    compat_runtime32_deallocate(context.scratch);free(host);return 1;
}
