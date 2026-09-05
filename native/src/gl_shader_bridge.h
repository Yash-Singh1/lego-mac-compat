#ifndef LP32_GL_SHADER_BRIDGE_H
#define LP32_GL_SHADER_BRIDGE_H
#include <stdint.h>
int gl_shader_bridge32_dispatch(const char *name, const uint32_t *arguments,
                               uint64_t *result);
#endif
