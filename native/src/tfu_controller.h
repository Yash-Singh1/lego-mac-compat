#ifndef LP32_TFU_CONTROLLER_H
#define LP32_TFU_CONTROLLER_H
#include <stdint.h>
int tfu_controller32_install(void);
int tfu_controller32_dispatch(const char *name, const uint32_t *args, uint64_t *result);
int tfu_controller32_self_test(void);
#endif
