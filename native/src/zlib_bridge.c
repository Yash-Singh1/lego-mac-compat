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
int zlib_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *out) {
    int init = !strcmp(name, "_inflateInit2_");
    int run = !strcmp(name, "_inflate");
    int end = !strcmp(name, "_inflateEnd");
    if (!init && !run && !end) return 0;
    *out = (uint32_t)Z_STREAM_ERROR;
    struct stream32 *g = (void *)(uintptr_t)a[0];
    if (!g) return 1;
    if (init) {
        const char *version = (void *)(uintptr_t)a[2];
        if (a[3] != sizeof(*g) || !version || version[0] != ZLIB_VERSION[0]) {
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
        int result = inflateInit2(&s->native, (int)a[1]);
        *out = (uint32_t)result;
        if (result != Z_OK) { compat_runtime32_deallocate(token); free(s); return 1; }
        memcpy((void *)(uintptr_t)token, &s, sizeof(s));
        g->state = token; sync_out(g, s); return 1;
    }
    struct stream *s = state(g);
    if (!s) return 1;
    if (run) {
        s->native.next_in = (void *)(uintptr_t)g->next_in;
        s->native.avail_in = g->avail_in;
        s->native.next_out = (void *)(uintptr_t)g->next_out;
        s->native.avail_out = g->avail_out;
        *out = (uint32_t)inflate(&s->native, (int)a[1]);
        sync_out(g, s);
    } else {
        *out = (uint32_t)inflateEnd(&s->native);
        if (s->message) compat_runtime32_deallocate(s->message);
        compat_runtime32_deallocate(g->state);
        g->state = g->msg = 0; free(s);
    }
    return 1;
}
