/* Audio Queue Services for i386 guests (MW2's Miles and Bink CoreAudio
 * drivers). Guest queues are small handles; guest buffers are i386
 * AudioQueueBuffer records in guest memory, each shadowed by a native buffer.
 * Output callbacks run on the native queue thread and call straight into the
 * guest, which refills and re-enqueues the guest buffer. */
#include "audio_queue_bridge.h"
#include "compat_runtime.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <mach/kern_return.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Each movie and audio reinitialization makes a queue; slots are never
   reused (see below), so allow for long sessions. */
enum { kQueueHandleBase = 0x51a00000, kMaxQueues = 4096, kMaxBuffers = 64 };

struct guest_buffer32 {
    uint32_t capacity, data, byte_size, user_data;
    uint32_t packet_capacity, packets, packet_count;
};

struct queue {
    AudioQueueRef audio;
    uint32_t handle, callback, user_data;
    /* Set, under lock, before the native queue is disposed. Callbacks still
       arriving from the queue thread then neither reach the guest nor copy
       into native buffers the disposal may already have freed. */
    bool disposing;
    unsigned buffer_count;
    struct { uint32_t guest; AudioQueueBufferRef host; } buffers[kMaxBuffers];
};

/* Slots are never reused: a late native callback may still carry the address. */
static struct queue queues[kMaxQueues];
static unsigned queue_count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static struct queue *queue_for(uint32_t handle)
{
    if (handle <= kQueueHandleBase || handle - kQueueHandleBase > queue_count) return NULL;
    struct queue *q = &queues[handle - kQueueHandleBase - 1];
    pthread_mutex_lock(&lock);
    bool live = q->audio && !q->disposing;
    pthread_mutex_unlock(&lock);
    return live ? q : NULL;
}

static AudioQueueBufferRef host_buffer(struct queue *q, uint32_t guest)
{
    for (unsigned i = 0; i < q->buffer_count; ++i)
        if (q->buffers[i].guest == guest) return q->buffers[i].host;
    return NULL;
}

static void output(void *raw, AudioQueueRef audio, AudioQueueBufferRef buffer)
{
    (void)audio;
    struct queue *q = raw;
    pthread_mutex_lock(&lock);
    bool live = q->audio && !q->disposing;
    pthread_mutex_unlock(&lock);
    if (!live) return;
    uint32_t arguments[] = {q->user_data, q->handle, (uint32_t)(uintptr_t)buffer->mUserData};
    if (getenv("LP32_TRACE_AUDIO_QUEUE")) {
        static unsigned calls;
        if (++calls % 200 == 1) fprintf(stderr, "compat32: AudioQueue output callback %u queue=%08x\n", calls, q->handle);
    }
    if (q->callback) compat_runtime32_call(q->callback, arguments, 3);
}

static OSStatus new_output(const uint32_t *a)
{
    const AudioStreamBasicDescription *format = (const void *)(uintptr_t)a[0];
    uint32_t *out = (void *)(uintptr_t)a[6];
    if (!format || !out) return kAudio_ParamError;
    pthread_mutex_lock(&lock);
    if (queue_count == kMaxQueues) { pthread_mutex_unlock(&lock); return kAudio_MemFullError; }
    struct queue *q = &queues[queue_count];
    q->handle = kQueueHandleBase + ++queue_count;
    pthread_mutex_unlock(&lock);
    q->callback = a[1];
    q->user_data = a[2];
    /* The guest's run loop is not a native CFRunLoopRef; use the queue's own thread. */
    OSStatus status = AudioQueueNewOutput(format, output, q, NULL, NULL, 0, &q->audio);
    if (status) return status;
    if (getenv("LP32_MUTE_AUDIO")) AudioQueueSetParameter(q->audio, kAudioQueueParam_Volume, 0);
    *out = q->handle;
    return noErr;
}

static OSStatus allocate_buffer(struct queue *q, uint32_t capacity, uint32_t *out)
{
    if (!out) return kAudio_ParamError;
    if (q->buffer_count == kMaxBuffers) return kAudio_MemFullError;
    uint32_t guest = compat_runtime32_allocate(sizeof(struct guest_buffer32) + capacity, 1);
    if (!guest) return kAudio_MemFullError;
    AudioQueueBufferRef host = NULL;
    OSStatus status = AudioQueueAllocateBuffer(q->audio, capacity, &host);
    if (status) { compat_runtime32_deallocate(guest); return status; }
    host->mUserData = (void *)(uintptr_t)guest;
    struct guest_buffer32 *record = (void *)(uintptr_t)guest;
    record->capacity = capacity;
    record->data = guest + sizeof(*record);
    pthread_mutex_lock(&lock);
    bool stored = !q->disposing && q->buffer_count < kMaxBuffers;
    if (stored) {
        q->buffers[q->buffer_count].guest = guest;
        q->buffers[q->buffer_count++].host = host;
    }
    pthread_mutex_unlock(&lock);
    if (!stored) { compat_runtime32_deallocate(guest); return kAudio_MemFullError; }
    *out = guest;
    return noErr;
}

/* The copy and native enqueue run under lock so that a disposal on another
   thread cannot free the native buffer between them. */
