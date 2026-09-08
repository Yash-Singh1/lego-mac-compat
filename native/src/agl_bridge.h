#ifndef LP32_AGL_BRIDGE_H
#define LP32_AGL_BRIDGE_H
#include <stdint.h>
int agl_bridge32_dispatch(const char *name, const uint32_t *args, uint64_t *result);
int agl_bridge32_self_test(void);
int agl_bridge32_gl_dispatch(const char *name, const uint32_t *args, uint64_t *result);
#endif
