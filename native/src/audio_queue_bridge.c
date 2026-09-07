#include "audio_queue_bridge.h"
#include "compat_runtime.h"
#include "objc_bridge.h"
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>

/* AudioQueueBuffer contains pointers, so neither it nor its native audio data
   can be exposed to i386. Each buffer has a low-memory mirror; enqueue copies
   the data into the buffer owned by Core Audio. */
struct buffer32 {
    uint32_t capacity, data, size, user_data;
    uint32_t packet_capacity, packets, packet_count;
};
_Static_assert(sizeof(struct buffer32) == 28, "i386 AudioQueueBuffer ABI");
struct queue_buffer {
    AudioQueueBufferRef host;
    uint32_t guest, data, packets;
    struct queue_buffer *next;
};
struct queue;
struct listener {
    struct queue *queue;
    uint32_t function, info, property;
    struct listener *next;
};
struct queue {
    bool used, disposing;
    AudioQueueRef host;
    uint32_t handle, function, info;
    struct queue_buffer *buffers;
    struct listener *listeners;
};
enum { QUEUE_BASE = 0xff800001u, QUEUE_COUNT = 64 };
static struct queue queues[QUEUE_COUNT];
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;

static bool trace_audio_queue(void)
{
    return getenv("LP32_TRACE_AUDIO") != NULL;
}

static void trace_queue_operation(const char *name, struct queue *q)
{
    if (!trace_audio_queue()) return;
    unsigned count = 0;
    size_t bytes = 0;
    pthread_mutex_lock(&queue_lock);
    for (struct queue_buffer *b = q->buffers; b; b = b->next) {
        ++count; bytes += b->host->mAudioDataBytesCapacity;
    }
    fprintf(stderr, "compat32: %s begin t=%.6f queue=0x%08x host=%p buffers=%u bytes=%zu\n",
        name + 1, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9, q->handle,
        (void *)q->host, count, bytes);
    pthread_mutex_unlock(&queue_lock);
    fflush(stderr);
}

static void output_ready(void *context, AudioQueueRef host, AudioQueueBufferRef buffer)
{
    (void)host;
    struct queue *q = context;
    uint32_t args[3] = {q->info, q->handle, 0}, function = 0;
    pthread_mutex_lock(&queue_lock);
    if (!q->disposing) {
        for (struct queue_buffer *b = q->buffers; b; b = b->next) {
            if (b->host != buffer) continue;
            struct buffer32 *g = (void *)(uintptr_t)b->guest;
            g->size = buffer->mAudioDataByteSize;
            g->packet_count = buffer->mPacketDescriptionCount;
            args[2] = b->guest;
            function = q->function;
            break;
        }
    }
    pthread_mutex_unlock(&queue_lock);
    if (function) compat_runtime32_call(function, args, 3);
}

static void property_changed(void *context, AudioQueueRef host, AudioQueuePropertyID property)
{
    (void)host;
    struct listener *l = context;
    pthread_mutex_lock(&queue_lock);
    uint32_t function = l->queue->disposing ? 0 : l->function;
    uint32_t args[] = {l->info, l->queue->handle, property};
    pthread_mutex_unlock(&queue_lock);
    if (function) compat_runtime32_call(function, args, 3);
}

static void release_buffer(struct queue_buffer *b)
{
    compat_runtime32_deallocate(b->packets);
    compat_runtime32_deallocate(b->data);
    compat_runtime32_deallocate(b->guest);
    free(b);
}