static OSStatus enqueue(struct queue *q, const uint32_t *a)
{
    pthread_mutex_lock(&lock);
    OSStatus status;
    AudioQueueBufferRef host = q->audio && !q->disposing ? host_buffer(q, a[1]) : NULL;
    const struct guest_buffer32 *record = (const void *)(uintptr_t)a[1];
    if (!host) {
        status = kAudioQueueErr_InvalidBuffer;
    } else if (record->byte_size > host->mAudioDataBytesCapacity) {
        status = kAudioQueueErr_BufferEmpty;
    } else {
        memcpy(host->mAudioData, (const void *)(uintptr_t)record->data, record->byte_size);
        host->mAudioDataByteSize = record->byte_size;
        /* AudioStreamPacketDescription has the same 16-byte layout on i386. */
        status = AudioQueueEnqueueBuffer(q->audio, host, a[2], (const void *)(uintptr_t)a[3]);
    }
    pthread_mutex_unlock(&lock);
    return status;
}

static void dispose(struct queue *q, bool immediate)
{
    pthread_mutex_lock(&lock);
    AudioQueueRef audio = q->disposing ? NULL : q->audio;
    q->disposing = true;
    pthread_mutex_unlock(&lock);
    if (!audio) return;
    /* Not under lock: disposal waits for a running output callback, which
       may itself be in enqueue(). */
    AudioQueueDispose(audio, immediate);
    pthread_mutex_lock(&lock);
    q->audio = NULL;
    unsigned count = q->buffer_count;
    q->buffer_count = 0;
    pthread_mutex_unlock(&lock);
    for (unsigned i = 0; i < count; ++i) compat_runtime32_deallocate(q->buffers[i].guest);
}

int audio_queue_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *result)
{
    if (!strcmp(name, "_AudioObjectAddPropertyListener") ||
        !strcmp(name, "_AudioObjectRemovePropertyListener")) {
        /* Device-change notifications are optional; the default device follows the host. */
        *result = noErr;
        return 1;
    }
    if (!strcmp(name, "_thread_policy_set")) {
        /* Miles requests time-constraint scheduling for its mixer thread. */
        *result = KERN_SUCCESS;
        return 1;
    }
    if (strncmp(name, "_AudioQueue", 11)) return 0;
    if (!strcmp(name, "_AudioQueueNewOutput")) {
        *result = (uint32_t)new_output(a);
        if (getenv("LP32_TRACE_AUDIO_QUEUE")) {
            const AudioStreamBasicDescription *f = (const void *)(uintptr_t)a[0];
            fprintf(stderr, "compat32: AudioQueueNewOutput rate=%.0f format=%.4s flags=%x ch=%u bits=%u -> %d\n",
                    f->mSampleRate, (const char *)&f->mFormatID, f->mFormatFlags,
                    f->mChannelsPerFrame, f->mBitsPerChannel, (int)*result);
        }
        return 1;
    }
    if (!strcmp(name, "_AudioQueueNewInput")) { *result = (uint32_t)kAudioQueueErr_InvalidDevice; return 1; }
    struct queue *q = queue_for(a[0]);
    if (!q) { *result = (uint32_t)kAudioQueueErr_InvalidQueueType; return 1; }
    OSStatus status;
    if (!strcmp(name, "_AudioQueueAllocateBuffer")) status = allocate_buffer(q, a[1], (void *)(uintptr_t)a[2]);
    else if (!strcmp(name, "_AudioQueueEnqueueBuffer")) status = enqueue(q, a);
    else if (!strcmp(name, "_AudioQueueStart")) status = AudioQueueStart(q->audio, (const void *)(uintptr_t)a[1]);
    else if (!strcmp(name, "_AudioQueueStop")) status = AudioQueueStop(q->audio, a[1] != 0);
    else if (!strcmp(name, "_AudioQueuePause")) status = AudioQueuePause(q->audio);
    else if (!strcmp(name, "_AudioQueueDispose")) { dispose(q, a[1] != 0); status = noErr; }
    else if (!strcmp(name, "_AudioQueueGetProperty") || !strcmp(name, "_AudioQueueSetProperty")) {
        /* Channel layouts, stream formats and codec settings are made of
           4-byte fields and match on i386. Object-valued properties do not. */
        if (a[1] == kAudioQueueProperty_CurrentDevice) status = kAudioQueueErr_InvalidPropertyValue;
        else if (name[11] == 'G')
            status = AudioQueueGetProperty(q->audio, a[1], (void *)(uintptr_t)a[2], (UInt32 *)(uintptr_t)a[3]);
        else status = AudioQueueSetProperty(q->audio, a[1], (const void *)(uintptr_t)a[2], a[3]);
    } else if (!strcmp(name, "_AudioQueueSetParameter")) {
        float value;
        memcpy(&value, &a[2], sizeof(value));
        if (a[1] == kAudioQueueParam_Volume && getenv("LP32_MUTE_AUDIO")) value = 0;
        status = AudioQueueSetParameter(q->audio, a[1], value);
    } else return 0;
    if (getenv("LP32_TRACE_AUDIO_QUEUE"))
        fprintf(stderr, "compat32: %s queue=%08x -> %d\n", name + 1, a[0], (int)status);
    *result = (uint32_t)status;
    return 1;
}
