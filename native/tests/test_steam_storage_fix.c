#include "../src/steam_storage_fix.h"
#include <CommonCrypto/CommonDigest.h>
#include <assert.h>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exercise the actual shipped walker with a temporary root supplied by the
 * Python test. No Steam initialization, game, user storage or write API runs.
 * Internal offsets are used only after verifying the exact routine hash. */
int main(int argc, char **argv)
{
    assert(argc == 4);
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    Dl_info info = {0};
    assert(dladdr(dlsym(library, "SteamRemoteStorage"), &info));
    unsigned char *base = info.dli_fbase;
    const struct mach_header_64 *header = (const void *)base;
    assert(header->magic == MH_MAGIC_64 && header->cputype == CPU_TYPE_X86_64);
    /* Check the UUID before using offsets in the test fixture. */
    const unsigned char uuid[] = {0xdc,0x2f,0x8c,0xa1,0x46,0x76,0x37,0x9f,
                                  0x82,0x56,0xfd,0x00,0xf9,0x1b,0x04,0xde};
    const struct load_command *lc = (const void *)(header + 1);
    size_t uuid_offset = 0;
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (lc->cmd == LC_UUID) {
            const struct uuid_command *u = (const void *)lc;
            assert(!memcmp(u->uuid, uuid, sizeof(uuid)));
            uuid_offset = (const unsigned char *)u->uuid - base;
        }
        lc = (const void *)((const char *)lc + lc->cmdsize);
    }
    assert(uuid_offset);
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(base + 0x199c4, 0x3d0, digest);
    const unsigned char expected[] = {
        0x01,0xe6,0x43,0xe1,0xd4,0xf1,0xe5,0x6d,0xa9,0xfa,0x71,0xc0,0xdf,0xec,0x37,0xd3,
        0x1a,0xd4,0x91,0x02,0x17,0x57,0x3d,0xe8,0x3b,0x9b,0x4a,0xf8,0x51,0xc2,0x3c,0x78
    };
    assert(!memcmp(digest, expected, sizeof(expected)));

    /* Unknown UUIDs and changed code must be a byte-for-byte no-op. */
    unsigned char *copy = malloc(0x62000), *snapshot = malloc(0x62000);
    assert(copy && snapshot);
    memcpy(copy, base, 0x62000);
    copy[uuid_offset] ^= 1;
    memcpy(snapshot, copy, 0x62000);
    assert(steam_storage_fix_apply((const void *)copy) == 0);
    assert(!memcmp(copy, snapshot, 0x62000));
    copy[uuid_offset] ^= 1;
    copy[0x19ca7] ^= 1;
    memcpy(snapshot, copy, 0x62000);
    assert(steam_storage_fix_apply((const void *)copy) == 0);
    assert(!memcmp(copy, snapshot, 0x62000));
    free(copy); free(snapshot);
    assert(steam_storage_fix_apply(NULL) == 0);

    /* Redirect only this isolated test process's private SDK root buffer. */
    assert(strlen(argv[2]) < 1024);
    strcpy((char *)base + 0x692a0, argv[2]);
    if (!strcmp(argv[3], "repaired")) {
        assert(steam_storage_fix_apply(header) == 1);
        assert(steam_storage_fix_apply(header) == 1); /* idempotent */
    } else assert(!strcmp(argv[3], "original"));
    int bytes = ((int (*)(const char *))(base + 0x199c4))(NULL);
    char **begin = *(char ***)(base + 0x68850), **end = *(char ***)(base + 0x68858);
    printf("bytes=%d\n", bytes);
    for (char **p = begin; p != end; ++p) printf("file=%s\n", *p);
    return 0;
}
