#ifndef LP32_XML_BRIDGE_H
#define LP32_XML_BRIDGE_H
#include <stdint.h>
int xml_bridge32_dispatch(const char *name, const uint32_t *args, uint64_t *result);
#endif
