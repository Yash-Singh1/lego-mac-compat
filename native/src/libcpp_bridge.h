#ifndef LP32_LIBCPP_BRIDGE_H
#define LP32_LIBCPP_BRIDGE_H
#include <stdint.h>
#include "compat_runtime.h"
int libcpp_bridge32_dispatch(const char *, const uint32_t *, uint64_t *);
lp32_fast_import_fn libcpp_bridge32_fast_import(const char *name);
#endif
