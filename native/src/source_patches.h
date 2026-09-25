#ifndef LP32_SOURCE_PATCHES_H
#define LP32_SOURCE_PATCHES_H

#include "compat_runtime.h"
#include <stdint.h>

/* UUID-gated repairs for defects in specific shipped Source dylibs. */
void lp32_source_patches_install(const char *path, uint32_t slide, const unsigned char uuid[16]);
lp32_fast_import_fn lp32_source_patch_handler(const char *name);

#endif
