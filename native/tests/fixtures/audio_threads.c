#include <AudioToolbox/AudioToolbox.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static volatile int worker_ran, returned_buffer, listener_ran;
static AudioQueueRef expected_queue;
static AudioQueueBufferRef expected_buffer;

static void *worker(void *info)
{
    worker_ran = (uintptr_t)info == 0x12345 ? 1 : -1;
    return (void *)0x56789;
}
static void buffer_done(void *info, AudioQueueRef q, AudioQueueBufferRef b)
{
    returned_buffer = (uintptr_t)info == 0xabc && q == expected_queue && b == expected_buffer &&
        b->mUserData == (void *)0xdef && b->mAudioDataBytesCapacity >= 17640 ? 1 : -1;
}
static void state_changed(void *info, AudioQueueRef q, AudioQueuePropertyID property)
{
    if ((uintptr_t)info != 0x987 || q != expected_queue || property != kAudioQueueProperty_IsRunning)
        listener_ran = -100;
    else ++listener_ran;
}

int check_audio_threads(void)
{
    worker_ran = returned_buffer = listener_ran = 0;
    struct { pthread_t thread; uint32_t guard; } t = {0, 0x12345678};
    if (pthread_create_suspended_np(&t.thread, NULL, worker, (void *)0x12345)) return -200;
    usleep(10000);
    if (worker_ran || t.guard != 0x12345678) return -201;
    mach_port_t port = pthread_mach_thread_np(t.thread);
    if (!port || thread_resume(port)) return -202;
    struct { void *value; uint32_t guard; } joined = {0, 0x87654321};
    if (pthread_join(t.thread, &joined.value) || worker_ran != 1 ||
        joined.value != (void *)0x56789 || joined.guard != 0x87654321) return -203;

    AudioStreamBasicDescription format = {44100, kAudioFormatLinearPCM,
        kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked, 4, 1, 4, 2, 16, 0};
    struct { AudioQueueRef queue; uint32_t guard; } output = {0, 0x12345678};
    if (AudioQueueNewOutput(&format, buffer_done, (void *)0xabc, NULL, NULL, 0, &output.queue)) return -204;
    expected_queue = output.queue;
    if (!output.queue || output.guard != 0x12345678) return -205;
    if (AudioQueueAddPropertyListener(output.queue, kAudioQueueProperty_IsRunning, state_changed, (void *)0x987)) return -206;
    struct { AudioQueueBufferRef buffer; uint32_t guard; } allocation = {0, 0x98765432};
    if (AudioQueueAllocateBuffer(output.queue, 17640, &allocation.buffer)) return -207;
    expected_buffer = allocation.buffer;
    if (!allocation.buffer || allocation.guard != 0x98765432 || !allocation.buffer->mAudioData ||
        allocation.buffer->mAudioDataBytesCapacity < 17640) return -208;
    memset(allocation.buffer->mAudioData, 0, 17640);
    allocation.buffer->mAudioDataByteSize = 17640;
    allocation.buffer->mUserData = (void *)0xdef;
    if (AudioQueueSetParameter(output.queue, kAudioQueueParam_Volume, 0.5f)) return -209;
    float volume = 0;
    if (AudioQueueGetParameter(output.queue, kAudioQueueParam_Volume, &volume) || volume != 0.5f) return -210;
    if (AudioQueueEnqueueBuffer(output.queue, allocation.buffer, 0, NULL) || AudioQueueStart(output.queue, NULL)) return -211;
    for (int i = 0; i < 300 && !returned_buffer; ++i) usleep(10000);
    if (returned_buffer != 1 || listener_ran < 0) return -212;
    if (AudioQueueStop(output.queue, true)) return -213;
    if (AudioQueueRemovePropertyListener(output.queue, kAudioQueueProperty_IsRunning, state_changed, (void *)0x987)) return -214;
    if (AudioQueueFreeBuffer(output.queue, allocation.buffer) || AudioQueueDispose(output.queue, true)) return -215;
    /* The registry is reusable after native teardown. */
    for (int i = 0; i < 65; ++i) {
        if (AudioQueueNewOutput(&format, buffer_done, NULL, NULL, NULL, 0, &output.queue) ||
            AudioQueueDispose(output.queue, true)) return -216;
    }
    return 0;
}
