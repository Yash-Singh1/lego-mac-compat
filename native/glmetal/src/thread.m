/* The command stream's batches and worker thread (thread.h). */
#import <Foundation/Foundation.h>

#include "glm_internal.h"
#include "thread.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum { BATCH_BYTES = 512 << 10, BATCHES = 8, FRAMES_AHEAD = 2 };

struct command_header {
    glm_exec_fn exec;
    uint32_t size; /* header included, 8-byte aligned */
    uint32_t pad;
};

struct glm_thread {
    struct glm_context *ctx;
    pthread_t thread;
    uint8_t *batch[BATCHES];
    size_t used[BATCHES];
    size_t filling;             /* bytes used in batch[submitted % BATCHES] */
    uint64_t submitted;         /* batches handed to the worker */
    uint64_t completed;         /* batches executed (under lock) */
    uint64_t frames_recorded, frames_executed;
    volatile int64_t pending[2]; /* recorded, unexecuted: [0] state changes, [1] texture-object changes */
    /* Commands recorded (application thread) and executed (worker). */
    uint64_t recorded;
    volatile uint64_t executed;
    /* glm_thread_request: run on the worker once `request_after` commands
       have executed, ahead of the rest of the stream. */
    glm_exec_fn request_fn;
    const void *request_arg;
    uint64_t request_after;
    volatile int request_state; /* 0 none, 1 pending, 2 done (under lock) */
    pthread_mutex_t lock;
    pthread_cond_t work, done;
    bool quit;
};

/* A pending request whose commands have executed runs now (worker). */
static void serve_request(struct glm_thread *t, bool locked)
{
    if (__atomic_load_n(&t->request_state, __ATOMIC_ACQUIRE) != 1 || t->executed < t->request_after) return;
    @autoreleasepool {
        t->request_fn(t->request_arg);
    }
    if (!locked) pthread_mutex_lock(&t->lock);
    __atomic_store_n(&t->request_state, 2, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&t->done);
    if (!locked) pthread_mutex_unlock(&t->lock);
}

static void *worker(void *argument)
{
    struct glm_thread *t = argument;
    pthread_setname_np("GLMetal command stream");
    glm_set_current(t->ctx);
    pthread_mutex_lock(&t->lock);
    for (;;) {
        while (t->completed == t->submitted && !t->quit) {
            serve_request(t, true);
            if (t->completed != t->submitted || t->quit) break;
            pthread_cond_wait(&t->work, &t->lock);
        }
        if (t->completed == t->submitted && t->quit) break;
        unsigned index = (unsigned)(t->completed % BATCHES);
        size_t used = t->used[index];
        pthread_mutex_unlock(&t->lock);
        @autoreleasepool {
            const uint8_t *p = t->batch[index], *end = p + used;
            while (p < end) {
                const struct command_header *h = (const struct command_header *)p;
                h->exec(h + 1);
                p += h->size;
                __atomic_store_n(&t->executed, t->executed + 1, __ATOMIC_RELEASE);
                if (__atomic_load_n(&t->request_state, __ATOMIC_ACQUIRE) == 1) serve_request(t, false);
            }
        }
        pthread_mutex_lock(&t->lock);
        ++t->completed;
        pthread_cond_broadcast(&t->done);
    }
    pthread_mutex_unlock(&t->lock);
    glm_set_current(NULL);
    return NULL;
}

void glm_thread_start(struct glm_context *ctx)
{
    if (ctx->thread) return;
    const char *env = getenv("GLMETAL_THREADED");
    if (env && env[0] == '0') return;
    struct glm_thread *t = calloc(1, sizeof *t);
    t->ctx = ctx;
    for (int i = 0; i < BATCHES; ++i) t->batch[i] = malloc(BATCH_BYTES);
    pthread_mutex_init(&t->lock, NULL);
    pthread_cond_init(&t->work, NULL);
    pthread_cond_init(&t->done, NULL);
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 8 << 20);
    pthread_create(&t->thread, &attributes, worker, t);
    pthread_attr_destroy(&attributes);
    ctx->thread = t;
    glm_log("command stream started for context %p", (void *)ctx);
}

void glm_thread_submit(struct glm_context *ctx)
{
    struct glm_thread *t = ctx->thread;
    if (!t || !t->filling) return;
    pthread_mutex_lock(&t->lock);
    t->used[t->submitted % BATCHES] = t->filling;
    ++t->submitted;
    pthread_cond_signal(&t->work);
    /* The next batch must be free. */
    while (t->submitted - t->completed >= BATCHES) pthread_cond_wait(&t->done, &t->lock);
    pthread_mutex_unlock(&t->lock);
    t->filling = 0;
}

void glm_thread_sync(struct glm_context *ctx)
{
    struct glm_thread *t = ctx->thread;
    if (!t) return;
    glm_thread_submit(ctx);
    pthread_mutex_lock(&t->lock);
    while (t->completed != t->submitted) pthread_cond_wait(&t->done, &t->lock);
    pthread_mutex_unlock(&t->lock);
}

