#include "../src/audio_bridge.c"
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>

static uint32_t cursor = 0x30001000, test_unit;
static unsigned callbacks;
static pthread_mutex_t sound_lock = PTHREAD_MUTEX_INITIALIZER;
static bool block_callback, callback_entered;
const struct lp32_game_profile *lp32_profile(void) {
    static const struct lp32_game_profile profile = {.title = LP32_TITLE_COD4};
    return &profile;
}
uint32_t compat_runtime32_allocate(size_t size, int clear) {
    uint32_t p = cursor; cursor += (size + 15) & ~15u;
    assert(cursor < 0x30400000);
    if (clear) memset((void *)(uintptr_t)p, 0, size);
    return p;
}
void compat_runtime32_deallocate(uint32_t p) { (void)p; }
uint32_t compat_runtime32_reallocate(uint32_t p, size_t size) {
    assert(!p); return compat_runtime32_allocate(size, 1);
}
int compat_runtime32_last_call_trapped(void) { return 0; }
uint32_t compat_runtime32_call(uint32_t function, const uint32_t *a, size_t n) {
    if (function == 9 || function == 10) {
        assert(n == 6 && a[0] == 99);
        assert(function == 9 ? !a[5] : a[5] != 0);
        return 0;
    }
    if(function==8){
        assert(n==6 && a[0]==99 && a[4]==128);
        if (block_callback) {
            __atomic_store_n(&callback_entered, true, __ATOMIC_RELEASE);
            pthread_mutex_lock(&sound_lock);
            pthread_mutex_unlock(&sound_lock);
        }
        ++callbacks;return 0;
    }
    assert(function == 7 && n == 6 && a[0] == 42 && a[4] == 128);
    ++callbacks;
    uint64_t result;
    uint32_t remove[] = {test_unit, 7, 42};
    assert(audio_bridge32_dispatch("_AudioUnitRemoveRenderNotify", remove, &result) && !result);
    /* A query after self-removal must not drain the teardown that is waiting
       for this callback to release its context lock. */
    *(uint32_t *)0x30000004 = 4;
    uint32_t query[] = {test_unit, kAudioUnitProperty_MaximumFramesPerSlice,
                       kAudioUnitScope_Global, 0, 0x30000000, 0x30000004};
    assert(audio_bridge32_dispatch("_AudioUnitGetProperty", query, &result) && !result);
    return 0;
}
static void *render_blocked_callback(void *opaque) {
    AudioUnitRenderActionFlags flags = 0;
    assert(!host_audio_callback(opaque, &flags, NULL, 0, 128, NULL));
    return NULL;
}

static struct audio_callback_context *installed_input(AudioUnit unit) {
    for (unsigned i = 0; i < kAudioCallbackCapacity; ++i)
        if (unit_input_bindings[i].unit == unit && unit_input_bindings[i].context)
            return unit_input_bindings[i].context;
    return NULL;
}

static void check_replacement_during_render(AudioUnit unit, uint32_t *set) {
    uint32_t *guest = (void *)(uintptr_t)set[4];
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        uint64_t result;
        guest[0] = 8; guest[1] = 99;
        assert(audio_bridge32_dispatch("_AudioUnitSetProperty", set, &result) && !result);
        struct audio_callback_context *old = installed_input(unit);
        assert(old);
        pthread_mutex_lock(&sound_lock);
        callback_entered = false;
        block_callback = true;
        pthread_t render;
        assert(!pthread_create(&render, NULL, render_blocked_callback, old));
        while (!__atomic_load_n(&callback_entered, __ATOMIC_ACQUIRE)) usleep(100);

        /* COD4 replaces the callback while holding the lock its old render
           is trying to acquire. Both replacement and removal must return. */
        if (iteration & 1) guest[0] = guest[1] = 0;
        assert(audio_bridge32_dispatch("_AudioUnitSetProperty", set, &result) && !result);
        struct audio_callback_context *replacement = installed_input(unit);
        assert((iteration & 1) ? !replacement : replacement && replacement != old);
        assert(__atomic_load_n(&old->retired, __ATOMIC_ACQUIRE));
        assert(old->in_use && old->guest_function == 8 && old->guest_refcon == 99);
        guest[0] = guest[1] = 0;
        assert(audio_bridge32_dispatch("_AudioUnitSetProperty", set, &result) && !result);
        assert(!installed_input(unit) && (!replacement || !replacement->in_use));
        pthread_mutex_unlock(&sound_lock);
        assert(!pthread_join(render, NULL));
        block_callback = false;
        assert(!old->in_use && !old->host_call_active);
    }
    /* Retired contexts must be reclaimed, not accumulate per sound. */
    assert(audio_callback_count <= 3);
}

