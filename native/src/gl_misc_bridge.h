#ifndef LP32_GL_MISC_BRIDGE_H
#define LP32_GL_MISC_BRIDGE_H
#include <stdint.h>
int gl_misc_bridge32_dispatch(const char *name, const uint32_t *arguments,
                             uint64_t *result);
void gl_misc_bridge32_enable_tfu_compat(void);
/* Opt-in provenance for TFU's 64x64 uncompressed blade texture uploads. */
void gl_misc_bridge32_trace_texture_upload(const char *operation, uint32_t target,
                                         int32_t level, uint32_t source);
#endif
