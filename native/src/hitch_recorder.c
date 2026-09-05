#include "hitch_recorder.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

/* Fixed storage; no allocation, driver queries, or file writes on a draw.
 * One render-thread producer, one log writer, and a small worker-event ring.
 * At most 128 reports per session, each with 32 preceding and 8 following
 * frames. A five-second cooldown prevents sustained low FPS flooding disk. */
enum { HISTORY = 32, AFTER = 8, TOP = 3, WORKERS = 16, REPORT_LIMIT = 128 };
struct event {
    const char *name;
    uint64_t start, ns;
    uint32_t caller, vp, fp, count;
};
struct frame {
    uint64_t swap, end, work, flush, pace, present;
    uint64_t count[HITCH_KIND_COUNT], ns[HITCH_KIND_COUNT];
    struct event top[TOP];
    bool active;
};
struct report {
    struct frame frames[HISTORY + AFTER + 1];
    struct event workers[WORKERS];
    unsigned count, worker_count;
    uint64_t trigger;
};
int hitch_recorder_enabled;
static FILE *output;
static pthread_t render_thread, writer;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static bool stopping, queued;
static struct report pending, queue;
static struct frame history[HISTORY], current;
static struct event workers[WORKERS];
static unsigned history_count, history_next, worker_next, worker_count;
static unsigned following, reports, skipped;
static uint64_t previous_end, last_trigger, threshold_ns;
static _Thread_local uint32_t vertex_program, fragment_program;

uint64_t hitch_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

unsigned hitch_classify(const char *name)
{
    if (*name == '_') ++name;
    /* Steam RemoteStorage uses generated C++ vtable thunks rather than libc
     * file imports, so include these in I/O attribution too. */
    if (!strncmp(name, "lp32_steam_4_", 13)) return HITCH_IO;
    if (!strncmp(name, "glDraw", 6) &&
        (strstr(name, "Elements") || strstr(name, "Arrays"))) return HITCH_DRAW;
    if (strstr(name, "TexImage") || strstr(name, "TexSubImage") ||
        strstr(name, "BufferData") || strstr(name, "BufferSubData") ||
        strstr(name, "MapBuffer") || strstr(name, "UnmapBuffer") ||
        strstr(name, "GenerateMipmap")) return HITCH_UPLOAD;
    if (!strcmp(name, "cgCreateProgram") || !strcmp(name, "cgCompileProgram") ||
        !strcmp(name, "glProgramStringARB") || !strcmp(name, "glCompileShader") ||
        !strcmp(name, "glLinkProgram")) return HITCH_SHADER;
    if (!strcmp(name, "fread") || !strcmp(name, "fwrite") ||
        !strncmp(name, "read$", 5) || !strcmp(name, "read") ||
        !strncmp(name, "pread", 5) || !strncmp(name, "pwrite", 6) ||
        !strcmp(name, "write") || !strncmp(name, "write$", 6) ||
        !strcmp(name, "fopen") || !strcmp(name, "fclose")) return HITCH_IO;
    if (!strncmp(name, "pthread_mutex_lock", 18) ||
        !strncmp(name, "pthread_cond_wait", 17) ||
        !strncmp(name, "pthread_cond_timedwait", 22) ||
        !strcmp(name, "glFinish") || !strcmp(name, "glFlush") ||
        strstr(name, "WaitSync")) return HITCH_WAIT;
    if (!strncmp(name, "AudioUnit", 9) || !strncmp(name, "AudioConverter", 14) ||
        !strncmp(name, "AudioFile", 9) || !strncmp(name, "ExtAudioFile", 12)) return HITCH_AUDIO;
    return HITCH_NONE;
}

