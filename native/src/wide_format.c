#include "wide_format.h"
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>

_Static_assert(sizeof(wchar_t) == 4 && sizeof(long double) == 16, "Darwin wide formatting ABI");

/* Darwin wchar_t is 32-bit in both processes; only the variadic arguments
   need conversion. Format one conversion at a time with the native locale.
   Positional arguments are rejected because they require a separate type
   pass before locating arguments in the i386 stack. */
int guest_vwformat(wchar_t *output, size_t capacity, const wchar_t *format,
                  const uint32_t *words)
{
    if (!output || !capacity || !format || !words) { errno = EINVAL; return -1; }
    size_t length = 0, word = 0;
    output[0] = 0;
    while (*format) {
        if (*format != '%' || format[1] == '%') {
            if (length + 1 >= capacity) { errno = EOVERFLOW; return -1; }
            if (*format == '%') ++format;
            output[length++] = *format++;
            output[length] = 0;
            continue;
        }
        const wchar_t *start = format++;
        while (*format && wcschr(L"-+ #0'", *format)) ++format;
        int stars[2] = {0}; unsigned star_count = 0;
        if (*format == '*') { stars[star_count++] = (int32_t)words[word++]; ++format; }
        else while (*format >= '0' && *format <= '9') ++format;
        if (*format == '.') {
            ++format;
            if (*format == '*') { stars[star_count++] = (int32_t)words[word++]; ++format; }
            else while (*format >= '0' && *format <= '9') ++format;
        }
        const wchar_t *modifier = format;
        if (*format && wcschr(L"hljztL", *format)) {
            wchar_t first = *format++;
            if ((first == 'h' || first == 'l') && *format == first) ++format;
        }
        wchar_t conversion = *format;
        if (!conversion || !wcschr(L"diouxXfFeEgGaAcCsSpn", conversion) || format - start > 60) {
            errno = EINVAL; return -1;
        }
        ++format;
        size_t modifiers = (size_t)(format - modifier - 1);
        bool wide = (*modifier == 'l' && modifiers == 2) || *modifier == 'j';
        bool integer = wcschr(L"diouxX", conversion) != NULL;
        if (conversion == 'n') {
            void *target = (void *)(uintptr_t)words[word++];
            if (!target) { errno = EINVAL; return -1; }
            if (*modifier == 'h' && modifiers == 2) *(int8_t *)target = (int8_t)length;
            else if (*modifier == 'h') *(int16_t *)target = (int16_t)length;
            else if (wide) *(int64_t *)target = (int64_t)length;
            else *(int32_t *)target = (int32_t)length;
            continue;
        }
        wchar_t spec[64];
        size_t prefix = (size_t)((integer ? modifier : format - 1) - start);
        wmemcpy(spec, start, prefix);
        /* Widen integer values explicitly; guest long/size_t/ptrdiff_t are
           four bytes, whereas native printf expects eight for those types. */
        if (integer) { spec[prefix++] = 'l'; spec[prefix++] = 'l'; }
        spec[prefix++] = conversion; spec[prefix] = 0;
        int count;
#define FORMAT(value) do { \
    if (star_count == 2) count = swprintf(output + length, capacity - length, spec, stars[0], stars[1], (value)); \
    else if (star_count == 1) count = swprintf(output + length, capacity - length, spec, stars[0], (value)); \
    else count = swprintf(output + length, capacity - length, spec, (value)); \
} while (0)
        if (integer) {
            uint64_t bits = words[word++];
            if (wide) { bits |= (uint64_t)words[word++] << 32; }
            if (conversion == 'd' || conversion == 'i') {
                int64_t value = wide ? (int64_t)bits : (int32_t)bits;
                if (*modifier == 'h') value = modifiers == 2 ? (int8_t)value : (int16_t)value;
                FORMAT((long long)value);
            } else {
                if (*modifier == 'h') bits = modifiers == 2 ? (uint8_t)bits : (uint16_t)bits;
                FORMAT((unsigned long long)bits);
            }
        } else if (wcschr(L"fFeEgGaA", conversion)) {
            if (*modifier == 'L') {
                long double value; memcpy(&value, words + word, sizeof(value));
                word += sizeof(value) / 4; FORMAT(value);
            } else {
                double value; memcpy(&value, words + word, sizeof(value));
                word += 2; FORMAT(value);
            }
        } else if (conversion == 'c' || conversion == 'C') {
            uint32_t value = words[word++]; FORMAT(value);
        } else {
            void *value = (void *)(uintptr_t)words[word++]; FORMAT(value);
        }
#undef FORMAT
        if (count < 0 || (size_t)count >= capacity - length) {
            output[capacity - 1] = 0;
            return -1;
        }
        length += (size_t)count;
    }
    if (length > INT_MAX) { errno = EOVERFLOW; return -1; }
    return (int)length;
}
