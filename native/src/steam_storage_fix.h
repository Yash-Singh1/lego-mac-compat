#ifndef LP32_STEAM_STORAGE_FIX_H
#define LP32_STEAM_STORAGE_FIX_H

struct mach_header_64;
/* Call immediately after dlopen, before publishing/calling SDK interfaces.
 * Returns 1 for repaired/already repaired, 0 for other builds, -1 on failure. */
int steam_storage_fix_apply(const struct mach_header_64 *image);

#endif
