#ifndef LP32_TLV_BRIDGE_H
#define LP32_TLV_BRIDGE_H
#include "macho_loader.h"
#include <stdint.h>
int tlv_bridge32_initialize(const struct macho_image32 *);
int tlv_bridge32_dispatch(const char *,const uint32_t *,uint64_t *);
#endif
