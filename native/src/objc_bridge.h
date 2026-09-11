#ifndef LP32_OBJC_BRIDGE_H
#define LP32_OBJC_BRIDGE_H

#include "compat_runtime.h"

#include <stdint.h>

uint32_t objc_bridge32_guest_selector(const char *name);
void *objc_bridge32_host_object(uint32_t token);
void objc_bridge32_display_size(uint32_t display, int32_t size[2]);
uint32_t objc_bridge32_guest_pointer(void *pointer);
uint32_t objc_bridge32_guest_object(void *object);

int objc_bridge32_dispatch(const char *import_name, const uint32_t *arguments,
                           uint64_t *result);
/* Direct handler for a hot OpenGL import, or NULL to keep using the chain. */
lp32_fast_import_fn objc_bridge32_fast_import(const char *import_name);
uint32_t objc_bridge32_pointer_import(const char *import_name);
int objc_bridge32_run_proxy_self_test(void);
int objc_bridge32_run_pointer_self_test(void);
int objc_bridge32_run_focus_self_test(int expected);
int objc_bridge32_prepare_shader_cache(void);
int objc_bridge32_run_gl_parameter_self_test(void);
int objc_bridge32_run_gl_buffer_self_test(void);
int objc_bridge32_run_gl_texture_self_test(void);

void objc_bridge32_pin_event(uint32_t, int);
int objc_bridge32_test_key_down(unsigned key);

void objc_bridge32_note_agl_frame(void);
void objc_bridge32_carbon_focus_changed(int active);
int objc_bridge32_run_carbon_input_self_test(void);

/* Keep Carbon polling consistent with mouse events delivered to the app. */
void objc_bridge32_record_mouse_position(int16_t horizontal, int16_t vertical);
#endif
