#ifndef LP32_CARBON_UI_H
#define LP32_CARBON_UI_H
#include <stdint.h>
int carbon_ui32_dispatch(const char *name, const uint32_t *args, uint64_t *result);
int carbon_ui32_test_keys(uint8_t keys[16]);
#endif
