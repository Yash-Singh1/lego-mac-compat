/* Disposes AudioQueues while their output callbacks keep refilling and
 * re-enqueueing buffers, and while another thread keeps starting, pausing,
 * stopping and adjusting the same queue, as MW2's Miles and Bink drivers do
 * around movies and level changes. A call that reached AudioToolbox after
 * disposal freed the queue corrupted the audio allocator; the next
 * AudioQueueNewOutput then trapped in caulk. */
#include "../src/audio_queue_bridge.h"
#include <AudioToolbox/AudioToolbox.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static uint32_t cursor = 0x40000000;
static pthread_mutex_t heap = PTHREAD_MUTEX_INITIALIZER;
uint32_t compat_runtime32_allocate(size_t n, int clear)
{
    pthread_mutex_lock(&heap);
    uint32_t p = cursor;
    cursor += (uint32_t)((n + 15) & ~(size_t)15);
    assert(cursor < 0x44000000);
    pthread_mutex_unlock(&heap);
    if (clear) memset((void *)(uintptr_t)p, 0, n);
    return p;
}
void compat_runtime32_deallocate(uint32_t p) { (void)p; }

static atomic_uint refills;
/* The guest callback: refill the buffer and enqueue it again. */
uint32_t compat_runtime32_call(uint32_t function, const uint32_t *a, size_t count)
{
    (void)function; (void)count;
    uint32_t *record = (uint32_t *)(uintptr_t)a[2];
    record[2] = record[0]; /* byte_size = capacity */
    uint32_t args[] = {a[1], a[2], 0, 0};
    uint64_t result;
    audio_queue_bridge32_dispatch("_AudioQueueEnqueueBuffer", args, &result);
    atomic_fetch_add(&refills, 1);
    return 0;
}

static _Atomic uint32_t current_queue;
static atomic_bool racing = true;
static atomic_uint racer_calls;
/* Another guest thread operating on whichever queue is current. */
static void *racer(void *unused)
{
    (void)unused;
    uint32_t *size = (void *)(uintptr_t)compat_runtime32_allocate(16, 1);
    while (atomic_load(&racing)) {
        uint32_t queue = atomic_load(&current_queue);
        if (!queue) continue;
        uint64_t result;
        float volume = 0.5f;
        uint32_t parameter[3] = {queue, kAudioQueueParam_Volume, 0};
        memcpy(&parameter[2], &volume, 4);
        audio_queue_bridge32_dispatch("_AudioQueueSetParameter", parameter, &result);
        uint32_t pause[] = {queue};
        audio_queue_bridge32_dispatch("_AudioQueuePause", pause, &result);
        uint32_t start[] = {queue, 0};
        audio_queue_bridge32_dispatch("_AudioQueueStart", start, &result);
        size[0] = 4;
        uint32_t get[] = {queue, kAudioQueueProperty_IsRunning, (uint32_t)(uintptr_t)(size + 1),
                          (uint32_t)(uintptr_t)size};
        audio_queue_bridge32_dispatch("_AudioQueueGetProperty", get, &result);
        uint32_t stop[] = {queue, 0};
        audio_queue_bridge32_dispatch("_AudioQueueStop", stop, &result);
        atomic_fetch_add(&racer_calls, 1);
    }
    return NULL;
}

