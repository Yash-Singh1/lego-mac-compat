#ifndef LP32_CARBON_DISPLAY_H
#define LP32_CARBON_DISPLAY_H
#include <stdint.h>
#include <CoreGraphics/CGGeometry.h>
CGRect carbon_display32_port_bounds(void *port);
uint32_t carbon_display32_id(uint32_t device);
int carbon_display32_dispatch(const char *name, const uint32_t *args, uint64_t *result);
#endif
