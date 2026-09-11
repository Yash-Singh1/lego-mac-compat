#ifndef LP32_GUEST_DYLD_H
#define LP32_GUEST_DYLD_H

#include <stddef.h>
#include <stdint.h>

/* Maps and links guest i386 dependencies independently of the host dyld. */
int guest_dyld32_initialize(const char *image_path);
/* Handles dl* imports only after the per-title guest loader is initialized. */
int guest_dyld32_dispatch(const char *name, const uint32_t *arguments, uint64_t *result);
struct macho_image32;
int guest_dyld32_bind_main_cxx(const struct macho_image32 *image);
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
