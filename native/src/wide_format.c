#include "wide_format.h"
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <wctype.h>
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

/* Scan one conversion at a time so native long/pointer outputs cannot
   overwrite adjacent i386 cells. wchar_t itself is 32 bits in both ABIs. */
int guest_vwscan(const wchar_t *input, const wchar_t *format,
                 const uint32_t *destinations)
{
    if (!input || !format || !destinations) { errno = EINVAL; return EOF; }
    const wchar_t *start = input;
    unsigned destination = 0;
    int assigned = 0;
    while (*format) {
        if (iswspace(*format)) {
            while (iswspace(*format)) ++format;
            while (iswspace(*input)) ++input;
            continue;
        }
        if (*format != '%' || format[1] == '%') {
            if (*format == '%') ++format;
            if (*input != *format) return !*input && !assigned ? EOF : assigned;
            ++input; ++format; continue;
        }
        ++format;
        bool suppress = *format == '*';
        if (suppress) ++format;
        wchar_t spec[256] = L"%";
        size_t cursor = 1;
        if (suppress) spec[cursor++] = '*';
        while (*format >= '0' && *format <= '9') {
            if (cursor >= 200) { errno = EINVAL; return EOF; }
            spec[cursor++] = *format++;
        }
        enum { normal, hh, h, l, ll, size_word, long_float } length = normal;
        if (*format == 'h') { ++format; length = *format == 'h' ? (++format, hh) : h; }
        else if (*format == 'l') { ++format; length = *format == 'l' ? (++format, ll) : l; }
        else if (*format == 'j' || *format == 'q') { ++format; length = ll; }
        else if (*format == 'z' || *format == 't') { ++format; length = size_word; }
        else if (*format == 'L') { ++format; length = long_float; }
        wchar_t conversion = *format++;
        bool integer = wcschr(L"diouxXn", conversion) != NULL;
        bool floating = wcschr(L"aAeEfFgG", conversion) != NULL;
        bool text = conversion == 's' || conversion == 'c' || conversion == '[';
        if (!conversion || (!integer && !floating && !text && conversion != 'p') || length == long_float) {
            errno = ENOTSUP; return EOF;
        }
        size_t output_size = length == hh ? 1 : length == h ? 2 : length == ll ? 8 : 4;
        // Darwin i386 long, size_t and ptrdiff_t are 32 bits.
        if (integer) {
            if (length == hh || length == h) { spec[cursor++] = 'h'; if (length == hh) spec[cursor++] = 'h'; }
            if (length == ll) { spec[cursor++] = 'l'; spec[cursor++] = 'l'; }
        } else if (length == l) { spec[cursor++] = 'l'; }
        if (floating) output_size = length == l ? 8 : 4;
        spec[cursor++] = conversion;
        if (conversion == '[') {
            if (*format == '^') spec[cursor++] = *format++;
            if (*format == ']') spec[cursor++] = *format++;
            while (*format && *format != ']' && cursor < 250) spec[cursor++] = *format++;
            if (*format != ']') { errno = EINVAL; return EOF; }
            spec[cursor++] = *format++;
        }
        spec[cursor++] = '%'; spec[cursor++] = 'n'; spec[cursor] = 0;
        void *output = suppress ? NULL : (void *)(uintptr_t)destinations[destination++];
        if (conversion == 'n') {
            uint64_t count = (uint64_t)(input - start);
            if (output) memcpy(output, &count, output_size);
            continue;
        }
        union { uint64_t integer; double floating; void *pointer; } temporary = {0};
        int consumed = -1;
        // Text output layouts already match, including %ls and %[...].
        // Scalar outputs use staging storage to handle native %p safely.
        int status = suppress ? swscanf(input, spec, &consumed) :
            swscanf(input, spec, text ? output : &temporary, &consumed);
        if (consumed < 0) return status == EOF && !assigned ? EOF : assigned;
        if (!suppress) {
            if (!text && output) memcpy(output, &temporary, output_size);
            ++assigned;
        }
        input += consumed;
    }
    return assigned;
}
