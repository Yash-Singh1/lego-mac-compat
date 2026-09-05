#ifndef LP32_GUEST_MEMORY_H
#define LP32_GUEST_MEMORY_H
#include <stdint.h>

uint32_t guest_memory32_map(uint32_t hint, uint32_t length, int protection,
                            int flags, int descriptor, int64_t offset);
int guest_memory32_unmap(uint32_t address, uint32_t length);
int guest_memory32_protect(uint32_t address, uint32_t length, int protection);
int guest_memory32_advise(uint32_t address, uint32_t length, int advice);
int guest_memory32_sync(uint32_t address, uint32_t length, int flags);
uint32_t guest_memory32_size(uint32_t address);
#endif
