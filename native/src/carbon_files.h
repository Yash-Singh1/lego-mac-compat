#ifndef LP32_CARBON_FILES_H
#define LP32_CARBON_FILES_H
#include <stdint.h>
int carbon_files32_dispatch(const char *name, const uint32_t *args, uint64_t *result);
void carbon_files32_make_spec(const void *reference, void *output);
#endif
