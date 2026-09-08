#ifndef LP32_MOVIE_BRIDGE_H
#define LP32_MOVIE_BRIDGE_H
#include <stdint.h>
#include <stdbool.h>
bool movie_bridge32_active(void);
int movie_bridge32_dispatch(const char *name, const uint32_t *args, uint64_t *result);
uint32_t movie_bridge32_pointer_import(const char *name);
int movie_bridge32_self_test(const char *path);
void movie_bridge32_service_main(void);
#endif
