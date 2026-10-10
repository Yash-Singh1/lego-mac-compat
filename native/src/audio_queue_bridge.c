/* Audio Queue Services for i386 guests (MW2's Miles and Bink CoreAudio
 * drivers). Guest queues are small handles; guest buffers are i386
 * AudioQueueBuffer records in guest memory, each shadowed by a native buffer.
 * Output callbacks run on the native queue thread and call straight into the
 * guest. Miles refills there; Bink only acknowledges completion and feeds
 * more audio when the game next calls BinkWait/BinkService. */
#include "audio_queue_bridge.h"
#include "compat_runtime.h"
#include "guest_dyld.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <mach/kern_return.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
    bool movie, playing;
    unsigned controls, callbacks;
    bool deferred_dispose, dispose_immediate, dispose_was_playing;
    unsigned buffer_count;
    struct { uint32_t guest; AudioQueueBufferRef host; } buffers[kMaxBuffers];
};

/* Slots are never reused: a late native callback may still carry the address. */
static struct queue queues[kMaxQueues];
static unsigned queue_count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned playing_movies;
static pthread_cond_t controls_done = PTHREAD_COND_INITIALIZER;
struct control_scope { struct queue *queue; struct control_scope *previous; };
static _Thread_local struct control_scope *control_scopes;

static bool current_thread_controls(struct queue *q)
{
    for (struct control_scope *scope = control_scopes; scope; scope = scope->previous)
        if (scope->queue == q) return true;
    return false;
}

int audio_queue_bridge32_movie_playing(void)
{
    return __atomic_load_n(&playing_movies, __ATOMIC_RELAXED) != 0;
}

static bool movie_callback(uint32_t callback)
{
    uint32_t offset;
    const char *path = guest_dyld32_describe(callback, &offset);
    if (!path) return false;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    return !strcmp(name, "libBinkMacx86.dylib") ||
           !strcmp(name, "libBink2Macx86.dylib") ||
           !strcmp(name, "libBinkMachOx86.dylib");
}

/* Called under lock. Count queues, not start calls: repeated starts and
   overlapping movies must not leave background pacing disabled forever. */
static void set_playing(struct queue *q, bool playing)
{
    playing = playing && q->audio && !q->disposing;
    if (!q->movie || q->playing == playing) return;
    q->playing = playing;
    if (playing) __atomic_fetch_add(&playing_movies, 1, __ATOMIC_RELAXED);
    else __atomic_fetch_sub(&playing_movies, 1, __ATOMIC_RELAXED);
}

static void playback_result(struct queue *q, bool playing, OSStatus status)
{
    if (status) return;
    pthread_mutex_lock(&lock);
    /* Retirement drains successful controls before native disposal. If
       disposal fails, restore their latest logical playback result. */
    if (q->disposing) q->dispose_was_playing = playing;
    else set_playing(q, playing);
    pthread_mutex_unlock(&lock);
}

/* Diagnostics for allocator corruption: every native buffer handed out, and
   a ring of recent queue operations, both read by the crash handler. */
enum { kHistory = 2048, kLifecycleHistory = 256, kKnownBuffers = 16384 };
enum event_kind { kEventNew, kEventAllocate, kEventEnqueue, kEventStart, kEventStop, kEventPause,
                  kEventDisposeBegin, kEventDisposeEnd, kEventCallback, kEventNewBegin };
static const char *const event_names[] = {"new", "allocate", "enqueue", "start", "stop", "pause",
                                          "dispose-begin", "dispose-end", "callback", "new-begin"};
struct event { uint64_t time; uint32_t thread, queue; uint8_t kind; uint64_t object; uint32_t bytes; int32_t status; };
static struct event history[kHistory];
static unsigned history_next;
static struct event lifecycle_history[kLifecycleHistory];
static unsigned lifecycle_next;
struct known_buffer { uint64_t data; uint32_t capacity, queue; uint64_t allocated, disposed; };
static struct known_buffer known_buffers[kKnownBuffers];
static unsigned known_buffer_count;

static void note_event(uint8_t kind, uint32_t queue, const void *object, uint32_t bytes, int32_t status)
{
    unsigned slot = __atomic_fetch_add(&history_next, 1, __ATOMIC_RELAXED) % kHistory;
    struct event event = {clock_gettime_nsec_np(CLOCK_UPTIME_RAW), pthread_mach_thread_np(pthread_self()),
                          queue, kind, (uint64_t)(uintptr_t)object, bytes, status};
    history[slot] = event;
    /* Refills can overwrite the ordinary history many times during a movie.
       Keep creation/control/disposal evidence independently of that traffic. */
    if (kind != kEventEnqueue && kind != kEventAllocate && kind != kEventCallback) {
        unsigned lifecycle_slot = __atomic_fetch_add(&lifecycle_next, 1, __ATOMIC_RELAXED) % kLifecycleHistory;
        lifecycle_history[lifecycle_slot] = event;
    }
}