int main(void) {
    alarm(30);
    setenv("LP32_MUTE_AUDIO", "1", 1);
    assert(mmap((void *)0x30000000, 0x400000, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != MAP_FAILED);
    AUGraph graph; AUNode node; AudioUnit unit;
    AudioComponentDescription description = {kAudioUnitType_Output,
        kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
    assert(!NewAUGraph(&graph));
    assert(!AUGraphAddNode(graph, &description, &node));
    assert(!AUGraphOpen(graph));
    assert(!AUGraphNodeInfo(graph, node, NULL, &unit));
    test_unit = guest_handle_for_unit(unit, graph);
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
    uint32_t add[] = {test_unit, 7, 42}; uint64_t result;
    assert(audio_bridge32_dispatch("_AudioUnitAddRenderNotify", add, &result) && !result);
    struct audio_callback_context *context = audio_callbacks[audio_callback_count - 1];
    AudioUnitRenderActionFlags flags = kAudioUnitRenderAction_PostRender;
    assert(!host_audio_callback(context, &flags, NULL, 0, 128, NULL));
    drain_deferred_graph_work(graph, false);
    assert(callbacks == iteration + 1 && !context->in_use);
    }
    uint32_t *guest=(void *)0x30000010;guest[0]=8;guest[1]=99;guest[2]=0xabcdef01;
    uint32_t set[]={test_unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,0x30000010,8};uint64_t result;
    assert(audio_bridge32_dispatch("_AudioUnitSetProperty",set,&result) && !result);
    struct audio_callback_context *context=audio_callbacks[audio_callback_count-1];
    assert(context->in_use && context->owner_unit==unit && context->guest_function==8 && context->guest_refcon==99);
    AudioUnitRenderActionFlags flags=0;AudioTimeStamp timestamp={0};
    assert(!host_audio_callback(context,&flags,&timestamp,0,128,NULL));
    assert(callbacks==101 && guest[2]==0xabcdef01);
    guest[0]=guest[1]=0;
    assert(audio_bridge32_dispatch("_AudioUnitSetProperty",set,&result) && !result && !context->in_use);
    check_replacement_during_render(unit, set);
    assert(callbacks == 201);
    struct audio_callback_context *notify = new_audio_callback(9, 99, "render-notify", graph, 0, 0);
    size_t page = (size_t)getpagesize();
    void *unprepared = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(unprepared != MAP_FAILED);
    AudioBufferList list = {1, {{1, 16, unprepared}}};
    flags = kAudioUnitRenderAction_PreRender;
    assert(!host_audio_callback(notify, &flags, NULL, 0, 128, &list));
    /* Neither skipped notifications nor muted test playback own the mix. */
    flags = kAudioUnitRenderAction_PostRender;
    notify->muted = true;
    assert(!host_audio_callback(notify, &flags, NULL, 0, 128, &list));
    notify->muted = false;
    hold_last_frame_ns = monotonic_ns() - 200000000;
    assert(!host_audio_callback(notify, &flags, NULL, 0, 128, &list));
    hold_last_frame_ns = 0;
    float mixed[] = {0.25f, -0.5f, 0.75f, -1};
    float original[4]; memcpy(original, mixed, sizeof(mixed));
    list.mBuffers[0].mData = mixed;
    notify->guest_function = 10;
    assert(!host_audio_callback(notify, &flags, NULL, 0, 128, &list));
    assert(!memcmp(mixed, original, sizeof(mixed)));
    release_audio_callback(notify);
    assert(!munmap(unprepared, page));
    assert(!DisposeAUGraph(graph));
    puts("Audio render notify PASS (callback ABI, removal, 100 locked-render replacements, reclamation, protected pre-render buffers, skipped/muted notifications, teardown)");
}
