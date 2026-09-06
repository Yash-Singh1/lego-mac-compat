#ifndef LP32_WIDE_FORMAT_H
#define LP32_WIDE_FORMAT_H
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
int guest_vwformat(wchar_t *output, size_t capacity, const wchar_t *format,
                  const uint32_t *words);
int guest_vwscan(const wchar_t *input, const wchar_t *format,
                 const uint32_t *destinations);
#endif