/* GLMETAL_STREAM_STATS=1: which calls drain the stream, per 300 frames. */
static int stream_stats = -1;
static struct { const char *name; unsigned count; } sync_counts[64];

void glm_thread_sync_named(struct glm_context *ctx, const char *name)
{
    if (stream_stats < 0) stream_stats = getenv("GLMETAL_STREAM_STATS") != NULL;
    if (stream_stats) {
        for (int i = 0; i < 64; ++i) {
            if (sync_counts[i].name == name || !sync_counts[i].name) {
                sync_counts[i].name = name;
                ++sync_counts[i].count;
                break;
            }
        }
    }
    glm_thread_sync(ctx);
}

static void report_syncs(void)
{
    static unsigned frames;
    if (stream_stats <= 0 || ++frames % 300) return;
    for (int i = 0; i < 64 && sync_counts[i].name; ++i) {
        if (sync_counts[i].count)
            fprintf(stderr, "glmetal: stream sync %s %.1f/frame\n", sync_counts[i].name, sync_counts[i].count / 300.0);
        sync_counts[i].count = 0;
    }
}

void *glm_thread_alloc(struct glm_context *ctx, size_t size, glm_exec_fn exec)
{
    struct glm_thread *t = ctx->thread;
    size_t total = (sizeof(struct command_header) + size + 7) & ~(size_t)7;
    if (t->filling + total > BATCH_BYTES) glm_thread_submit(ctx);
    struct command_header *h = (struct command_header *)(t->batch[t->submitted % BATCHES] + t->filling);
    h->exec = exec;
    h->size = (uint32_t)total;
    t->filling += total;
    ++t->recorded;
    return h + 1;
}

uint64_t glm_thread_recorded(struct glm_context *ctx) { return ctx->thread ? ctx->thread->recorded : 0; }
uint64_t glm_thread_executed(struct glm_context *ctx)
{
    return ctx->thread ? __atomic_load_n(&ctx->thread->executed, __ATOMIC_ACQUIRE) : 0;
}

void glm_thread_request(struct glm_context *ctx, uint64_t after, glm_exec_fn fn, const void *arg)
{
    struct glm_thread *t = ctx->thread;
    if (!t) {
        fn(arg);
        return;
    }
    /* The commands it waits for must reach the worker. */
    if (t->executed < after) glm_thread_submit(ctx);
    pthread_mutex_lock(&t->lock);
    t->request_fn = fn;
    t->request_arg = arg;
    t->request_after = after;
    __atomic_store_n(&t->request_state, 1, __ATOMIC_RELEASE);
    pthread_cond_signal(&t->work);
    while (__atomic_load_n(&t->request_state, __ATOMIC_ACQUIRE) != 2) pthread_cond_wait(&t->done, &t->lock);
    __atomic_store_n(&t->request_state, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&t->lock);
}

void glm_thread_state_recorded(struct glm_context *ctx) { glm_thread_pending_add(ctx, 0, 1); }
void glm_thread_state_executed(struct glm_context *ctx) { glm_thread_pending_add(ctx, 0, -1); }
int glm_thread_state_settled(struct glm_context *ctx) { return glm_thread_pending_zero(ctx, 0); }

void glm_thread_pending_add(struct glm_context *ctx, int which, int delta)
{
    if (ctx && ctx->thread) __atomic_add_fetch(&ctx->thread->pending[which], delta, __ATOMIC_ACQ_REL);
}

int glm_thread_pending_zero(struct glm_context *ctx, int which)
{
    return __atomic_load_n(&ctx->thread->pending[which], __ATOMIC_ACQUIRE) == 0;
}

void glm_thread_frame_recorded(struct glm_context *ctx)
{
    struct glm_thread *t = ctx->thread;
    if (!t) return;
    report_syncs();
    glm_thread_submit(ctx);
    pthread_mutex_lock(&t->lock);
    ++t->frames_recorded;
    while (t->frames_recorded - t->frames_executed > FRAMES_AHEAD) pthread_cond_wait(&t->done, &t->lock);
    pthread_mutex_unlock(&t->lock);
}

void glm_thread_frame_executed(struct glm_context *ctx)
{
    struct glm_thread *t = ctx->thread;
    if (!t) return;
    pthread_mutex_lock(&t->lock);
    ++t->frames_executed;
    pthread_cond_broadcast(&t->done);
    pthread_mutex_unlock(&t->lock);
}

void glm_thread_stop(struct glm_context *ctx)
{
    struct glm_thread *t = ctx->thread;
    if (!t) return;
    glm_thread_sync(ctx);
    pthread_mutex_lock(&t->lock);
    t->quit = true;
    pthread_cond_signal(&t->work);
    pthread_mutex_unlock(&t->lock);
    pthread_join(t->thread, NULL);
    for (int i = 0; i < BATCHES; ++i) free(t->batch[i]);
    pthread_mutex_destroy(&t->lock);
    pthread_cond_destroy(&t->work);
    pthread_cond_destroy(&t->done);
    free(t);
    ctx->thread = NULL;
}
