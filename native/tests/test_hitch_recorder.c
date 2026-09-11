#include "../src/hitch_recorder.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static uint64_t end = 1000000000ULL, swap;
static void frame(uint64_t duration, uint64_t target, bool active)
{
    end += duration;
    hitch_frame(++swap, end - 2000000, end - 1000000, end, target, active);
}
static void *worker(void *unused)
{
    (void)unused;
    hitch_note(HITCH_SHADER, "cgCreateProgram", 0x1234, end, end + 9000000, 0);
    hitch_count_runtime(); /* Worker calls must not change render-frame counts. */
    return NULL;
}
int main(void)
{
    /* Keep the recorder in the same nanosecond domain as frame pacing. */
    for (unsigned i = 0; i < 1000; ++i) {
        uint64_t before = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        uint64_t measured = hitch_now();
        uint64_t after = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        assert(before <= measured && measured <= after);
    }
    assert(hitch_classify("glDrawRangeElements") == HITCH_DRAW);
    assert(hitch_classify("_glDrawArrays") == HITCH_DRAW);
    assert(hitch_classify("_glDrawBuffers") == HITCH_GL_STATE);
    assert(hitch_classify("_glCompressedTexSubImage2DARB") == HITCH_UPLOAD);
    assert(hitch_classify("_glProgramStringARB") == HITCH_SHADER);
    assert(hitch_classify("_cgCreateProgram") == HITCH_SHADER);
    assert(hitch_classify("_fread") == HITCH_IO);
    assert(hitch_classify("_FSReadFork") == HITCH_IO);
    assert(hitch_classify("_PBReadForkAsync") == HITCH_IO);
    assert(hitch_classify("_MPWaitOnSemaphore") == HITCH_WAIT);
    assert(hitch_classify("_MPWaitOnQueue") == HITCH_WAIT);
    assert(hitch_classify("_MPDelayUntil") == HITCH_WAIT);
    assert(hitch_classify("_usleep") == HITCH_WAIT);
    assert(hitch_classify("_lp32_steam_4_0") == HITCH_IO);
    assert(hitch_classify("_lp32_steam_4_16") == HITCH_IO);
    assert(hitch_classify("_lp32_steam_1_0") == HITCH_RUNTIME);
    assert(hitch_classify("_pthread_cond_wait$UNIX2003") == HITCH_WAIT);
    assert(hitch_classify("_glProgramEnvParameters4fvEXT") == HITCH_GL_STATE);
    assert(hitch_classify("_objc_msgSend") == HITCH_OBJC);
    assert(hitch_classify("_AudioUnitRender") == HITCH_AUDIO);
    assert(hitch_classify("_strlen") == HITCH_COUNTED_RUNTIME);
    assert(hitch_classify("___tolower") == HITCH_COUNTED_RUNTIME);
    assert(hitch_classify("_memcpy") == HITCH_COUNTED_RUNTIME);
    assert(hitch_classify("_pthread_self") == HITCH_COUNTED_RUNTIME);
    assert(hitch_classify("_malloc") == HITCH_RUNTIME);
    assert(hitch_classify("_free") == HITCH_RUNTIME);
    assert(!hitch_recorder_enabled);
    char path[128];
    snprintf(path, sizeof(path), "/tmp/lp32-hitch-test-%ld.log", (long)getpid());
    assert(!hitch_start(path, 25));
    const char *full = getenv("LP32_HITCH_FULL_IMPORTS");
    assert(!!hitch_full_imports == !!(full && full[0] && strcmp(full, "0")));
    assert(hitch_classify("_strlen") == (hitch_full_imports ? HITCH_RUNTIME : HITCH_COUNTED_RUNTIME));
    assert(hitch_classify("_pthread_mutex_lock") == HITCH_WAIT);
    /* Warm-up, ring wrap, intentional 30 FPS, and inactive frames are quiet. */
    for (int i = 0; i < 50; ++i) frame(16666667, 16666667, true);
    for (int i = 0; i < 40; ++i) frame(33333333, 33333333, true);
    for (int i = 0; i < 40; ++i) frame(50000000, 16666667, false);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, worker, NULL));
    pthread_join(thread, NULL);
    for (int i = 0; i < 17; ++i) hitch_count_runtime();
    struct hitch_scope outer, inner, presentation;
    hitch_scope_begin(&outer, end);
    hitch_scope_begin(&inner, end + 2000000);
    hitch_scope_end(&inner, HITCH_RUNTIME, "nested", 0, end + 6000000, 0);
    hitch_scope_end(&outer, HITCH_GL_STATE, "outer", 0, end + 10000000, 0);
    hitch_program(0x8620, 17); hitch_program(0x8804, 28);
    hitch_note(HITCH_DRAW, "glDrawRangeElements", 0xabcdef, end, end + 40000000, 600);
    hitch_scope_begin(&presentation, end);
    frame(45000000, 16666667, true);
    hitch_scope_end(&presentation, HITCH_OBJC, "presentation", 0, end, 0);
    for (int i = 0; i < 8; ++i) frame(16666667, 16666667, true);
    /* Another hitch within five seconds must not generate another report. */
    frame(45000000, 16666667, true);
    usleep(20000);
    /* Benchmark selected-call timing without driver work or per-call logging. */
    uint64_t before = hitch_now();
    for (int i = 0; i < 100000; ++i) {
        uint64_t start = hitch_now();
        hitch_note(HITCH_DRAW, "glDrawArrays", 0, start, hitch_now(), 3);
    }
    printf("recorder selected-call overhead: %.0f ns/call\n", (hitch_now() - before) / 100000.0);
    before = hitch_now();
    for (int i = 0; i < 100000; ++i) {
        struct hitch_scope scope;
        hitch_scope_begin(&scope, hitch_now());
        hitch_scope_end(&scope, HITCH_RUNTIME, "benchmark", 0, hitch_now(), 0);
    }
    printf("recorder exclusive-call overhead: %.0f ns/call\n", (hitch_now() - before) / 100000.0);
    hitch_stop();
    FILE *f = fopen(path, "r"); assert(f);
    char text[65536]; size_t count = fread(text, 1, sizeof(text) - 1, f);
    text[count] = 0; fclose(f);
    assert(strstr(text, "hitch trigger=131 frames=41"));
    assert(strstr(text, " frame=99 "));
    assert(!strstr(text, " frame=98 "));
    assert(strstr(text, " frame=139 "));
    assert(strstr(text, "present=45.000 work=43.000 flush=1.000 pace=1.000"));
    assert(strstr(text, "draw=1/40.000"));
    assert(strstr(text, " counted_runtime=17\n"));
    assert(!strstr(text, " counted_runtime=18\n"));
    assert(strstr(text, "glstate=1/6.000 objc=0/0.000 runtime=1/4.000"));
    assert(!strstr(text, "objc=1/"));
    assert(!strstr(text, "name=presentation"));
    assert(strstr(text, "host model="));
    assert(strstr(text, "rosetta="));
    assert(strstr(text, "caller=00abcdef vp=17 fp=28 count=600"));
    assert(strstr(text, "worker name=cgCreateProgram"));
    assert(strstr(text, "end reports=1 skipped=0"));
    unlink(path);
    puts("hitch-recorder PASS (threshold, history, workers, cooldown, exclusive nesting, presentation boundaries, host metadata)");
    return 0;
}
