#ifndef ARB_PROGRAM_GUARD_H
#define ARB_PROGRAM_GUARD_H

#include <stddef.h>
#include <stdint.h>
char *arb_program_normalize_line_endings(const void *, size_t, size_t *);

char *arb_program_guard_undefined_math(const void *source, size_t source_size,
                                       size_t *output_size,
                                       size_t *rsq_guard_count,
                                       size_t *rcp_guard_count);

char *arb_program_plain_shadow_targets(const void *source, size_t source_size,
                                       size_t *output_size,
                                       size_t *rewrite_count);
/* Bit N set means texture[N] is sampled with a SHADOW target. */
uint32_t arb_program_shadow_texture_units(const void *source, size_t source_size);
/* strip_units: bit N set rewrites texture[N]'s SHADOW target to the plain
   target. ~0u rewrites every SHADOW target, including ones with no unit. */
char *arb_program_plain_shadow_targets_masked(const void *source,
                                              size_t source_size,
                                              uint32_t strip_units,
                                              size_t *output_size,
                                              size_t *rewrite_count);

char *arb_program_expand_output_aliases(const void *source, size_t source_size,
                                        size_t *output_size);

#endif
