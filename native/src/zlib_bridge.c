#include "zlib_bridge.h"
#include "compat_runtime.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* z_stream contains pointers AND unsigned longs; both double in width on
   Darwin x86_64. Its native opaque state must never enter the guest record. */
struct stream32 {
    uint32_t next_in, avail_in, total_in, next_out, avail_out, total_out;
    uint32_t msg, state, zalloc, zfree, opaque;
    int32_t data_type;
    uint32_t adler, reserved;
};
_Static_assert(sizeof(struct stream32) == 56, "i386 z_stream ABI");
struct stream {
    uint32_t guest, token, alloc, free, opaque, message;
    int deflating;
    z_stream native;
    struct stream *next;
};
static struct stream *streams;
static pthread_mutex_t stream_lock = PTHREAD_MUTEX_INITIALIZER;

static void *guest_alloc(void *opaque, unsigned items, unsigned size)
{
    struct stream *s = opaque;
    uint32_t args[] = {s->opaque, items, size};
    return (void *)(uintptr_t)(uint32_t)compat_runtime32_call(s->alloc, args, 3);
}
static void guest_free(void *opaque, void *pointer)
{
    struct stream *s = opaque;
    uint32_t args[] = {s->opaque, (uint32_t)(uintptr_t)pointer};
    compat_runtime32_call(s->free, args, 2);
}
static struct stream *lookup(uint32_t guest)
{
    pthread_mutex_lock(&stream_lock);
    struct stream *s = streams;
    while (s && s->guest != guest) s = s->next;
    pthread_mutex_unlock(&stream_lock);
    return s;
}
static void publish(struct stream *s)
{
    struct stream32 *g = (void *)(uintptr_t)s->guest;
    z_stream *n = &s->native;
    g->next_in = (uint32_t)(uintptr_t)n->next_in; g->avail_in = n->avail_in;
    g->next_out = (uint32_t)(uintptr_t)n->next_out; g->avail_out = n->avail_out;
    g->total_in = (uint32_t)n->total_in; g->total_out = (uint32_t)n->total_out;
    g->adler = (uint32_t)n->adler; g->data_type = n->data_type; g->reserved = (uint32_t)n->reserved;
    if (n->msg && (!s->message || strcmp((char *)(uintptr_t)s->message, n->msg))) {
        uint32_t copy = compat_runtime32_copy_cstring(n->msg);
        compat_runtime32_deallocate(s->message); s->message = copy;
    }
    g->msg = n->msg ? s->message : 0;
    g->state = n->state ? s->token : 0;
}
static void dispose(struct stream *s)
{
    pthread_mutex_lock(&stream_lock);
    struct stream **entry = &streams;
    while (*entry && *entry != s) entry = &(*entry)->next;
    if (*entry) *entry = s->next;
    pthread_mutex_unlock(&stream_lock);
    compat_runtime32_deallocate(s->token);
    compat_runtime32_deallocate(s->message);
    free(s);
}
int zlib_bridge32_dispatch(const char *name, const uint32_t *args, uint64_t *result)
{
    if (!strcmp(name, "_crc32") || !strcmp(name, "_adler32")) {
        *result = (uint32_t)(name[1] == 'c' ? crc32(args[0], (void *)(uintptr_t)args[1], args[2]) :
            adler32(args[0], (void *)(uintptr_t)args[1], args[2]));
        return 1;
    }
    int operation;
    if (!strcmp(name, "_inflateInit_")) operation = 0;
    else if (!strcmp(name, "_inflateInit2_")) operation = 1;
    else if (!strcmp(name, "_deflateInit_")) operation = 2;
    else if (!strcmp(name, "_deflateInit2_")) operation = 3;
    else if (!strcmp(name, "_inflate")) operation = 4;
    else if (!strcmp(name, "_deflate")) operation = 5;
    else if (!strcmp(name, "_inflateReset")) operation = 6;
    else if (!strcmp(name, "_deflateReset")) operation = 7;
    else if (!strcmp(name, "_inflateEnd")) operation = 8;
    else if (!strcmp(name, "_deflateEnd")) operation = 9;
    else return 0;
    *result = (uint32_t)Z_STREAM_ERROR;
    if (!args[0]) return 1;
    struct stream32 *g = (void *)(uintptr_t)args[0];
    struct stream *s = lookup(args[0]);
    int rc;
    if (operation < 4) {
        unsigned version_word = operation == 0 ? 1 : operation == 3 ? 6 : 2;
        const char *version = (void *)(uintptr_t)args[version_word];
        if (!version || version[0] != ZLIB_VERSION[0] || args[version_word + 1] != sizeof(*g)) {
            *result = (uint32_t)Z_VERSION_ERROR; return 1;
        }
        if (s || (!!g->zalloc != !!g->zfree)) return 1;
        s = calloc(1, sizeof(*s));
        if (!s) { *result = (uint32_t)Z_MEM_ERROR; return 1; }
        s->guest = args[0]; s->alloc = g->zalloc; s->free = g->zfree; s->opaque = g->opaque;
        s->deflating = operation >= 2;
        s->token = compat_runtime32_allocate(4, 1);
        if (!s->token) { free(s); *result = (uint32_t)Z_MEM_ERROR; return 1; }
        if (s->alloc) {
            s->native.zalloc = guest_alloc; s->native.zfree = guest_free; s->native.opaque = s;
        }
        s->native.next_in = (void *)(uintptr_t)g->next_in; s->native.avail_in = g->avail_in;
        s->native.next_out = (void *)(uintptr_t)g->next_out; s->native.avail_out = g->avail_out;
        if (operation == 0) rc = inflateInit(&s->native);
        else if (operation == 1) rc = inflateInit2(&s->native, (int)args[1]);
        else if (operation == 2) rc = deflateInit(&s->native, (int)args[1]);
        else rc = deflateInit2(&s->native, (int)args[1], (int)args[2], (int)args[3], (int)args[4], (int)args[5]);
        publish(s);
        if (rc == Z_OK) {
            pthread_mutex_lock(&stream_lock); s->next = streams; streams = s; pthread_mutex_unlock(&stream_lock);
        } else {
            g->msg = g->state = 0; dispose(s);
        }
    } else {
        if (!s || g->state != s->token || s->deflating != (operation & 1)) return 1;
        s->native.next_in = (void *)(uintptr_t)g->next_in; s->native.avail_in = g->avail_in;
        s->native.next_out = (void *)(uintptr_t)g->next_out; s->native.avail_out = g->avail_out;
        if (operation == 4) rc = inflate(&s->native, (int)args[1]);
        else if (operation == 5) rc = deflate(&s->native, (int)args[1]);
        else if (operation == 6) rc = inflateReset(&s->native);
        else if (operation == 7) rc = deflateReset(&s->native);
        else if (operation == 8) rc = inflateEnd(&s->native);
        else rc = deflateEnd(&s->native);
        publish(s);
        if (operation >= 8 && !s->native.state) { g->msg = g->state = 0; dispose(s); }
    }
    *result = (uint32_t)rc;
    return 1;
}
