#ifndef LP32_RTTI_BRIDGE_H
#define LP32_RTTI_BRIDGE_H
#include <stdint.h>
struct macho_image32;
void rtti_bridge32_initialize(const struct macho_image32 *image);
uint32_t rtti_bridge32_cast(uint32_t object, uint32_t source, uint32_t target);
#endif
