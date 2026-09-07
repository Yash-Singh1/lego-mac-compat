#ifndef LP32_MACHO_LOADER_H
#define LP32_MACHO_LOADER_H

#include <mach-o/loader.h>
#include <stddef.h>
#include <stdint.h>

#define MACHO_IMAGE32_MAX_IMPORTS 16384

enum macho_import32_kind {
    MACHO_IMPORT32_POINTER,
    MACHO_IMPORT32_STUB,
    MACHO_IMPORT32_FUNCTION_POINTER,
};

struct macho_import32 {
    const char *name;
    uint32_t address;
    uint32_t target; /* Nonzero for a symbol defined in this image, not a host import. */
    enum macho_import32_kind kind;
};

struct macho_image32 {
    const struct mach_header *header;
    uint32_t entry_eip;
    uint32_t main_address; /* LC_MAIN supplies main directly; LC_UNIXTHREAD does not. */
    uint32_t min_address;
    uint32_t max_address;
    uint32_t initializer_count;
    uint32_t initializer_address;
    uint32_t segment_count;
    /* __TEXT,__cstring and __DATA,__cfstring; the old fragile ObjC ABI keeps
       class references and constant strings there until dyld binds them. */
    uint32_t cstring_start;
    uint32_t cstring_end;
    uint32_t cfstring_start;
    uint32_t cfstring_end;
    uint32_t import_count;
    struct macho_import32 imports[MACHO_IMAGE32_MAX_IMPORTS];
};

int macho_image32_load(const char *path, struct macho_image32 *image);
void macho_image32_unload(const struct macho_image32 *image);

#endif
