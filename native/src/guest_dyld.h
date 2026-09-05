#ifndef LP32_GUEST_DYLD_H
#define LP32_GUEST_DYLD_H

#include <stddef.h>
#include <stdint.h>

/* Source's dlopen/dlsym run against guest i386 images, never host dlopen. */
int guest_dyld32_initialize(const char *image_path);
const char *guest_dyld32_game_root(void);
uint32_t guest_dyld32_open(const char *path, int flags);
uint32_t guest_dyld32_symbol(uint32_t handle, const char *symbol);
int guest_dyld32_close(uint32_t handle);
uint32_t guest_dyld32_error(void);
uint32_t guest_dyld32_symbol_module(uint32_t address);
uint32_t guest_dyld32_module_name(uint32_t handle);
int guest_dyld32_self_test(void);
int guest_dyld32_contains(uint32_t address, size_t size);
int guest_dyld32_section_contains(const char *section, uint32_t address);
const char *guest_dyld32_describe(uint32_t address, uint32_t *offset);

#endif
