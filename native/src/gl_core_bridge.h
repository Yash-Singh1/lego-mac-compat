#ifndef LP32_GL_CORE_BRIDGE_H
#define LP32_GL_CORE_BRIDGE_H
#include "compat_runtime.h"
lp32_fast_import_fn gl_core_bridge32_fast_import(const char *);
int gl_core_bridge32_dispatch(const char *, const uint32_t *, uint64_t *);
/* malloc'd shader with sun-shadow samples outside the map forced lit, or NULL. */
char *gl_core_bridge32_rewrite_glsl_shadows(const char *source, size_t length);
#endif
