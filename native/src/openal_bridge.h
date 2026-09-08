#ifndef LP32_OPENAL_BRIDGE_H
#define LP32_OPENAL_BRIDGE_H
#include <stdint.h>
int openal_bridge32_dispatch(const char *name, const uint32_t *args, uint64_t *result);
int openal_bridge32_self_test(void);
#endif
