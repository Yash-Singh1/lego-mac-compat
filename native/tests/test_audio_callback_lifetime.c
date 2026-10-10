/* CPU-only native callback tests. No device, graph, or audible output. */
#include <pthread.h>
#include <AudioToolbox/AudioToolbox.h>
static OSStatus test_remove_notify(AudioUnit, AURenderCallback, void *);
#define AudioUnitRemoveRenderNotify test_remove_notify
static int controlled_wait(pthread_cond_t *, pthread_mutex_t *);
#define pthread_cond_wait controlled_wait
#include "../src/audio_bridge.c"
#undef pthread_cond_wait
#undef AudioUnitRemoveRenderNotify
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>

/* Delay a release waiter after wakeup, before it resumes with the render
   lock. This forces the scheduling window where the pool can recycle it. */
static _Thread_local bool release_waiter;
static pthread_mutex_t waiter_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t waiter_changed = PTHREAD_COND_INITIALIZER;
static bool waiter_awake, waiter_resume;
static int controlled_wait(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
    int result = pthread_cond_wait(condition, mutex);
    if (release_waiter) {
        pthread_mutex_unlock(mutex);
        pthread_mutex_lock(&waiter_gate);
        waiter_awake = true;
        pthread_cond_broadcast(&waiter_changed);
        while (!waiter_resume) pthread_cond_wait(&waiter_changed, &waiter_gate);
        pthread_mutex_unlock(&waiter_gate);
        pthread_mutex_lock(mutex);
    }
    return result;
}

static struct audio_callback_registration *expected_removal;
static unsigned removal_calls;
static OSStatus test_remove_notify(AudioUnit unit, AURenderCallback callback, void *refcon)
{
    assert(unit == (AudioUnit)(uintptr_t)1 && callback == host_audio_callback);
    assert(refcon == expected_removal);
    ++removal_calls;
    return noErr;
}

static uint32_t cursor = 0x30001000;
const struct lp32_game_profile *lp32_profile(void)
{
    static const struct lp32_game_profile profile = {.title = LP32_TITLE_MW3};
    return &profile;
}
uint32_t compat_runtime32_allocate(size_t size, int clear)
{
    uint32_t pointer = cursor;
    cursor += (size + 15) & ~15u;
    assert(cursor < 0x30400000);
    if (clear) memset((void *)(uintptr_t)pointer, 0, size);
    return pointer;
}
void compat_runtime32_deallocate(uint32_t pointer) { (void)pointer; }
uint32_t compat_runtime32_reallocate(uint32_t pointer, size_t size)
{
    assert(!pointer);
    return compat_runtime32_allocate(size, 1);
}
int compat_runtime32_last_call_trapped(void) { return 0; }
uint32_t compat_runtime32_call(uint32_t function, const uint32_t *arguments, size_t count)
{
    (void)function; (void)arguments; (void)count;
    assert(!"Tests complete each worker job manually");
    return 0;
}
static void manual_workers(void) { audio_worker_available = true; }

