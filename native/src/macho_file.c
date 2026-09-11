#include "macho_file.h"

#include <mach-o/fat.h>
#include <mach-o/loader.h>

static uint32_t read32(const uint8_t *p, int big)
{
    if (big) return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                    (uint32_t)p[2] << 8 | p[3];
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 |
           (uint32_t)p[1] << 8 | p[0];
}

int macho_file32_slice(const void *bytes, size_t size,
                       const uint8_t **slice, size_t *slice_size)
{
    const uint8_t *p = bytes;
    if (!p || !slice || !slice_size || size < sizeof(struct mach_header)) return -1;
    uint32_t magic = read32(p, 0);
    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        int big = magic == FAT_CIGAM;
        uint32_t count = read32(p + 4, big);
        if (count > (size - 8) / sizeof(struct fat_arch)) return -1;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t *arch = p + 8 + i * sizeof(struct fat_arch);
            if (read32(arch, big) != CPU_TYPE_I386) continue;
            uint32_t offset = read32(arch + 8, big);
            uint32_t length = read32(arch + 12, big);
            if (offset < 8 + count * sizeof(struct fat_arch) ||
                offset > size || length > size - offset ||
                length < sizeof(struct mach_header)) return -1;
            p += offset;
            size = length;
            break;
        }
    }
    if (read32(p, 0) != MH_MAGIC || read32(p + 4, 0) != CPU_TYPE_I386) return -1;
    *slice = p;
    *slice_size = size;
    return 0;
}
