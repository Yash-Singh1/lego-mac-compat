#ifndef LP32_TFU_INPUT_H
#define LP32_TFU_INPUT_H
#include <stdint.h>
int tfu_input32_install(void);
int tfu_input32_dispatch(const char *name, const uint32_t *args, uint64_t *result);
int tfu_input32_self_test(void);
#endif
