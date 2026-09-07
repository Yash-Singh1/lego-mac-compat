#include "../src/zlib_bridge.c"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
static uint32_t cursor = 0x30001000;
static unsigned allocated, released;
uint32_t compat_runtime32_allocate(size_t size, int clear) {
    uint32_t p = cursor; cursor += (size + 15) & ~15u;
    assert(cursor < 0x30400000);
    if (clear) memset((void *)(uintptr_t)p, 0, size);
    return p;
}
void compat_runtime32_deallocate(uint32_t p) { assert(p >= 0x30001000 && p < cursor); }
uint32_t compat_runtime32_copy_cstring(const char *s) {
    if (!s) return 0;
    uint32_t p = compat_runtime32_allocate(strlen(s) + 1, 0);
    strcpy((void *)(uintptr_t)p, s); return p;
}
uint32_t compat_runtime32_call(uint32_t fn, const uint32_t *a, size_t n) {
    assert(a[0] == 42);
    if (fn == 1) { assert(n == 3); ++allocated; return compat_runtime32_allocate((size_t)a[1] * a[2], 0); }
    assert(fn == 2 && n == 2); ++released; compat_runtime32_deallocate(a[1]); return 0;
}
static int call(const char *name, uint32_t *a) {
    uint64_t r; assert(zlib_bridge32_dispatch(name, a, &r)); return (int32_t)r;
}
int main(void) {
    assert(mmap((void *)0x30000000, 0x400000, PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANON|MAP_FIXED, -1, 0) != MAP_FAILED);
    const char input[] = "compressed game archive data, compressed game archive data";
    uint32_t compressed = compat_runtime32_allocate(256, 0), output = compat_runtime32_allocate(256, 0);
    uLongf length = 256;
    assert(compress((void *)(uintptr_t)compressed, &length, (const void *)input, sizeof(input)) == Z_OK);
    struct { struct stream32 stream; uint32_t canary; } *g = (void *)0x30000000;
    for (int custom = 0; custom < 2; ++custom) {
        memset(g, 0, sizeof(*g)); g->canary = 0x87654321;
        g->stream.next_in = compressed; g->stream.avail_in = (uint32_t)length;
        g->stream.next_out = output; g->stream.avail_out = 8;
        if (custom) {g->stream.zalloc = 1; g->stream.zfree = 2; g->stream.opaque = 42;}
        uint32_t a[] = {0x30000000, 15, compat_runtime32_copy_cstring(ZLIB_VERSION), 56};
        assert(call("_inflateInit2_", a) == Z_OK);
        assert(g->stream.next_in == compressed && g->stream.avail_in == length);
        a[1] = Z_NO_FLUSH; assert(call("_inflate", a) == Z_OK);
        assert(g->stream.total_out == 8 && g->stream.next_out == output + 8);
        g->stream.avail_out = 248;
        assert(call("_inflate", a) == Z_STREAM_END);
        assert(g->stream.total_out == sizeof(input) && !memcmp((void *)(uintptr_t)output, input, sizeof(input)));
        assert(call("_inflateEnd", a) == Z_OK && !g->stream.state && !g->stream.msg);
        assert(g->canary == 0x87654321);
    }
    assert(allocated && allocated == released);
    puts("zlib PASS (incremental archive decompression, stream layout, callback ownership, canary)");
}
