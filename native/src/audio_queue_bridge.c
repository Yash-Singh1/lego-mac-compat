#include "audio_queue_bridge.h"
#include "compat_runtime.h"
#include "objc_bridge.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreServices/CoreServices.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
/* Queue contexts remain valid after disposal for any already queued native
 * notifications. Audio payloads and buffer records are freed at disposal. */
struct buffer32 { uint32_t capacity,data,size,user,packet_capacity,packets,packet_count; };
struct buffer {AudioQueueBufferRef native;uint32_t guest;struct buffer *next;};
struct listener {struct queue *queue;uint32_t property,function,user;struct listener *next;};
struct queue {AudioQueueRef native;uint32_t token,function,user;_Atomic int closed;int muted;struct buffer *buffers;struct listener *listeners;struct queue *next;};
static struct queue *queues;
static uint32_t next_token=0x7f090000;
static pthread_mutex_t queue_lock=PTHREAD_MUTEX_INITIALIZER;
static struct queue *lookup(uint32_t token){
    pthread_mutex_lock(&queue_lock);struct queue *q=queues;while(q&&q->token!=token)q=q->next;
    pthread_mutex_unlock(&queue_lock);return q&&!q->closed?q:NULL;
}
static struct buffer *buffer_for(struct queue *q,uint32_t token){struct buffer *b=q->buffers;while(b&&b->guest!=token)b=b->next;return b;}
static void output_callback(void *raw,AudioQueueRef native,AudioQueueBufferRef buffer){
    (void)native;struct queue *q=raw;pthread_mutex_lock(&queue_lock);
    struct buffer *b=q->buffers;while(b&&b->native!=buffer)b=b->next;
    uint32_t token=b?b->guest:0,function=q->closed?0:q->function;
    pthread_mutex_unlock(&queue_lock);
    if(token&&function){uint32_t a[]={q->user,q->token,token};compat_runtime32_call(function,a,3);}
}
static void property_callback(void *raw,AudioQueueRef native,AudioQueuePropertyID property){
    (void)native;struct listener *l=raw;struct queue *q=l->queue;
    if(!q->closed && l->function){
        uint32_t a[]={l->user,q->token,property};compat_runtime32_call(l->function,a,3);
    }
}
int audio_queue_bridge32_dispatch(const char *name,const uint32_t *a,uint64_t *out){
    if(strncmp(name,"_AudioQueue",11))return 0;
#define IS(s) (!strcmp(name,s))
#define P(i) ((void *)(uintptr_t)a[i])
    if(IS("_AudioQueueNewOutput")){
        if(!a[0]||!a[6]){*out=(uint32_t)paramErr;return 1;}
        struct queue *q=calloc(1,sizeof(*q));if(!q){*out=(uint32_t)memFullErr;return 1;}
        q->function=a[1];q->user=a[2];q->muted=getenv("LP32_MUTE_AUDIO")!=NULL;
        OSStatus status=AudioQueueNewOutput(P(0),output_callback,q,
            objc_bridge32_host_object(a[3]),objc_bridge32_host_object(a[4]),a[5],&q->native);
        if(status){free(q);*out=(uint32_t)status;return 1;}
        if(q->muted)AudioQueueSetParameter(q->native,kAudioQueueParam_Volume,0);
        pthread_mutex_lock(&queue_lock);q->token=next_token;next_token+=16;q->next=queues;queues=q;pthread_mutex_unlock(&queue_lock);
        *(uint32_t *)P(6)=q->token;*out=0;return 1;
    }
    struct queue *q=lookup(a[0]);if(!q){*out=(uint32_t)paramErr;return 1;}
    OSStatus status=0;
    if(IS("_AudioQueueAllocateBuffer")){
        if(!a[2]){*out=(uint32_t)paramErr;return 1;}
        struct buffer *b=calloc(1,sizeof(*b));if(!b){*out=(uint32_t)memFullErr;return 1;}
        status=AudioQueueAllocateBuffer(q->native,a[1],&b->native);
        if(!status){b->guest=compat_runtime32_allocate(sizeof(struct buffer32),1);
            uint32_t data=compat_runtime32_allocate(a[1]?a[1]:1,1);
            if(!b->guest||!data){if(b->guest)compat_runtime32_deallocate(b->guest);if(data)compat_runtime32_deallocate(data);AudioQueueFreeBuffer(q->native,b->native);status=memFullErr;}
            else {struct buffer32 *g=(void *)(uintptr_t)b->guest;g->capacity=a[1];g->data=data;
                pthread_mutex_lock(&queue_lock);b->next=q->buffers;q->buffers=b;pthread_mutex_unlock(&queue_lock);*(uint32_t *)P(2)=b->guest;}
        }
        if(status)free(b);
    } else if(IS("_AudioQueueFreeBuffer")){
        struct buffer *b=buffer_for(q,a[1]);if(!b)status=paramErr;
        else if(!(status=AudioQueueFreeBuffer(q->native,b->native))){
            pthread_mutex_lock(&queue_lock);struct buffer **p=&q->buffers;while(*p!=b)p=&(*p)->next;*p=b->next;pthread_mutex_unlock(&queue_lock);
            compat_runtime32_deallocate(((struct buffer32 *)(uintptr_t)b->guest)->data);compat_runtime32_deallocate(b->guest);free(b);}
    } else if(IS("_AudioQueueEnqueueBuffer")){
        struct buffer *b=buffer_for(q,a[1]);struct buffer32 *g=b?(void *)(uintptr_t)b->guest:NULL;
        if(!b||g->size>b->native->mAudioDataBytesCapacity)status=paramErr;
        else {memcpy(b->native->mAudioData,(void *)(uintptr_t)g->data,g->size);b->native->mAudioDataByteSize=g->size;
            status=AudioQueueEnqueueBuffer(q->native,b->native,a[2],P(3));}
    } else if(IS("_AudioQueueStart")){
        if(q->muted)AudioQueueSetParameter(q->native,kAudioQueueParam_Volume,0);
        status=AudioQueueStart(q->native,P(1));
    } else if(IS("_AudioQueuePause"))status=AudioQueuePause(q->native);
    else if(IS("_AudioQueueStop"))status=AudioQueueStop(q->native,a[1]!=0);
    else if(IS("_AudioQueueDispose")){
        status=AudioQueueDispose(q->native,a[1]!=0);
        if(!status){q->closed=1;while(q->buffers){struct buffer *b=q->buffers;q->buffers=b->next;
            compat_runtime32_deallocate(((struct buffer32 *)(uintptr_t)b->guest)->data);compat_runtime32_deallocate(b->guest);free(b);}}
    } else if(IS("_AudioQueueSetParameter")){
        float value;memcpy(&value,a+2,4);if(q->muted&&a[1]==kAudioQueueParam_Volume)value=0;
        status=AudioQueueSetParameter(q->native,a[1],value);
    } else if(IS("_AudioQueueGetParameter"))status=AudioQueueGetParameter(q->native,a[1],P(2));
    else if(IS("_AudioQueueGetProperty"))status=AudioQueueGetProperty(q->native,a[1],P(2),P(3));
    else if(IS("_AudioQueueGetPropertySize"))status=AudioQueueGetPropertySize(q->native,a[1],P(2));
    else if(IS("_AudioQueueSetProperty"))status=AudioQueueSetProperty(q->native,a[1],P(2),a[3]);
    else if(IS("_AudioQueueAddPropertyListener")){
        struct listener *l=calloc(1,sizeof(*l));if(!l)status=memFullErr;
        else {l->queue=q;l->property=a[1];l->function=a[2];l->user=a[3];
            status=AudioQueueAddPropertyListener(q->native,a[1],property_callback,l);
            if(status)free(l);
            else {pthread_mutex_lock(&queue_lock);l->next=q->listeners;q->listeners=l;pthread_mutex_unlock(&queue_lock);}}
    } else if(IS("_AudioQueueSetOfflineRenderFormat"))status=AudioQueueSetOfflineRenderFormat(q->native,P(1),P(2));
    else if(IS("_AudioQueueOfflineRender")){
        struct buffer *b=buffer_for(q,a[2]);if(!b)status=paramErr;
        else {status=AudioQueueOfflineRender(q->native,P(1),b->native,a[3]);if(!status){
            struct buffer32 *g=(void *)(uintptr_t)b->guest;g->size=b->native->mAudioDataByteSize;
            memcpy((void *)(uintptr_t)g->data,b->native->mAudioData,g->size);}}
    } else return 0;
    *out=(uint32_t)status;return 1;
#undef IS
#undef P
}