int audio_queue_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *result)
{
    if (strncmp(name, "_AudioQueue", 11)) return 0;
#define IS(s) (!strcmp(name, "_AudioQueue" s))
#define PTR(i) ((void *)(uintptr_t)a[i])
    OSStatus status = kAudio_ParamError;
    if (IS("NewOutput")) {
        if (!a[0] || !a[1] || !a[6]) goto done;
        *(uint32_t *)PTR(6) = 0;
        pthread_mutex_lock(&queue_lock);
        struct queue *q = NULL;
        for (unsigned i = 0; i < QUEUE_COUNT; ++i) {
            if (queues[i].used) continue;
            q = &queues[i];
            memset(q, 0, sizeof(*q));
            q->used = true; q->handle = QUEUE_BASE + i;
            q->function = a[1]; q->info = a[2];
            break;
        }
        pthread_mutex_unlock(&queue_lock);
        if (!q) { status = -108; goto done; }
        if (trace_audio_queue()) {
            const AudioStreamBasicDescription *f = PTR(0);
            fprintf(stderr, "compat32: AudioQueueNewOutput begin queue=0x%08x rate=%.3f format=0x%x flags=0x%x channels=%u bits=%u bytes/frame=%u frames/packet=%u\n",
                q->handle, f->mSampleRate, f->mFormatID, f->mFormatFlags,
                f->mChannelsPerFrame, f->mBitsPerChannel, f->mBytesPerFrame, f->mFramesPerPacket);
            fflush(stderr);
        }
        status = AudioQueueNewOutput(PTR(0), output_ready, q,
            a[3] ? objc_bridge32_host_object(a[3]) : NULL,
            a[4] ? objc_bridge32_host_object(a[4]) : NULL, a[5], &q->host);
        if (!status) *(uint32_t *)PTR(6) = q->handle;
        else { pthread_mutex_lock(&queue_lock); q->used = false; pthread_mutex_unlock(&queue_lock); }
        goto done;
    }
    unsigned index = a[0] - QUEUE_BASE;
    struct queue *q = index < QUEUE_COUNT && queues[index].used ? &queues[index] : NULL;
    if (!q || !q->host) goto done;
    if (IS("Start") || IS("Stop") || IS("Reset") || IS("Dispose"))
        trace_queue_operation(name, q);
    if (IS("AllocateBuffer") || IS("AllocateBufferWithPacketDescriptions")) {
        bool packets = IS("AllocateBufferWithPacketDescriptions");
        unsigned out = packets ? 3 : 2;
        if (!a[out]) goto done;
        *(uint32_t *)PTR(out) = 0;
        struct queue_buffer *b = calloc(1, sizeof(*b));
        if (!b) { status = -108; goto done; }
        status = packets ? AudioQueueAllocateBufferWithPacketDescriptions(q->host, a[1], a[2], &b->host)
                         : AudioQueueAllocateBuffer(q->host, a[1], &b->host);
        if (status) { free(b); goto done; }
        b->guest = compat_runtime32_allocate(sizeof(struct buffer32), 1);
        b->data = compat_runtime32_allocate(b->host->mAudioDataBytesCapacity, 1);
        if (b->host->mPacketDescriptionCapacity)
            b->packets = compat_runtime32_allocate((size_t)b->host->mPacketDescriptionCapacity * sizeof(AudioStreamPacketDescription), 1);
        if (!b->guest || !b->data || (b->host->mPacketDescriptionCapacity && !b->packets)) {
            AudioQueueFreeBuffer(q->host, b->host); release_buffer(b);
            status = -108; goto done;
        }
        struct buffer32 *g = (void *)(uintptr_t)b->guest;
        g->capacity = b->host->mAudioDataBytesCapacity; g->data = b->data;
        g->packet_capacity = b->host->mPacketDescriptionCapacity; g->packets = b->packets;
        pthread_mutex_lock(&queue_lock);
        b->next = q->buffers; q->buffers = b;
        pthread_mutex_unlock(&queue_lock);
        *(uint32_t *)PTR(out) = b->guest;
    } else if (IS("EnqueueBuffer") || IS("FreeBuffer")) {
        pthread_mutex_lock(&queue_lock);
        struct queue_buffer *b = q->buffers;
        while (b && b->guest != a[1]) b = b->next;
        pthread_mutex_unlock(&queue_lock);
        if (!b) goto done;
        if (IS("FreeBuffer")) {
            status = AudioQueueFreeBuffer(q->host, b->host);
            if (!status) {
                pthread_mutex_lock(&queue_lock);
                struct queue_buffer **link = &q->buffers;
                while (*link && *link != b) link = &(*link)->next;
                if (*link) *link = b->next;
                pthread_mutex_unlock(&queue_lock);
                release_buffer(b);
            }
        } else {
            struct buffer32 *g = (void *)(uintptr_t)b->guest;
            if (g->size > b->host->mAudioDataBytesCapacity ||
                g->packet_count > b->host->mPacketDescriptionCapacity) goto done;
            memcpy(b->host->mAudioData, (void *)(uintptr_t)b->data, g->size);
            b->host->mAudioDataByteSize = g->size;
            b->host->mPacketDescriptionCount = g->packet_count;
            if (g->packet_count) memcpy(b->host->mPacketDescriptions, (void *)(uintptr_t)b->packets,
                (size_t)g->packet_count * sizeof(AudioStreamPacketDescription));
            status = AudioQueueEnqueueBuffer(q->host, b->host, a[2], PTR(3));
        }
    } else if (IS("Start")) status = AudioQueueStart(q->host, PTR(1));
    else if (IS("Stop")) status = AudioQueueStop(q->host, !!a[1]);
    else if (IS("Pause")) status = AudioQueuePause(q->host);
    else if (IS("Reset")) status = AudioQueueReset(q->host);
    else if (IS("Flush")) status = AudioQueueFlush(q->host);
    else if (IS("Prime")) status = AudioQueuePrime(q->host, a[1], PTR(2));
    else if (IS("SetParameter")) {
        float value; memcpy(&value, &a[2], sizeof(value));
        status = AudioQueueSetParameter(q->host, a[1], value);
    } else if (IS("GetParameter")) status = AudioQueueGetParameter(q->host, a[1], PTR(2));
    else if (IS("GetPropertySize")) {
        status = AudioQueueGetPropertySize(q->host, a[1], PTR(2));
        if (!status && a[1] == kAudioQueueProperty_CurrentDevice) *(uint32_t *)PTR(2) = 4;
    } else if (IS("GetProperty")) {
        if (a[1] == kAudioQueueProperty_CurrentDevice) {
            if (!a[2] || !a[3] || *(uint32_t *)PTR(3) < 4) goto done;
            CFStringRef device = NULL; UInt32 size = sizeof(device);
            status = AudioQueueGetProperty(q->host, a[1], &device, &size);
            if (!status) { *(uint32_t *)PTR(2) = objc_bridge32_guest_object((void *)device); *(uint32_t *)PTR(3) = 4; }
        } else status = AudioQueueGetProperty(q->host, a[1], PTR(2), PTR(3));
    } else if (IS("SetProperty")) {
        if (a[1] == kAudioQueueProperty_CurrentDevice) {
            if (!a[2] || a[3] != 4) goto done;
            CFStringRef device = objc_bridge32_host_object(*(uint32_t *)PTR(2));
            status = AudioQueueSetProperty(q->host, a[1], &device, sizeof(device));
        } else status = AudioQueueSetProperty(q->host, a[1], PTR(2), a[3]);
    } else if (IS("AddPropertyListener") || IS("RemovePropertyListener")) {
        bool add = IS("AddPropertyListener");
        struct listener *l = NULL;
        pthread_mutex_lock(&queue_lock);
        for (struct listener *candidate = q->listeners; candidate; candidate = candidate->next) {
            if (candidate->function == a[2] && candidate->property == a[1] && candidate->info == a[3]) { l = candidate; break; }
        }
        if (l && add) { pthread_mutex_unlock(&queue_lock); status = noErr; goto done; }
        if (!l && add && a[2]) {
            l = calloc(1, sizeof(*l));
            if (l) {
                *l = (struct listener){q, a[2], a[3], a[1], q->listeners};
                q->listeners = l;
            }
        }
        pthread_mutex_unlock(&queue_lock);
        if (!l) goto done;
        status = add ? AudioQueueAddPropertyListener(q->host, a[1], property_changed, l)
                     : AudioQueueRemovePropertyListener(q->host, a[1], property_changed, l);
        if ((!add && !status) || (add && status)) {
            pthread_mutex_lock(&queue_lock); l->function = 0; pthread_mutex_unlock(&queue_lock);
        }
    } else if (IS("Dispose")) {
        pthread_mutex_lock(&queue_lock); q->disposing = true; pthread_mutex_unlock(&queue_lock);
        /* Teardown is synchronous so native callbacks cannot outlive their
           guest mirrors, including when the caller permits deferred disposal. */
        status = AudioQueueDispose(q->host, true);
        if (!status) {
            while (q->buffers) { struct queue_buffer *b = q->buffers; q->buffers = b->next; release_buffer(b); }
            while (q->listeners) { struct listener *l = q->listeners; q->listeners = l->next; free(l); }
            pthread_mutex_lock(&queue_lock); memset(q, 0, sizeof(*q)); pthread_mutex_unlock(&queue_lock);
        } else { pthread_mutex_lock(&queue_lock); q->disposing = false; pthread_mutex_unlock(&queue_lock); }
    } else return 0;
done:
    if (status || (trace_audio_queue() && (IS("NewOutput") || IS("Start") ||
        IS("Stop") || IS("Reset") || IS("Dispose")))) {
        fprintf(stderr, "compat32: %s end arg0=0x%08x status=%d\n", name + 1, a[0], (int)status);
        fflush(stderr);
    }
    *result = (uint32_t)status;
    return 1;
#undef IS
#undef PTR
}
