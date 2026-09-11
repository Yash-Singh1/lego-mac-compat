#include "macho_file.h"
#include <assert.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <string.h>

static void put32(unsigned char *p, uint32_t n, int big)
{
    for (int i = 0; i < 4; ++i) p[big ? 3 - i : i] = (unsigned char)(n >> (8 * i));
}

int main(void)
{
    unsigned char bytes[256] = {0};
    struct mach_header h = {.magic = MH_MAGIC, .cputype = CPU_TYPE_I386, .filetype = MH_EXECUTE};
    const uint8_t *slice = NULL;
    size_t size = 0;
    memcpy(bytes, &h, sizeof(h));
    assert(!macho_file32_slice(bytes, sizeof(h), &slice, &size));
    assert(slice == bytes && size == sizeof(h));
    assert(macho_file32_slice(bytes, sizeof(h) - 1, &slice, &size));
    h.cputype = CPU_TYPE_X86_64;
    memcpy(bytes, &h, sizeof(h));
    assert(macho_file32_slice(bytes, sizeof(h), &slice, &size));
    h.cputype = CPU_TYPE_I386;
    for (int big = 0; big <= 1; ++big) {
        memset(bytes, 0, sizeof(bytes));
        put32(bytes, FAT_MAGIC, big);
        put32(bytes + 4, 2, big);
        put32(bytes + 8, CPU_TYPE_X86_64, big);
        put32(bytes + 28, CPU_TYPE_I386, big);
        put32(bytes + 36, 128, big);
        put32(bytes + 40, sizeof(h), big);
        memcpy(bytes + 128, &h, sizeof(h));
        assert(!macho_file32_slice(bytes, sizeof(bytes), &slice, &size));
        assert(slice == bytes + 128 && size == sizeof(h));
        assert(macho_file32_slice(bytes, 128 + sizeof(h) - 1, &slice, &size));
        put32(bytes + 40, UINT32_MAX, big);
        assert(macho_file32_slice(bytes, sizeof(bytes), &slice, &size));
        put32(bytes + 40, sizeof(h), big);
        put32(bytes + 36, 8, big);
        assert(macho_file32_slice(bytes, sizeof(bytes), &slice, &size));
        put32(bytes + 4, UINT32_MAX, big);
        assert(macho_file32_slice(bytes, sizeof(bytes), &slice, &size));
    }
    puts("Mach-O slice tests: PASS (thin, fat, swapped fat, truncation, invalid ranges)");
    return 0;
}
