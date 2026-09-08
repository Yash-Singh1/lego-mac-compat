#ifndef LP32_CARBON_NATIVE_H
#define LP32_CARBON_NATIVE_H
#include <stdint.h>
#include <stdbool.h>
void carbon_native32_display_mode(uint32_t display, int32_t width, int32_t height);
bool carbon_native32_surface_size(void *window, int32_t *size);
void carbon_native32_report_windows(void);
void carbon_native32_pump_appkit_events(void);
void *carbon_native32_symbol(const char *name);
void *carbon_native32_pointer(uint32_t handle);
uint32_t carbon_native32_handle(void *pointer);
uint32_t carbon_native32_owned_cf(void *pointer);
void *carbon_native32_cocoa_window(uint32_t handle);
int carbon_native32_dispatch(const char *name, const uint32_t *args, uint64_t *result);
int32_t carbon_native32_event_parameter(void *event, const uint32_t *args);
int32_t carbon_native32_set_event_parameter(void *event, uint32_t name, uint32_t type, const void *data, uint32_t size);
#endif