int main(void)
{
    assert(mmap((void *)0x40000000, 0x4000000, PROT_READ | PROT_WRITE,
                MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) == (void *)0x40000000);
    setenv("LP32_MUTE_AUDIO", "1", 1);
    AudioStreamBasicDescription *format = (void *)(uintptr_t)compat_runtime32_allocate(sizeof(*format), 1);
    *format = (AudioStreamBasicDescription){
        .mSampleRate = 44100, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
        .mBytesPerPacket = 4, .mFramesPerPacket = 1, .mBytesPerFrame = 4,
        .mChannelsPerFrame = 2, .mBitsPerChannel = 16,
    };
    uint32_t *out = (void *)(uintptr_t)compat_runtime32_allocate(8, 1);
    pthread_t racing_thread;
    pthread_create(&racing_thread, NULL, racer, NULL);
    for (unsigned round = 0; round < 200; ++round) {
        uint64_t result;
        uint32_t create[] = {(uint32_t)(uintptr_t)format, 1, 0, 0, 0, 0, (uint32_t)(uintptr_t)out};
        assert(audio_queue_bridge32_dispatch("_AudioQueueNewOutput", create, &result) && !result);
        uint32_t queue = out[0];
        for (unsigned i = 0; i < 3; ++i) {
            uint32_t alloc[] = {queue, 256, (uint32_t)(uintptr_t)(out + 1)};
            assert(audio_queue_bridge32_dispatch("_AudioQueueAllocateBuffer", alloc, &result) && !result);
            uint32_t *record = (uint32_t *)(uintptr_t)out[1];
            record[2] = 256;
            uint32_t enqueue[] = {queue, out[1], 0, 0};
            audio_queue_bridge32_dispatch("_AudioQueueEnqueueBuffer", enqueue, &result);
        }
        uint32_t start[] = {queue, 0};
        audio_queue_bridge32_dispatch("_AudioQueueStart", start, &result);
        atomic_store(&current_queue, queue);
        usleep(round % 4 ? 300 + (round % 11) * 700 : 60000);
        uint32_t dispose[] = {queue, round % 2};
        assert(audio_queue_bridge32_dispatch("_AudioQueueDispose", dispose, &result));
    }
    atomic_store(&racing, false);
    pthread_join(racing_thread, NULL);
    usleep(200000);
    /* Fresh queues still work after all the disposals (the step that trapped
       in caulk when the allocator had been corrupted). */
    uint64_t result;
    for (unsigned i = 0; i < 50; ++i) {
        uint32_t create[] = {(uint32_t)(uintptr_t)format, 1, 0, 0, 0, 0, (uint32_t)(uintptr_t)out};
        assert(audio_queue_bridge32_dispatch("_AudioQueueNewOutput", create, &result) && !result);
        uint32_t dispose[] = {out[0], 1};
        audio_queue_bridge32_dispatch("_AudioQueueDispose", dispose, &result);
    }
    /* The crash report finds a native buffer from its address. */
    {
        int pipe_fds[2];
        assert(pipe(pipe_fds) == 0);
        uint32_t create[] = {(uint32_t)(uintptr_t)format, 1, 0, 0, 0, 0, (uint32_t)(uintptr_t)out};
        assert(audio_queue_bridge32_dispatch("_AudioQueueNewOutput", create, &result) && !result);
        uint32_t alloc[] = {out[0], 4096, (uint32_t)(uintptr_t)(out + 1)};
        assert(audio_queue_bridge32_dispatch("_AudioQueueAllocateBuffer", alloc, &result) && !result);
        uint64_t none = 0;
        audio_queue_bridge32_crash_report(pipe_fds[1], -1, &none, 1);
        static char text[1 << 16];
        ssize_t n = read(pipe_fds[0], text, sizeof(text) - 1);
        assert(n > 0);
        text[n] = 0;
        const char *allocate = strstr(text, " allocate ");
        assert(allocate && strstr(text, "audio queue operations"));
        unsigned long long address = strtoull(strstr(allocate, "object=") + 7, NULL, 16);
        uint64_t inside = address + 16;
        audio_queue_bridge32_crash_report(pipe_fds[1], -1, &inside, 1);
        n = read(pipe_fds[0], text, sizeof(text) - 1);
        assert(n > 0);
        text[n] = 0;
        assert(strstr(text, "+16 bytes from buffer"));
        close(pipe_fds[0]);
        close(pipe_fds[1]);
    }
    /* Playing queues are still refilled; only disposing ones are not. */
    assert(atomic_load(&refills) > 100);
    printf("AudioQueue bridge PASS (200 disposals during callback refills and %u racing control "
           "calls, %u refills)\n", atomic_load(&racer_calls), atomic_load(&refills));
    return 0;
}
