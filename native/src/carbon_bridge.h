#ifndef LP32_CARBON_BRIDGE_H
#define LP32_CARBON_BRIDGE_H
#include <stdint.h>
void carbon_bridge32_draw_user_pane(uint32_t function, uint32_t control, int32_t part, void *context);
int carbon_bridge32_configure(const char *image_path);
void carbon_bridge32_service_keyboard_layout(void);
int carbon_bridge32_keyboard_self_test(void);
int carbon_bridge32_dispatch(const char *name, const uint32_t *args, uint64_t *result);
uint32_t carbon_bridge32_pointer_import(const char *name);
int carbon_bridge32_data_import(const char *name, void *data, uint32_t capacity);
#endif
