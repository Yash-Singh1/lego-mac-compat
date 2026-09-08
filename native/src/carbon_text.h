#ifndef LP32_CARBON_TEXT_H
#define LP32_CARBON_TEXT_H
#include <stdint.h>
int carbon_text32_dispatch(const char *name, const uint32_t *args, uint64_t *result);
void carbon_text32_layout(void);
void carbon_text32_remove_window(void *window);
#endif
