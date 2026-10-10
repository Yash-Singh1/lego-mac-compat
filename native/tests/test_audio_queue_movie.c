/* CPU-only AudioQueue dispatch regression. No native audio device or game
 * is opened. Movie playback must exempt the game loop from background
 * throttling only for the lifetime of a successfully started Bink queue. */
#include <AudioToolbox/AudioToolbox.h>
#include <assert.h>
#include <sys/mman.h>

static OSStatus mock_new(const AudioStreamBasicDescription *, AudioQueueOutputCallback,
                         void *, CFRunLoopRef, CFStringRef, UInt32, AudioQueueRef *);
static OSStatus mock_start(AudioQueueRef, const AudioTimeStamp *);
static OSStatus mock_stop(AudioQueueRef, Boolean);
static OSStatus mock_pause(AudioQueueRef);
static OSStatus mock_dispose(AudioQueueRef, Boolean);
#define AudioQueueNewOutput mock_new
#define AudioQueueStart mock_start
#define AudioQueueStop mock_stop
#define AudioQueuePause mock_pause
#define AudioQueueDispose mock_dispose
#include "../src/audio_queue_bridge.c"
#undef AudioQueueNewOutput
#undef AudioQueueStart
#undef AudioQueueStop
#undef AudioQueuePause
#undef AudioQueueDispose

static OSStatus next_status;
static bool dispose_during_start;
static unsigned guest_frees;
static uintptr_t next_audio = 0x1000;
static const char *modules[] = {
    NULL, "/game/Contents/MacOS/libBinkMacx86.dylib",
    "/game/libBink2Macx86.dylib", "libBinkMachOx86.dylib",
    "/game/libMilesX86.dylib", "/libBinkMacx86.dylib/not-bink.dylib"
};
const char *guest_dyld32_describe(uint32_t address, uint32_t *offset)
{
    assert(address < sizeof(modules) / sizeof(modules[0]));
    *offset = 0x23c0;
    return modules[address];
}
uint32_t compat_runtime32_allocate(size_t size, int clear)
{ (void)size; (void)clear; abort(); }
void compat_runtime32_deallocate(uint32_t pointer) { assert(pointer == 0x40000200); ++guest_frees; }
uint32_t compat_runtime32_call(uint32_t f, const uint32_t *a, size_t n)
{ (void)f; (void)a; (void)n; abort(); }

static OSStatus mock_new(const AudioStreamBasicDescription *format, AudioQueueOutputCallback callback,
                         void *data, CFRunLoopRef loop, CFStringRef mode, UInt32 flags, AudioQueueRef *out)
{
    (void)format; (void)callback; (void)data;
    assert(!loop && !mode && !flags);
    if (!next_status) *out = (AudioQueueRef)++next_audio;
    return next_status;
}
static OSStatus mock_start(AudioQueueRef audio, const AudioTimeStamp *when)
{
    assert(audio && !when);
    if (dispose_during_start) {
        for (unsigned i = 0; i < queue_count; ++i)
            if (queues[i].audio == audio) dispose(&queues[i], true);
    }
    return next_status;
}
static OSStatus mock_stop(AudioQueueRef audio, Boolean immediate)
{ assert(audio); (void)immediate; return next_status; }
static OSStatus mock_pause(AudioQueueRef audio) { assert(audio); return next_status; }
static OSStatus mock_dispose(AudioQueueRef audio, Boolean immediate)
{ assert(audio); (void)immediate; return next_status; }

