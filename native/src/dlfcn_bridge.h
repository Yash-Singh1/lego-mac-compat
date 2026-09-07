#ifndef LP32_DLFCN_BRIDGE_H
#define LP32_DLFCN_BRIDGE_H
#include <stdint.h>
int dlfcn_bridge32_dispatch(const char *,const uint32_t *,uint64_t *);
#endif
