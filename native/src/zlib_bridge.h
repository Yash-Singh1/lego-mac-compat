#ifndef LP32_ZLIB_BRIDGE_H
#define LP32_ZLIB_BRIDGE_H
#include <stdint.h>
int zlib_bridge32_dispatch(const char *name, const uint32_t *args, uint64_t *result);
#endif
