#ifndef LP32_PHYSICS_TRACE_H
#define LP32_PHYSICS_TRACE_H
#include "compat_runtime.h"
void lp32_physics_trace_install(const char *path, uint32_t slide, const unsigned char uuid[16]);
lp32_fast_import_fn lp32_physics_trace_handler(const char *name);
int lp32_physics_trace_selftest(void);
#endif
