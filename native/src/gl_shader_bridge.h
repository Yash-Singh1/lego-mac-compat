#ifndef LP32_GL_SHADER_BRIDGE_H
#define LP32_GL_SHADER_BRIDGE_H
#include <stdint.h>
void gl_shader_bridge32_enable_tfu_compat(void);
void gl_shader_bridge32_dump_program(uint32_t program, const char *directory);
int gl_shader_bridge32_dispatch(const char *name, const uint32_t *arguments,
                               uint64_t *result);
#endif
