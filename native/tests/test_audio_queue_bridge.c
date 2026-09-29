/* Disposes AudioQueues while their output callbacks keep refilling and
 * re-enqueueing buffers, as MW2's Miles and Bink drivers do. Before disposal
 * was ordered against enqueue, the refill could copy into native buffers the
 * disposal had freed, corrupting the audio allocator. */
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
        usleep(round % 4 ? 300 + (round % 11) * 700 : 60000);
        uint32_t dispose[] = {queue, round % 2};
        assert(audio_queue_bridge32_dispatch("_AudioQueueDispose", dispose, &result));
    }
    usleep(200000);
    /* A fresh queue still works after all the disposals. */
    uint64_t result;
    uint32_t create[] = {(uint32_t)(uintptr_t)format, 1, 0, 0, 0, 0, (uint32_t)(uintptr_t)out};
    assert(audio_queue_bridge32_dispatch("_AudioQueueNewOutput", create, &result) && !result);
    /* Playing queues are still refilled; only disposing ones are not. */
    assert(atomic_load(&refills) > 100);
    printf("AudioQueue bridge PASS (200 disposals during callback refills, %u refills)\n",
           atomic_load(&refills));
    return 0;
}
