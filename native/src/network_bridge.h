#ifndef LP32_NETWORK_BRIDGE_H
#define LP32_NETWORK_BRIDGE_H
#include <stdint.h>
int network_bridge32_dispatch(const char *name, const uint32_t *arguments,
                              uint64_t *result);
#endif
