#ifndef LP32_RESOURCE_BRIDGE_H
#define LP32_RESOURCE_BRIDGE_H
#include <stdint.h>
int resource_bridge32_dispatch(const char *, const uint32_t *, uint64_t *);
int16_t resource_bridge32_open(const char *path);
#endif
