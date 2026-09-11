#ifndef LP32_ZLIB_BRIDGE_H
#define LP32_ZLIB_BRIDGE_H
#include <stdint.h>
int zlib_bridge32_dispatch(const char *name, const uint32_t *args, uint64_t *result);
uint64_t zlib_bridge32_inflate(const uint32_t *args, uint32_t caller);
uint64_t zlib_bridge32_deflate(const uint32_t *args, uint32_t caller);
#endif
