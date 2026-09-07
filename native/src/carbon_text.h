#ifndef LP32_CARBON_TEXT_H
#define LP32_CARBON_TEXT_H
#include <stdint.h>
int32_t carbon_text_attach(void *view, uint32_t token, void *text,
                           int password);
int carbon_text_is_control(void *view);
void *carbon_text_copy_value(void *view);
void carbon_text_set_value(void *view,void *value);
#endif
