#include "steam_storage_fix.h"

#include <CommonCrypto/CommonDigest.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach-o/loader.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* A shipped x86_64 storage implementation corrupts its parent-directory
 * buffer during recursion. RBX holds the absolute parent path; R14 holds
 * the temporary child path. Three MOVs wrongly select RBX when constructing
 * and passing a relative child path. Subsequent siblings then fail stat().
 * Select R14 instead, preserving the parent for the entire directory walk.
 *
 * This repairs the library routine, not the game's save catalog. Its normal
 * enumeration, quota accounting, filters, locking and cache remain in use.
 * Match both the library UUID and every byte of the routine before editing;
 * other SDK builds are untouched. No game IDs, paths or filenames are used.
 * See analysis/marvel-perf/storage-root-cause.md for the native reproducer. */
int steam_storage_fix_apply(const struct mach_header_64 *image)
{
    static const uint8_t uuid[16] = {
        0xdc,0x2f,0x8c,0xa1,0x46,0x76,0x37,0x9f,
        0x82,0x56,0xfd,0x00,0xf9,0x1b,0x04,0xde
    };
    static const uint8_t original[CC_SHA256_DIGEST_LENGTH] = {
        0x01,0xe6,0x43,0xe1,0xd4,0xf1,0xe5,0x6d,0xa9,0xfa,0x71,0xc0,0xdf,0xec,0x37,0xd3,
        0x1a,0xd4,0x91,0x02,0x17,0x57,0x3d,0xe8,0x3b,0x9b,0x4a,0xf8,0x51,0xc2,0x3c,0x78
    };
    static const uint8_t repaired[CC_SHA256_DIGEST_LENGTH] = {
        0xa4,0x17,0x33,0x22,0xdb,0x1e,0xe3,0xea,0x9c,0x56,0x26,0xe4,0x2a,0xe6,0x49,0x36,
        0x2d,0x9f,0x20,0x84,0x93,0x7f,0x22,0x41,0x17,0x35,0xea,0x5f,0xb8,0x65,0x0d,0x48
    };
    enum { start = 0x199c4, end = 0x19d94 };
    if (!image || image->magic != MH_MAGIC_64 || image->cputype != CPU_TYPE_X86_64)
        return 0;
    const uint8_t *command = (const uint8_t *)(image + 1);
    size_t remaining = image->sizeofcmds;
    bool known_uuid = false, contains_code = false;
    for (uint32_t i = 0; i < image->ncmds; ++i) {
        if (remaining < sizeof(struct load_command)) return 0;
        const struct load_command *lc = (const void *)command;
        if (lc->cmdsize < sizeof(*lc) || lc->cmdsize > remaining) return 0;
        if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(struct uuid_command))
            known_uuid = !memcmp(((const struct uuid_command *)lc)->uuid, uuid, sizeof(uuid));
        if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *segment = (const void *)lc;
            if (!strncmp(segment->segname, "__TEXT", sizeof(segment->segname)) && segment->vmaddr == 0 &&
                segment->vmsize >= end && (segment->initprot & VM_PROT_EXECUTE))
                contains_code = true;
        }
        command += lc->cmdsize;
        remaining -= lc->cmdsize;
    }
    if (!known_uuid || !contains_code) return 0;
    uint8_t *base = (uint8_t *)(uintptr_t)image;
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(base + start, end - start, digest);
    if (!memcmp(digest, repaired, sizeof(digest))) return 1;
    if (memcmp(digest, original, sizeof(digest))) return 0;

    uintptr_t first = ((uintptr_t)base + start) & ~(uintptr_t)(vm_page_size - 1);
    uintptr_t limit = ((uintptr_t)base + end + vm_page_size - 1) & ~(uintptr_t)(vm_page_size - 1);
    /* Private copy-on-write pages: never modify the source dylib or another
     * process. Remove execute permission while editing, then restore RX. */
    if (vm_protect(mach_task_self(), first, limit - first, false,
                   VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS)
        return -1;
    static const size_t offsets[] = {0x19a96, 0x19aaa, 0x19abc};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i)
        memcpy(base + offsets[i], "\x4c\x89\xf7", 3); /* mov %r14, %rdi */
    if (vm_protect(mach_task_self(), first, limit - first, false,
                   VM_PROT_READ | VM_PROT_EXECUTE) != KERN_SUCCESS)
        return -1;
    sys_icache_invalidate((void *)first, limit - first);
    return 1;
}