struct render_request {
    struct audio_callback_registration *registration;
    unsigned frame_count;
    bool entered, returned;
    OSStatus status;
};
static void *render(void *opaque)
{
    struct render_request *request = opaque;
    __atomic_store_n(&request->entered, true, __ATOMIC_RELEASE);
    request->status = host_audio_callback(request->registration, NULL, NULL,
                                          0, request->frame_count, NULL);
    __atomic_store_n(&request->returned, true, __ATOMIC_RELEASE);
    return NULL;
}
static unsigned queued_jobs(void)
{
    pthread_mutex_lock(&audio_job_lock);
    unsigned count = 0;
    for (struct audio_callback_job *job = audio_job_head; job; job = job->next) ++count;
    pthread_mutex_unlock(&audio_job_lock);
    return count;
}
static void await_job(void)
{
    for (unsigned attempt = 0; attempt < 10000 && !queued_jobs(); ++attempt) usleep(100);
    assert(queued_jobs() == 1);
}
static void complete_job(unsigned frames, OSStatus status)
{
    pthread_mutex_lock(&audio_job_lock);
    struct audio_callback_job *job = audio_job_head;
    assert(job && !job->next);
    audio_job_head = audio_job_tail = NULL;
    struct audio_callback_context *context = job->context;
    pthread_mutex_unlock(&audio_job_lock);
    pthread_mutex_lock(&context->lock);
    assert(context->host_call_active && context->frame_count == frames);
    context->guest_status = status;
    context->response_ready = true;
    pthread_cond_broadcast(&context->response_condition);
    pthread_mutex_unlock(&context->lock);
}
static void check_overlapping_renders(void)
{
    struct audio_callback_context *context = new_audio_callback(8, 99, "unit-input", NULL, 0, 0);
    assert(context);
    struct render_request first = {context->registration, 128, false, false, 0};
    struct render_request second = {context->registration, 256, false, false, 0};
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, render, &first));
    await_job();
    assert(!pthread_create(&b, NULL, render, &second));
    while (!__atomic_load_n(&second.entered, __ATOMIC_ACQUIRE)) usleep(100);
    /* Leave both native invocations pending while all workers are held. */
    usleep(50000);
    assert(queued_jobs() == 1);
    assert(!__atomic_load_n(&first.returned, __ATOMIC_ACQUIRE));
    assert(!__atomic_load_n(&second.returned, __ATOMIC_ACQUIRE));
    complete_job(128, -111);
    assert(!pthread_join(a, NULL) && first.status == -111);
    await_job();
    assert(!__atomic_load_n(&second.returned, __ATOMIC_ACQUIRE));
    complete_job(256, -222);
    assert(!pthread_join(b, NULL) && second.status == -222);
    assert(!queued_jobs() && !context->host_call_active);
    release_audio_callback(context);
}
static void *release_registration(void *raw)
{
    release_waiter = true;
    release_audio_registration(raw);
    release_waiter = false;
    return NULL;
}
static void check_release_waiter_recycling(void)
{
    struct audio_callback_context *context = new_audio_callback(8, 99, "unit-input", NULL, 0, 0);
    struct render_request request = {context->registration, 128, false, false, 0};
    pthread_t host, releasing;
    assert(!pthread_create(&host, NULL, render, &request));
    await_job();
    assert(!pthread_create(&releasing, NULL, release_registration, context->registration));
    while (!__atomic_load_n(&context->retired, __ATOMIC_ACQUIRE)) usleep(100);
    complete_job(128, -111);
    pthread_mutex_lock(&waiter_gate);
    while (!waiter_awake) pthread_cond_wait(&waiter_changed, &waiter_gate);
    pthread_mutex_unlock(&waiter_gate);
    assert(!pthread_join(host, NULL));
    assert(!context->in_use);
    struct audio_callback_context *replacement = new_audio_callback(8, 123, "unit-input", NULL, 0, 0);
    assert(replacement == context);
    struct audio_callback_registration *registration = replacement->registration;
    pthread_mutex_lock(&waiter_gate);
    waiter_resume = true;
    pthread_cond_broadcast(&waiter_changed);
    pthread_mutex_unlock(&waiter_gate);
    assert(!pthread_join(releasing, NULL));
    assert(replacement->in_use && replacement->registration == registration);
    assert(replacement->guest_refcon == 123 && !replacement->retired);
    release_audio_callback(replacement);
}

static void check_delayed_removal(void)
{
    struct audio_callback_context *context = new_audio_callback(8, 99, "render-notify", NULL, 0, 0);
    struct deferred_graph_op removal = {.kind = kDeferredUnitRemoveRenderNotify,
        .unit = (AudioUnit)(uintptr_t)1, .registration = context->registration};
    expected_removal = context->registration;
    release_audio_callback(context);
    struct audio_callback_context *replacement = new_audio_callback(8, 123, "unit-input", NULL, 0, 0);
    assert(replacement == context);
    struct audio_callback_registration *registration = replacement->registration;
    perform_graph_op(&removal);
    assert(removal_calls == 1);
    assert(replacement->in_use && replacement->registration == registration);
    assert(replacement->guest_refcon == 123);
    release_audio_callback(replacement);
}

static void check_retired_and_reused_callbacks(void)
{
    size_t page = getpagesize();
    void *inaccessible = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(inaccessible != MAP_FAILED);
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
        struct audio_callback_context *context = new_audio_callback(8, 99,
            iteration & 1 ? "render-notify" : "unit-input", NULL, 0, 0);
        assert(context);
        struct audio_callback_registration *old = context->registration;
        __atomic_store_n(&context->retired, true, __ATOMIC_RELEASE);
        /* A retired invocation must not even read its old AudioBufferList. */
        assert(!host_audio_callback(old, NULL, NULL, 0, 128, inaccessible));
        release_audio_callback(context);
        struct audio_callback_context *replacement = new_audio_callback(8, 99, "unit-input", NULL, 0, 0);
        assert(replacement == context && replacement->registration != old);
        release_audio_registration(old);
        assert(replacement->in_use && replacement->guest_function == 8 && !replacement->retired);
        AudioUnitRenderActionFlags flags = kAudioUnitRenderAction_PreRender;
        assert(!host_audio_callback(old, &flags, NULL, 0, 128, inaccessible));
        current_audio_callback = replacement;
        assert(host_audio_callback(replacement->registration, NULL, NULL, 0, 128, inaccessible)
            == kAudioUnitErr_CannotDoInCurrentContext);
        current_audio_callback = NULL;
        assert(!queued_jobs());
        release_audio_callback(replacement);
    }
    assert(audio_callback_count == 1);
    assert(!munmap(inaccessible, page));
}
int main(void)
{
    alarm(15);
    assert(mmap((void *)0x30000000, 0x400000, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != MAP_FAILED);
    pthread_once(&audio_worker_once, manual_workers);
    check_overlapping_renders();
    check_release_waiter_recycling();
    check_delayed_removal();
    check_retired_and_reused_callbacks();
    puts("Audio callback lifetime PASS (independent overlapping responses, recycled context while release waits, delayed notification removal, 1000 retired/context-reuse cycles with inaccessible native lists, recursive-render rejection)");
}