static void remember_buffer(uint32_t queue, AudioQueueBufferRef host)
{
    unsigned slot = __atomic_fetch_add(&known_buffer_count, 1, __ATOMIC_RELAXED);
    if (slot >= kKnownBuffers) return;
    known_buffers[slot] = (struct known_buffer){(uint64_t)(uintptr_t)host->mAudioData,
        host->mAudioDataBytesCapacity, queue, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), 0};
}

static void forget_buffers(uint32_t queue)
{
    unsigned count = __atomic_load_n(&known_buffer_count, __ATOMIC_RELAXED);
    if (count > kKnownBuffers) count = kKnownBuffers;
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    for (unsigned i = 0; i < count; ++i)
        if (known_buffers[i].queue == queue && !known_buffers[i].disposed) known_buffers[i].disposed = now;
}

void audio_queue_bridge32_crash_report(int fd, int extra_fd, const uint64_t *addresses, unsigned count)
{
    char line[256];
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#define REPORT(...) do { int n = snprintf(line, sizeof(line), __VA_ARGS__); \
        if (n > 0) { if (fd >= 0) write(fd, line, (size_t)n); if (extra_fd >= 0) write(extra_fd, line, (size_t)n); } } while (0)
    unsigned known = __atomic_load_n(&known_buffer_count, __ATOMIC_RELAXED);
    if (known > kKnownBuffers) known = kKnownBuffers;
    REPORT("compat32: audio queues %u, native buffers %u\n", queue_count, known);
    for (unsigned a = 0; a < count; ++a) {
        uint64_t address = addresses[a];
        /* Newest first: freed buffers' memory is reused by later ones. The
           margin covers an allocator header just before the data. */
        unsigned matches = 0;
        for (unsigned i = known; i-- > 0 && matches < 8;) {
            const struct known_buffer *b = &known_buffers[i];
            if (address + 0x100 < b->data || address >= b->data + b->capacity) continue;
            ++matches;
            REPORT("compat32: address %#llx is %+lld bytes from buffer %#llx (capacity %u) of queue %08x, "
                   "allocated %.0f ms ago, %s\n", (unsigned long long)address,
                   (long long)(address - b->data), (unsigned long long)b->data, b->capacity, b->queue,
                   (now - b->allocated) / 1e6, b->disposed ? "disposed" : "live");
            if (b->disposed) REPORT("compat32:   that queue was disposed %.0f ms ago\n", (now - b->disposed) / 1e6);
        }
    }
    unsigned next = __atomic_load_n(&history_next, __ATOMIC_RELAXED);
    unsigned shown = next < 48 ? next : 48;
    REPORT("compat32: last %u audio queue operations (oldest first):\n", shown);
    for (unsigned i = next - shown; i != next; ++i) {
        const struct event *e = &history[i % kHistory];
        REPORT("compat32:   -%.1f ms thread %u %s queue=%08x object=%#llx bytes=%u status=%d\n",
               (now - e->time) / 1e6, e->thread, e->kind < sizeof(event_names) / sizeof(event_names[0]) ?
               event_names[e->kind] : "?", e->queue, (unsigned long long)e->object, e->bytes, e->status);
    }
    next = __atomic_load_n(&lifecycle_next, __ATOMIC_RELAXED);
    shown = next < 48 ? next : 48;
    REPORT("compat32: last %u audio queue lifecycle operations (oldest first):\n", shown);
    for (unsigned i = next - shown; i != next; ++i) {
        const struct event *e = &lifecycle_history[i % kLifecycleHistory];
        REPORT("compat32:   -%.1f ms thread %u %s queue=%08x object=%#llx bytes=%u status=%d\n",
               (now - e->time) / 1e6, e->thread, e->kind < sizeof(event_names) / sizeof(event_names[0]) ?
               event_names[e->kind] : "?", e->queue, (unsigned long long)e->object, e->bytes, e->status);
    }
#undef REPORT
}
static _Thread_local struct queue *callback_queue;

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

/* Asynchronous native disposal can return while an output callback is still
   running. Its guest buffer records must survive until that callback returns.
   Called under lock; only one caller takes ownership of the retired records. */
static unsigned take_retired_buffers(struct queue *q)
{
    if (q->audio || !q->disposing || q->callbacks) return 0;
    unsigned count = q->buffer_count;
    q->buffer_count = 0;
    return count;
}

static void release_guest_buffers(struct queue *q, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        compat_runtime32_deallocate(q->buffers[i].guest);
}

