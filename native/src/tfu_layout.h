#ifndef LP32_TFU_LAYOUT_H
#define LP32_TFU_LAYOUT_H
#include "game_profile.h"
/* Searches executable code only. Every signature must match exactly once;
   relative control-flow targets and the input/movie ABI are checked too.
   No executable hash, load address or distribution identifier is required. */
int tfu_layout_find(const uint8_t *code, size_t size, uint32_t base, uint32_t entry,
                    struct lp32_tfu_layout *layout, uint32_t *display, uint32_t *main);
#endif
