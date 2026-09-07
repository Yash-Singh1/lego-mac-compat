#ifndef LP32_CFNETWORK_BRIDGE_H
#define LP32_CFNETWORK_BRIDGE_H
#include <stdint.h>
int cfnetwork_bridge32_dispatch(const char *, const uint32_t *, uint64_t *);
uint32_t cfnetwork_bridge32_pointer_import(const char *);
#endif