static uint32_t create(uint32_t callback)
{
    uint32_t *out = (void *)0x40000100;
    *out = 0;
    uint32_t a[] = {0x40000000, callback, 0, 0, 0, 0, 0x40000100};
    uint64_t result;
    assert(audio_queue_bridge32_dispatch("_AudioQueueNewOutput", a, &result));
    assert((int32_t)result == next_status);
    assert(next_status ? !*out : *out != 0);
    return *out;
}
static void operation(const char *name, uint32_t queue, int expected_playing)
{
    uint32_t a[] = {queue, 0};
    uint64_t result;
    assert(audio_queue_bridge32_dispatch(name, a, &result));
    assert((int32_t)result == next_status);
    assert(audio_queue_bridge32_movie_playing() == expected_playing);
}
int main(void)
{
    unsetenv("LP32_MUTE_AUDIO");
    assert(mmap((void *)0x40000000, 4096, PROT_READ | PROT_WRITE,
                MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) == (void *)0x40000000);
    assert(!audio_queue_bridge32_movie_playing());
    /* Unknown callbacks and Miles gameplay audio retain background pacing. */
    for (uint32_t callback = 0; callback <= 5; ++callback) {
        uint32_t q = create(callback);
        bool movie = callback >= 1 && callback <= 3;
        assert(!audio_queue_bridge32_movie_playing());
        operation("_AudioQueueStart", q, movie);
        operation("_AudioQueueStart", q, movie);
        operation("_AudioQueuePause", q, false);
        operation("_AudioQueueStart", q, movie);
        operation("_AudioQueueStop", q, false);
        operation("_AudioQueueStart", q, movie);
        operation("_AudioQueueDispose", q, false);
    }
    uint32_t a = create(1), b = create(2);
    operation("_AudioQueueStart", a, true);
    operation("_AudioQueueStart", b, true);
    operation("_AudioQueueDispose", a, true);
    operation("_AudioQueuePause", b, false);
    /* Failed native operations must not change playback state. */
    next_status = kAudio_ParamError;
    assert(!create(1));
    operation("_AudioQueueStart", b, false);
    next_status = noErr;
    operation("_AudioQueueStart", b, true);
    next_status = kAudio_ParamError;
    operation("_AudioQueuePause", b, true);
    operation("_AudioQueueStop", b, true);
    operation("_AudioQueueDispose", b, true);
    next_status = noErr;
    operation("_AudioQueueDispose", b, false);
    /* A start returning after disposal cannot resurrect the exemption. */
    uint32_t c = create(1);
    dispose_during_start = true;
    operation("_AudioQueueStart", c, false);
    dispose_during_start = false;
    /* A running callback pins its guest record through self-disposal.
       Once it returns the record can be reclaimed, without leaking it. */
    uint32_t d = create(1);
    struct queue *q = queue_for(d);
    q->buffer_count = 1;
    q->buffers[0].guest = 0x40000200;
    q->callbacks = 1;
    callback_queue = q;
    operation("_AudioQueueDispose", d, false);
    callback_queue = NULL;
    assert(!guest_frees);
    pthread_mutex_lock(&lock);
    --q->callbacks;
    unsigned retired = take_retired_buffers(q);
    pthread_mutex_unlock(&lock);
    release_guest_buffers(q, retired);
    assert(guest_frees == 1);
    uint32_t e = create(1);
    q = queue_for(e);
    q->buffer_count = 1;
    q->buffers[0].guest = 0x40000200;
    operation("_AudioQueueDispose", e, false);
    assert(guest_frees == 2);
    /* Refills must not erase the lifecycle leading up to allocator failure. */
    for (unsigned i = 0; i < kHistory + 32; ++i)
        note_event(kEventEnqueue, e, NULL, 1408, noErr);
    int fds[2];
    assert(!pipe(fds));
    audio_queue_bridge32_crash_report(fds[1], -1, NULL, 0);
    close(fds[1]);
    char report[16384];
    ssize_t length = read(fds[0], report, sizeof(report) - 1);
    assert(length > 0);
    report[length] = 0;
    assert(strstr(report, "audio queue lifecycle operations"));
    assert(strstr(report, "new-begin") && strstr(report, "dispose-begin"));
    close(fds[0]);
    puts("AudioQueue movie pacing lifecycle PASS (refill-independent crash history)");
    return 0;
}
