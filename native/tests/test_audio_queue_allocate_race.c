/* Force disposal while native buffer allocation is in progress. A queue must
 * stay alive until the bridge has finished initializing and publishing the
 * buffer, even if another thread requests disposal before allocation returns. */
#include <AudioToolbox/AudioToolbox.h>
#include <assert.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>

static OSStatus test_allocate(AudioQueueRef, UInt32, AudioQueueBufferRef *);
static OSStatus test_dispose(AudioQueueRef, Boolean);
#define AudioQueueAllocateBuffer test_allocate
#define AudioQueueDispose test_dispose
#include "../src/audio_queue_bridge.c"
#undef AudioQueueAllocateBuffer
#undef AudioQueueDispose

static atomic_bool disposal_requested, native_disposed;
const char *guest_dyld32_describe(uint32_t address, uint32_t *offset)
{ (void)address; (void)offset; return NULL; }
static bool disposed_during_allocate;
static unsigned allocations, guest_frees;
static pthread_t disposer;
static unsigned char audio_data[256];
static AudioQueueBuffer buffer = {.mAudioDataBytesCapacity = sizeof(audio_data), .mAudioData = audio_data};

uint32_t compat_runtime32_allocate(size_t size, int clear)
{
    assert(size <= 4096);
    if (clear) memset((void *)0x40000000, 0, size);
    return 0x40000000;
}
void compat_runtime32_deallocate(uint32_t pointer) { assert(pointer == 0x40000000); ++guest_frees; }
uint32_t compat_runtime32_call(uint32_t f, const uint32_t *a, size_t n)
{ (void)f; (void)a; (void)n; abort(); }

static void *dispose_worker(void *unused)
{
    (void)unused;
    atomic_store(&disposal_requested, true);
    dispose(&queues[0], true);
    return NULL;
}
static OSStatus test_allocate(AudioQueueRef audio, UInt32 capacity, AudioQueueBufferRef *out)
{
    assert(audio == (AudioQueueRef)0x1234 && capacity == sizeof(audio_data));
    ++allocations;
    assert(!pthread_create(&disposer, NULL, dispose_worker, NULL));
    while (!atomic_load(&disposal_requested)) usleep(100);
    /* Leave the allocating thread inside the native call while the disposing
       thread runs. The old bridge lets native disposal finish in this window. */
    for (unsigned i = 0; i < 1000 && !atomic_load(&native_disposed); ++i) usleep(100);
    disposed_during_allocate = atomic_load(&native_disposed);
    *out = &buffer;
    return noErr;
}
static OSStatus test_dispose(AudioQueueRef audio, Boolean immediate)
{
    assert(audio == (AudioQueueRef)0x1234 && immediate);
    atomic_store(&native_disposed, true);
    return noErr;
}
int main(void)
{
    assert(mmap((void *)0x40000000, 4096, PROT_READ | PROT_WRITE,
                MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) == (void *)0x40000000);
    queue_count = 1;
    queues[0].audio = (AudioQueueRef)0x1234;
    queues[0].handle = kQueueHandleBase + 1;
    uint32_t out = 0;
    OSStatus status = allocate_buffer(&queues[0], sizeof(audio_data), &out);
    pthread_join(disposer, NULL);
    if (disposed_during_allocate) {
        fputs("FAIL: native queue disposed before buffer allocation finished\n", stderr);
        return 1;
    }
    assert(status == noErr && out == 0x40000000 && guest_frees == 1);
    assert(atomic_load(&native_disposed) && !queues[0].audio && !queues[0].buffer_count);
    /* A stale lookup must not allocate on a queue already being disposed. */
    assert(allocate_buffer(&queues[0], sizeof(audio_data), &out) == kAudioQueueErr_InvalidQueueType);
    assert(allocations == 1 && guest_frees == 2);
    puts("AudioQueue allocation/disposal race PASS");
    return 0;
}
