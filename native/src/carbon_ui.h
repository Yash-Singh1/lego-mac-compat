#ifndef LP32_CARBON_UI_H
#define LP32_CARBON_UI_H
#include <stdint.h>
void carbon_ui_show(void *window);
void carbon_ui_hide(void *window);
void carbon_ui_dispose(void *window);
int32_t carbon_ui_run_modal(void *window);
int carbon_ui_stop_modal(void *window);
int carbon_ui_is_visible(void *window);
void carbon_ui_use_native(void *window);
int32_t carbon_ui_show_sheet(void *sheet, void *parent);
int32_t carbon_ui_hide_sheet(void *sheet);
#endif
