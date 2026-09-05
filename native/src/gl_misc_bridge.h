#ifndef LP32_GL_MISC_BRIDGE_H
#define LP32_GL_MISC_BRIDGE_H
#include <stdint.h>
int gl_misc_bridge32_dispatch(const char *name, const uint32_t *arguments,
                             uint64_t *result);
#endif
