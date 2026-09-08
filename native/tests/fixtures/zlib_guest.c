/* Actual i386 z_stream declarations from the system header, not the bridge. */
#include <zlib.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static unsigned allocated, freed;
static int fail_alloc;
static void *allocate(void *opaque, unsigned count, unsigned size) {
    if (opaque != (void *)0x1234 || crc32(0, (const unsigned char *)"123456789", 9) != 0xcbf43926) return 0;
    if (fail_alloc) return 0;
    void *p = calloc(count, size); if (p) ++allocated; return p;
}
static void release(void *opaque, void *p) {
    if (opaque != (void *)0x1234) return;
    if (p) ++freed; free(p);
}
int check_zlib(void) {
    const unsigned char input[] = "controller glyph pixels: red green blue alpha red green blue alpha";
    unsigned char compressed[256], decoded[256];
    if (sizeof(z_stream) != 56 || crc32(0, (void *)"123456789", 9) != 0xcbf43926 ||
        adler32(1, (void *)"123456789", 9) != 0x091e01de) return -101;
    for (unsigned custom = 0; custom < 2; ++custom) {
        allocated = freed = 0;
        struct { uint32_t before; z_stream value; uint32_t after; } d = {.before=0x11223344, .after=0xaabbccdd};
        struct { uint32_t before; z_stream value; uint32_t after; } z = {.before=0x12345678, .after=0x87654321};
        if (custom) {
            d.value.zalloc = z.value.zalloc = allocate;
            d.value.zfree = z.value.zfree = release;
            d.value.opaque = z.value.opaque = (void *)0x1234;
        }
        if (inflateInit_(&z.value, ZLIB_VERSION, 112) != Z_VERSION_ERROR ||
            inflateInit_(&z.value, "0.invalid", 56) != Z_VERSION_ERROR) return -102;
        if (custom) {
            fail_alloc = 1;
            if (inflateInit(&z.value) != Z_MEM_ERROR || z.value.state) return -103;
            fail_alloc = 0;
        }
        if (deflateInit2(&d.value, 6, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK ||
            inflateInit(&z.value) != Z_OK) return -104;
        for (unsigned cycle = 0; cycle < 10; ++cycle) {
            d.value.next_in = (Bytef *)input; d.value.avail_in = sizeof(input);
            d.value.next_out = compressed; d.value.avail_out = sizeof(compressed);
            if (deflate(&d.value, Z_FINISH) != Z_STREAM_END || d.value.total_in != sizeof(input)) return -105;
            unsigned length = d.value.total_out;
            z.value.next_in = compressed; z.value.avail_in = length;
            int rc = Z_OK;
            while (rc == Z_OK) {
                z.value.next_out = decoded + z.value.total_out;
                z.value.avail_out = 7;
                rc = inflate(&z.value, Z_NO_FLUSH);
                if (z.value.total_out > sizeof(input)) return -106;
            }
            if (rc != Z_STREAM_END || z.value.total_in != length || z.value.total_out != sizeof(input) ||
                memcmp(input, decoded, sizeof(input))) return -107;
            if (inflateReset(&z.value) != Z_OK || deflateReset(&d.value) != Z_OK ||
                z.value.total_in || z.value.total_out || d.value.total_out) return -108;
        }
        unsigned char corrupt[] = {0, 0, 0, 0};
        z.value.next_in = corrupt; z.value.avail_in = sizeof(corrupt);
        z.value.next_out = decoded; z.value.avail_out = sizeof(decoded);
        if (inflate(&z.value, Z_NO_FLUSH) != Z_DATA_ERROR || !z.value.msg || !*z.value.msg) return -109;
        if (inflateEnd(&z.value) != Z_OK || deflateEnd(&d.value) != Z_OK || z.value.state || d.value.state) return -110;
        if (inflateEnd(&z.value) != Z_STREAM_ERROR || d.before != 0x11223344 || d.after != 0xaabbccdd ||
            z.before != 0x12345678 || z.after != 0x87654321) return -111;
        if (custom && (!allocated || allocated != freed)) return -112;
    }
    return 0;
}
