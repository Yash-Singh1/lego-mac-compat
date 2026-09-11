#ifndef LP32_MACHO_FILE_H
#define LP32_MACHO_FILE_H

#include <stddef.h>
#include <stdint.h>

/* Borrows the native-endian i386 slice of a thin or universal Mach-O. */
int macho_file32_slice(const void *bytes, size_t size,
                       const uint8_t **slice, size_t *slice_size);

#endif
