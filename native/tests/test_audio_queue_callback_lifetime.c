/* CPU-only: AudioQueueDispose(false) can return while an output callback is
 * active. Releasing its guest record then lets another allocation replace
 * memory that the callback still reads or writes. */
#include <AudioToolbox/AudioToolbox.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>
static OSStatus mock_dispose(AudioQueueRef, Boolean);
#define AudioQueueDispose mock_dispose
#ifndef AUDIO_QUEUE_BRIDGE_SOURCE
#define AUDIO_QUEUE_BRIDGE_SOURCE "../src/audio_queue_bridge.c"
#endif
#include AUDIO_QUEUE_BRIDGE_SOURCE
#undef AudioQueueDispose

static atomic_uint entered, returned, guest_frees;
static atomic_bool proceed, proceed_first;
static bool self_dispose;
static OSStatus native_status;
static struct queue *tested_queue;
static AudioQueueBuffer native_buffer;
const char *guest_dyld32_describe(uint32_t a, uint32_t *o)
{ (void)a; (void)o; return NULL; }
uint32_t compat_runtime32_allocate(size_t n, int clear)
{ (void)n; (void)clear; abort(); }
void compat_runtime32_deallocate(uint32_t p)
{
    assert(p == 0x40000000);
    /* Make stale guest writes fail instead of silently touching a reused
       allocation. The next case restores this page's permissions. */
    assert(!mprotect((void *)(uintptr_t)p, 4096, PROT_NONE));
    atomic_fetch_add(&guest_frees, 1);
}
static OSStatus mock_dispose(AudioQueueRef audio, Boolean immediate)
{
    assert(audio == (AudioQueueRef)0x1234);
    (void)immediate;
    return native_status;
}
uint32_t compat_runtime32_call(uint32_t function, const uint32_t *args, size_t count)
{
    assert(function == 1 && count == 3 && args[1] == tested_queue->handle && args[2] == 0x40000000);
    unsigned ordinal = atomic_fetch_add(&entered, 1);
    if (self_dispose) {
        assert(!dispose(tested_queue, false));
        assert(!atomic_load(&guest_frees));
    } else {
        while (!atomic_load(&proceed) && !(ordinal == 0 && atomic_load(&proceed_first))) usleep(100);
    }
    /* Guest callbacks may continue using their record after native disposal
       has returned, including disposal requested by the callback itself. */
    assert(!atomic_load(&guest_frees));
    __atomic_store_n(&((struct guest_buffer32 *)(uintptr_t)args[2])->user_data, 0xfeed, __ATOMIC_RELAXED);
    atomic_fetch_add(&returned, 1);
    return 0;
}
static void *run_callback(void *p)
{
    struct queue *q = p;
    output(q, q->audio, &native_buffer);
    return NULL;
}
static void setup(unsigned index)
{
    assert(!mprotect((void *)0x40000000, 4096, PROT_READ | PROT_WRITE));
    queue_count = index + 1;
    tested_queue = &queues[index];
    *tested_queue = (struct queue){.audio = (AudioQueueRef)0x1234,
        .handle = kQueueHandleBase + index + 1, .callback = 1, .buffer_count = 1};
    native_buffer.mUserData = (void *)0x40000000;
    tested_queue->buffers[0].guest = 0x40000000;
    tested_queue->buffers[0].host = &native_buffer;
    atomic_store(&entered, 0); atomic_store(&returned, 0); atomic_store(&guest_frees, 0);
    atomic_store(&proceed, false); atomic_store(&proceed_first, false);
    native_status = noErr; self_dispose = false;
}
static void wait_entered(unsigned count)
{
    for (unsigned i = 0; i < 10000 && atomic_load(&entered) != count; ++i) usleep(100);
    assert(atomic_load(&entered) == count);
}
int main(void)
{
    assert(mmap((void *)0x40000000, 4096, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == (void *)0x40000000);
    for (unsigned callbacks = 1; callbacks <= 2; ++callbacks) {
        setup(callbacks - 1);
        pthread_t threads[2];
        for (unsigned i = 0; i < callbacks; ++i) assert(!pthread_create(&threads[i], NULL, run_callback, tested_queue));
        wait_entered(callbacks);
        assert(!dispose(tested_queue, false));
        if (atomic_load(&guest_frees)) {
            fputs("FAIL: asynchronous disposal freed an active callback's guest buffer\n", stderr);
            return 1;
        }
        assert(!tested_queue->audio && tested_queue->buffer_count == 1);
        if (callbacks == 2) {
            atomic_store(&proceed_first, true);
            for (unsigned i = 0; i < 10000 && !atomic_load(&returned); ++i) usleep(100);
            assert(atomic_load(&returned) == 1 && !atomic_load(&guest_frees));
        }
        atomic_store(&proceed, true);
        for (unsigned i = 0; i < callbacks; ++i) pthread_join(threads[i], NULL);
        assert(atomic_load(&returned) == callbacks && atomic_load(&guest_frees) == 1);
        assert(!tested_queue->buffer_count);
        /* Native disposal can deliver late callbacks. Neither their native
           buffers nor their already reclaimed guest buffers may be touched. */
        output(tested_queue, (AudioQueueRef)0x1234, (void *)0x40000000);
        assert(atomic_load(&guest_frees) == 1);
    }
    setup(2); self_dispose = true;
    output(tested_queue, tested_queue->audio, &native_buffer);
    assert(atomic_load(&returned) == 1 && atomic_load(&guest_frees) == 1);
    assert(!tested_queue->buffer_count);
    setup(3);
    pthread_t thread; assert(!pthread_create(&thread, NULL, run_callback, tested_queue));
    wait_entered(1);
    native_status = kAudio_ParamError;
    assert(dispose(tested_queue, false) == native_status);
    assert(tested_queue->audio && !tested_queue->disposing && !atomic_load(&guest_frees));
    atomic_store(&proceed, true); pthread_join(thread, NULL);
    assert(!atomic_load(&guest_frees));
    native_status = noErr;
    assert(!dispose(tested_queue, true));
    assert(atomic_load(&guest_frees) == 1);
    puts("AudioQueue callback lifetime PASS (asynchronous disposal, overlapping callbacks, self-disposal, late callbacks, failed disposal)");
    return 0;
}