static void output(void *raw, AudioQueueRef audio, AudioQueueBufferRef buffer)
{
    (void)audio;
    struct queue *q = raw;
    pthread_mutex_lock(&lock);
    uint32_t guest = 0;
    if (q->audio && !q->disposing)
        for (unsigned i = 0; i < q->buffer_count; ++i)
            if (q->buffers[i].host == buffer) { guest = q->buffers[i].guest; break; }
    if (guest) ++q->callbacks;
    pthread_mutex_unlock(&lock);
    if (!guest) return;
    /* Read the bridge's permanent association, not a native buffer that a
       concurrent disposal may retire. */
    uint32_t arguments[] = {q->user_data, q->handle, guest};
    if (getenv("LP32_TRACE_AUDIO_QUEUE")) {
        static unsigned calls;
        if (++calls % 200 == 1) fprintf(stderr, "compat32: AudioQueue output callback %u queue=%08x\n", calls, q->handle);
    }
    if (q->callback) {
        struct queue *previous = callback_queue;
        callback_queue = q;
        compat_runtime32_call(q->callback, arguments, 3);
        callback_queue = previous;
    }
    pthread_mutex_lock(&lock);
    --q->callbacks;
    unsigned retired = take_retired_buffers(q);
    pthread_mutex_unlock(&lock);
    release_guest_buffers(q, retired);
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
    q->movie = movie_callback(q->callback);
    /* The guest's run loop is not a native CFRunLoopRef; use the queue's own thread. */
    fprintf(stderr, "compat32: AudioQueue create queue=%08x rate=%.0f format=%.4s "
                    "flags=%x channels=%u bits=%u bytes/frame=%u\n",
            q->handle, format->mSampleRate, (const char *)&format->mFormatID,
            format->mFormatFlags, format->mChannelsPerFrame,
            format->mBitsPerChannel, format->mBytesPerFrame);
    note_event(kEventNewBegin, q->handle, format, format->mBytesPerFrame, noErr);
    OSStatus status = AudioQueueNewOutput(format, output, q, NULL, NULL, 0, &q->audio);
    note_event(kEventNew, q->handle, q->audio, 0, status);
    fprintf(stderr, "compat32: AudioQueue create queue=%08x status=%d\n",
            q->handle, (int)status);
    if (status) return status;
    if (getenv("LP32_MUTE_AUDIO")) AudioQueueSetParameter(q->audio, kAudioQueueParam_Volume, 0);
    *out = q->handle;
    return noErr;
}

static OSStatus allocate_buffer(struct queue *q, uint32_t capacity, uint32_t *out)
{
    if (!out) return kAudio_ParamError;
    uint32_t guest = compat_runtime32_allocate(sizeof(struct guest_buffer32) + capacity, 1);
    if (!guest) return kAudio_MemFullError;
    /* Disposal must not free a newly allocated host buffer before its user
       data and guest shadow have been initialized and published. Checking
       disposing only after AudioQueueAllocateBuffer leaves that entire
       initialization window exposed to use-after-free. */
    pthread_mutex_lock(&lock);
    OSStatus status;
    if (!q->audio || q->disposing) status = kAudioQueueErr_InvalidQueueType;
    else if (q->buffer_count == kMaxBuffers) status = kAudio_MemFullError;
    else {
        AudioQueueBufferRef host = NULL;
        status = AudioQueueAllocateBuffer(q->audio, capacity, &host);
        if (!status) {
            host->mUserData = (void *)(uintptr_t)guest;
            remember_buffer(q->handle, host);
            note_event(kEventAllocate, q->handle, host->mAudioData, capacity, noErr);
            struct guest_buffer32 *record = (void *)(uintptr_t)guest;
            record->capacity = capacity;
            record->data = guest + sizeof(*record);
            q->buffers[q->buffer_count].guest = guest;
            q->buffers[q->buffer_count++].host = host;
            *out = guest;
        }
    }
    pthread_mutex_unlock(&lock);
    if (status) compat_runtime32_deallocate(guest);
    return status;
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
    note_event(kEventEnqueue, q->handle, host ? host->mAudioData : NULL, host ? record->byte_size : 0, status);
    pthread_mutex_unlock(&lock);
    return status;
}

/* No bridge lock is held across native calls or callbacks. */
static OSStatus finish_dispose(struct queue *q)
{
    AudioQueueRef audio = q->audio;
    note_event(kEventDisposeBegin, q->handle, audio, q->dispose_immediate, noErr);
    /* Not under lock: synchronous disposal waits for an output callback,
       which may itself be in enqueue(). Asynchronous disposal can return
       before that callback finishes, so guest records remain pinned. */
    OSStatus disposed = AudioQueueDispose(audio, q->dispose_immediate);
    note_event(kEventDisposeEnd, q->handle, audio, q->dispose_immediate, disposed);
    if (disposed) {
        pthread_mutex_lock(&lock);
        q->disposing = false;
        set_playing(q, q->dispose_was_playing);
        pthread_mutex_unlock(&lock);
        return disposed;
    }
    forget_buffers(q->handle);
    pthread_mutex_lock(&lock);
    q->audio = NULL;
    unsigned count = take_retired_buffers(q);
    pthread_mutex_unlock(&lock);
    release_guest_buffers(q, count);
    return noErr;
}