static void write_event(const char *label, const struct event *e)
{
    fprintf(output, "  %s name=%s start=%.3f ms=%.3f caller=%08x vp=%u fp=%u count=%u\n",
            label, e->name, e->start / 1e9, e->ns / 1e6,
            e->caller, e->vp, e->fp, e->count);
}
static void write_report(const struct report *r)
{
    fprintf(output, "hitch trigger=%llu frames=%u\n", (unsigned long long)r->trigger, r->count);
    for (unsigned i = 0; i < r->count; ++i) {
        const struct frame *f = &r->frames[i];
        fprintf(output, " frame=%llu end=%.3f active=%d present=%.3f work=%.3f flush=%.3f pace=%.3f",
                (unsigned long long)f->swap, f->end / 1e9, f->active,
                f->present / 1e6, f->work / 1e6, f->flush / 1e6, f->pace / 1e6);
        static const char *names[] = {"unknown", "none", "draw", "upload", "shader", "io", "wait", "audio"};
        for (unsigned k = HITCH_DRAW; k < HITCH_KIND_COUNT; ++k)
            fprintf(output, " %s=%llu/%.3f", names[k], (unsigned long long)f->count[k], f->ns[k] / 1e6);
        fputc('\n', output);
        for (unsigned j = 0; j < TOP; ++j)
            if (f->top[j].name) write_event("call", &f->top[j]);
    }
    for (unsigned i = 0; i < r->worker_count; ++i) write_event("worker", &r->workers[i]);
    fflush(output);
}
static void *write_loop(void *unused)
{
    (void)unused;
    struct report local;
    for (;;) {
        pthread_mutex_lock(&mutex);
        while (!queued && !stopping) pthread_cond_wait(&ready, &mutex);
        if (!queued && stopping) { pthread_mutex_unlock(&mutex); break; }
        local = queue;
        queued = false;
        pthread_mutex_unlock(&mutex);
        write_report(&local);
    }
    return NULL;
}
static void enqueue(void)
{
    pthread_mutex_lock(&mutex);
    if (!queued) {
        pending.worker_count = 0;
        uint64_t oldest = pending.frames[0].end - pending.frames[0].present;
        uint64_t newest = pending.frames[pending.count - 1].end;
        for (unsigned i = 0; i < worker_count; ++i) {
            const struct event *e = &workers[(worker_next + WORKERS - worker_count + i) % WORKERS];
            if (e->start <= newest && e->start + e->ns >= oldest)
                pending.workers[pending.worker_count++] = *e;
        }
        queue = pending;
        queued = true;
        ++reports;
        pthread_cond_signal(&ready);
    } else ++skipped;
    pthread_mutex_unlock(&mutex);
    pending.count = 0;
}
int hitch_start(const char *path, double threshold_ms)
{
    if (output) { errno = EALREADY; return -1; }
    output = fopen(path, "wx");
    if (!output) return -1;
    fchmod(fileno(output), 0600);
    threshold_ns = (threshold_ms >= 1 && threshold_ms <= 10000) ?
        (uint64_t)(threshold_ms * 1e6) : 25000000;
    render_thread = pthread_self();
    fprintf(output, "hitch-recorder v1 pid=%ld wall=%lld monotonic=%.3f threshold_ms=%.3f history=%d after=%d limit=%d\n"
        "CPU wall timings only; draw time can include driver compilation or waiting, not GPU execution time.\n"
        "Category values are call-count/total-ms. Work includes untimed guest work; flush excludes pacing.\n",
        (long)getpid(), (long long)time(NULL), hitch_now() / 1e9, threshold_ns / 1e6,
        HISTORY, AFTER, REPORT_LIMIT);
    fflush(output);
    int error = pthread_create(&writer, NULL, write_loop, NULL);
    if (error) { fclose(output); output = NULL; errno = error; return -1; }
    hitch_recorder_enabled = 1;
    return 0;
}
void hitch_stop(void)
{
    if (!hitch_recorder_enabled) return;
    /* Called after guest execution stops; preserve a partially collected hitch. */
    if (pending.count) enqueue();
    pthread_mutex_lock(&mutex);
    stopping = true;
    pthread_cond_signal(&ready);
    pthread_mutex_unlock(&mutex);
    pthread_join(writer, NULL);
    fprintf(output, "end reports=%u skipped=%u\n", reports, skipped);
    fclose(output);
    hitch_recorder_enabled = 0;
}
void hitch_program(uint32_t target, uint32_t program)
{
    if (target == 0x8620) vertex_program = program;
    if (target == 0x8804) fragment_program = program;
}
void hitch_note(unsigned kind, const char *name, uint32_t caller,
                uint64_t start, uint64_t end, uint32_t draw_count)
{
    if (!hitch_recorder_enabled || kind < HITCH_DRAW || kind >= HITCH_KIND_COUNT) return;
    uint64_t ns = end - start;
    struct event e = {name, start, ns, caller, vertex_program, fragment_program, draw_count};
    if (!pthread_equal(pthread_self(), render_thread)) {
        if (ns < 2000000 || kind == HITCH_WAIT) return;
        pthread_mutex_lock(&mutex);
        workers[worker_next++ % WORKERS] = e;
        if (worker_count < WORKERS) ++worker_count;
        pthread_mutex_unlock(&mutex);
        return;
    }
    ++current.count[kind]; current.ns[kind] += ns;
    for (unsigned i = 0; i < TOP; ++i) {
        if (ns > current.top[i].ns) {
            for (unsigned j = TOP - 1; j > i; --j) current.top[j] = current.top[j - 1];
            current.top[i] = e; break;
        }
    }
}
void hitch_frame(uint64_t swap, uint64_t work_end, uint64_t flush_end,
                 uint64_t present_end, uint64_t target_ns, bool active)
{
    if (!hitch_recorder_enabled) return;
    current.swap = swap; current.end = present_end; current.active = active;
    current.present = previous_end ? present_end - previous_end : 0;
    current.work = previous_end ? work_end - previous_end : 0;
    current.flush = flush_end - work_end;
    current.pace = present_end - flush_end;
    uint64_t limit = target_ns * 3 / 2;
    if (limit < threshold_ns) limit = threshold_ns;
    if (pending.count) {
        pending.frames[pending.count++] = current;
        if (!--following) enqueue();
    } else if (active && history_count == HISTORY && current.present > limit &&
               (!last_trigger || present_end - last_trigger >= 5000000000ULL) && reports < REPORT_LIMIT) {
        pending.trigger = swap;
        for (unsigned i = 0; i < history_count; ++i)
            pending.frames[pending.count++] = history[(history_next + HISTORY - history_count + i) % HISTORY];
        pending.frames[pending.count++] = current;
        following = AFTER;
        last_trigger = present_end;
    }
    history[history_next++ % HISTORY] = current;
    if (history_count < HISTORY) ++history_count;
    previous_end = present_end;
    memset(&current, 0, sizeof(current));
}
