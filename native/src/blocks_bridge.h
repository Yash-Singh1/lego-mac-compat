#ifndef LP32_BLOCKS_BRIDGE_H
#define LP32_BLOCKS_BRIDGE_H
#include <stdint.h>
uint32_t blocks_bridge32_copy(uint32_t block);
void blocks_bridge32_release(uint32_t block);
uint32_t blocks_bridge32_invoke(uint32_t block);
int blocks_bridge32_dispatch(const char *,const uint32_t *,uint64_t *);
#endif
