#ifndef LP32_STEAM_BRIDGE_H
#define LP32_STEAM_BRIDGE_H
#include <stdint.h>
/* Returns nonzero only for handled Steam imports. */
int steam_bridge32_dispatch(const char *name, const uint32_t *args, uint64_t *result);
int steam_bridge32_probe_storage(void);
#endif
