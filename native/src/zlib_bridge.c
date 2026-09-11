#include "zlib_bridge.h"
#include "compat_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* Darwin i386 z_stream contains fourteen 32-bit fields. Native z_stream
 * contains 64-bit pointers and uLongs; only input/output buffers are shared. */
struct stream32 {
    uint32_t next_in, avail_in, total_in, next_out, avail_out, total_out;
    uint32_t msg, state, zalloc, zfree, opaque;
    int32_t data_type;
    uint32_t adler, reserved;
};
struct stream {
    z_stream native;
    int compressing;
    uint32_t allocate, release, opaque, message;
};
_Static_assert(sizeof(struct stream32) == 56, "i386 z_stream layout");
static voidpf allocate(voidpf opaque, uInt count, uInt size) {
    struct stream *s = opaque;
    uint32_t args[] = {s->opaque, count, size};
    return (void *)(uintptr_t)compat_runtime32_call(s->allocate, args, 3);
}
static void release(voidpf opaque, voidpf pointer) {
    struct stream *s = opaque;
    uint32_t args[] = {s->opaque, (uint32_t)(uintptr_t)pointer};
    compat_runtime32_call(s->release, args, 2);
}
/* Native state is carried in a guest-owned pointer cell, never exposed as a
 * 64-bit pointer in the stream itself. Each z_stream is independently owned. */
static struct stream *state(struct stream32 *g) {
    if (!g || !g->state) return NULL;
    struct stream *s;
    memcpy(&s, (void *)(uintptr_t)g->state, sizeof(s));
    return s;
}
static void sync_out(struct stream32 *g, struct stream *s) {
    g->next_in = (uint32_t)(uintptr_t)s->native.next_in;
    g->avail_in = s->native.avail_in; g->total_in = (uint32_t)s->native.total_in;
    g->next_out = (uint32_t)(uintptr_t)s->native.next_out;
    g->avail_out = s->native.avail_out; g->total_out = (uint32_t)s->native.total_out;
    g->data_type = s->native.data_type; g->adler = (uint32_t)s->native.adler;
    if (s->message) compat_runtime32_deallocate(s->message);
    g->msg = s->message = compat_runtime32_copy_cstring(s->native.msg);
}
/* Checkpoint restore can issue hundreds of thousands of small inflate calls.
 * Both ordinary dispatch and its memoized handler use this same ABI adapter. */
static uint64_t run_stream(const uint32_t *a, int compressing) {
    struct stream32 *g = (void *)(uintptr_t)a[0];
    struct stream *s = state(g);
    if (!s || s->compressing != compressing) return (uint32_t)Z_STREAM_ERROR;
    s->native.next_in = (void *)(uintptr_t)g->next_in;
    s->native.avail_in = g->avail_in;
    s->native.next_out = (void *)(uintptr_t)g->next_out;
    s->native.avail_out = g->avail_out;
    int result = compressing ? deflate(&s->native, (int)a[1]) : inflate(&s->native, (int)a[1]);
    sync_out(g, s);
    return (uint32_t)result;
}
uint64_t zlib_bridge32_inflate(const uint32_t *a, uint32_t caller) {
    (void)caller;
    return run_stream(a, 0);
}
uint64_t zlib_bridge32_deflate(const uint32_t *a, uint32_t caller) {
    (void)caller;
    return run_stream(a, 1);
}
int zlib_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *out) {
    if (!strcmp(name, "_inflate")) { *out = zlib_bridge32_inflate(a, 0); return 1; }
    if (!strcmp(name, "_deflate")) { *out = zlib_bridge32_deflate(a, 0); return 1; }
    if (!strcmp(name, "_inflateInit_")) {
        const uint32_t modern[] = {a[0], MAX_WBITS, a[1], a[2]};
        return zlib_bridge32_dispatch("_inflateInit2_", modern, out);
    }
    if (!strcmp(name, "_deflateInit_")) {
        const uint32_t modern[] = {a[0], a[1], Z_DEFLATED, MAX_WBITS, 8, Z_DEFAULT_STRATEGY, a[2], a[3]};
        return zlib_bridge32_dispatch("_deflateInit2_", modern, out);
    }
    int compressing = !strncmp(name, "_deflate", 8);
    int init = !strcmp(name, "_inflateInit2_") || !strcmp(name, "_deflateInit2_");
    int end = !strcmp(name, "_inflateEnd") || !strcmp(name, "_deflateEnd");
    int reset = !strcmp(name, "_inflateReset") || !strcmp(name, "_deflateReset");
    if (!init && !end && !reset) return 0;
    *out = (uint32_t)Z_STREAM_ERROR;
    struct stream32 *g = (void *)(uintptr_t)a[0];
    if (!g) return 1;
    if (init) {
        const char *version = (void *)(uintptr_t)a[compressing ? 6 : 2];
        if (a[compressing ? 7 : 3] != sizeof(*g) || !version || version[0] != ZLIB_VERSION[0]) {
            *out = (uint32_t)Z_VERSION_ERROR; return 1;
        }
        struct stream *s = calloc(1, sizeof(*s));
        uint32_t token = s ? compat_runtime32_allocate(sizeof(s), 0) : 0;
        if (!token) { free(s); *out = (uint32_t)Z_MEM_ERROR; return 1; }
        s->allocate = g->zalloc; s->release = g->zfree; s->opaque = g->opaque;
        s->native.next_in = (void *)(uintptr_t)g->next_in;
        s->native.avail_in = g->avail_in;
        s->native.next_out = (void *)(uintptr_t)g->next_out;
        s->native.avail_out = g->avail_out;
        s->native.opaque = s;
        s->native.zalloc = s->allocate ? allocate : NULL;
        s->native.zfree = s->release ? release : NULL;
        s->compressing = compressing;
        int result = compressing ? deflateInit2(&s->native, (int)a[1], (int)a[2], (int)a[3], (int)a[4], (int)a[5]) : inflateInit2(&s->native, (int)a[1]);
        *out = (uint32_t)result;
        if (result != Z_OK) { compat_runtime32_deallocate(token); free(s); return 1; }
        memcpy((void *)(uintptr_t)token, &s, sizeof(s));
        g->state = token; sync_out(g, s); return 1;
    }
    struct stream *s = state(g);
    if (!s || s->compressing != compressing) return 1;
    if (reset) {
        *out = (uint32_t)(compressing ? deflateReset(&s->native) : inflateReset(&s->native));
        sync_out(g, s);
    } else {
        *out = (uint32_t)(compressing ? deflateEnd(&s->native) : inflateEnd(&s->native));
        if (s->message) compat_runtime32_deallocate(s->message);
        compat_runtime32_deallocate(g->state);
        g->state = g->msg = 0; free(s);
    }
    return 1;
}