static OSStatus dispose(struct queue *q, bool immediate)
{
    pthread_mutex_lock(&lock);
    if (!q->audio || q->disposing) {
        pthread_mutex_unlock(&lock);
        return kAudioQueueErr_InvalidQueueType;
    }
    q->disposing = true;
    q->dispose_was_playing = q->playing;
    q->dispose_immediate = immediate;
    set_playing(q, false);
    /* A native control can synchronously invoke the requesting callback.
       Waiting here would deadlock that control. Retire after it returns. */
    if (q->controls && (current_thread_controls(q) || callback_queue == q)) {
        q->deferred_dispose = true;
        pthread_mutex_unlock(&lock);
        return noErr;
    }
    while (q->controls) pthread_cond_wait(&controls_done, &lock);
    pthread_mutex_unlock(&lock);
    return finish_dispose(q);
}

static AudioQueueRef begin_control(struct queue *q)
{
    pthread_mutex_lock(&lock);
    AudioQueueRef audio = q->disposing ? NULL : q->audio;
    if (audio) ++q->controls;
    pthread_mutex_unlock(&lock);
    return audio;
}

static void end_control(struct queue *q)
{
    pthread_mutex_lock(&lock);
    bool deferred = --q->controls == 0 && q->deferred_dispose;
    if (deferred) q->deferred_dispose = false;
    pthread_cond_broadcast(&controls_done);
    pthread_mutex_unlock(&lock);
    if (deferred) finish_dispose(q);
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
    bool control = !strcmp(name, "_AudioQueueStart") || !strcmp(name, "_AudioQueueStop") ||
        !strcmp(name, "_AudioQueuePause") || !strcmp(name, "_AudioQueueGetProperty") ||
        !strcmp(name, "_AudioQueueSetProperty") || !strcmp(name, "_AudioQueueSetParameter");
    AudioQueueRef audio = control ? begin_control(q) : NULL;
    if (control && !audio) { *result = (uint32_t)kAudioQueueErr_InvalidQueueType; return 1; }
    struct control_scope scope = {q, control_scopes};
    if (control) control_scopes = &scope;
    OSStatus status;
    if (!strcmp(name, "_AudioQueueAllocateBuffer")) status = allocate_buffer(q, a[1], (void *)(uintptr_t)a[2]);
    else if (!strcmp(name, "_AudioQueueEnqueueBuffer")) status = enqueue(q, a);
    else if (!strcmp(name, "_AudioQueueStart")) {
        status = AudioQueueStart(audio, (const void *)(uintptr_t)a[1]);
        playback_result(q, true, status);
        note_event(kEventStart, q->handle, q->audio, 0, status);
    } else if (!strcmp(name, "_AudioQueueStop")) {
        status = AudioQueueStop(audio, a[1] != 0);
        playback_result(q, false, status);
        note_event(kEventStop, q->handle, q->audio, a[1], status);
    } else if (!strcmp(name, "_AudioQueuePause")) {
        status = AudioQueuePause(audio);
        playback_result(q, false, status);
        note_event(kEventPause, q->handle, q->audio, 0, status);
    }
    else if (!strcmp(name, "_AudioQueueDispose")) status = dispose(q, a[1] != 0);
    else if (!strcmp(name, "_AudioQueueGetProperty") || !strcmp(name, "_AudioQueueSetProperty")) {
        /* Channel layouts, stream formats and codec settings are made of
           4-byte fields and match on i386. Object-valued properties do not. */
        if (a[1] == kAudioQueueProperty_CurrentDevice) status = kAudioQueueErr_InvalidPropertyValue;
        else if (name[11] == 'G')
            status = AudioQueueGetProperty(audio, a[1], (void *)(uintptr_t)a[2], (UInt32 *)(uintptr_t)a[3]);
        else status = AudioQueueSetProperty(audio, a[1], (const void *)(uintptr_t)a[2], a[3]);
    } else if (!strcmp(name, "_AudioQueueSetParameter")) {
        float value;
        memcpy(&value, &a[2], sizeof(value));
        if (a[1] == kAudioQueueParam_Volume && getenv("LP32_MUTE_AUDIO")) value = 0;
        status = AudioQueueSetParameter(audio, a[1], value);
    } else return 0;
    if (control) { control_scopes = scope.previous; end_control(q); }
    if (getenv("LP32_TRACE_AUDIO_QUEUE"))
        fprintf(stderr, "compat32: %s queue=%08x -> %d\n", name + 1, a[0], (int)status);
    *result = (uint32_t)status;
    return 1;
}
