#include "cf_format.h"
#include "objc_bridge.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

enum argument_kind { NONE, SIGNED32, UNSIGNED32, BITS64, EXTENDED, OBJECT };
struct format_arguments { enum argument_kind kinds[256]; unsigned next, count; };

static unsigned position(const char **cursor)
{
    const char *p = *cursor; unsigned value = 0;
    while (isdigit((unsigned char)*p) && value <= 256) value = value * 10 + (unsigned)(*p++ - '0');
    if (*p != '$') return 0;
    *cursor = p + 1;
    return value ? value : 257;
}
static int argument(struct format_arguments *list, unsigned index, enum argument_kind kind)
{
    if (!index) index = ++list->next;
    if (index > 256 || (list->kinds[index - 1] && list->kinds[index - 1] != kind)) return -1;
    list->kinds[index - 1] = kind;
    if (index > list->count) list->count = index;
    return 0;
}
static int parse(const char *p, struct format_arguments *list)
{
    while (*p) {
        if (*p++ != '%') continue;
        if (*p == '%') { ++p; continue; }
        unsigned index = position(&p);
        while (*p && strchr("-+ #0'", *p)) ++p;
        if (*p == '*') { ++p; if (argument(list, position(&p), SIGNED32)) return -1; }
        else while (isdigit((unsigned char)*p)) ++p;
        if (*p == '.') {
            ++p;
            if (*p == '*') { ++p; if (argument(list, position(&p), SIGNED32)) return -1; }
            else while (isdigit((unsigned char)*p)) ++p;
        }
        bool wide = false, extended = false;
        if (*p == 'l') { ++p; if (*p == 'l') { ++p; wide = true; } }
        else if (*p == 'q' || *p == 'j') { ++p; wide = true; }
        else if (*p == 'L') { ++p; extended = true; }
        else if (*p == 'h') { ++p; if (*p == 'h') ++p; }
        else if (*p == 'z' || *p == 't') ++p;
        char conversion = *p++;
        enum argument_kind kind;
        if (!conversion) return -1;
        if (conversion == '@') kind = OBJECT;
        else if (strchr("aAeEfFgG", conversion)) kind = extended ? EXTENDED : BITS64;
        else if (strchr("diDcC", conversion)) kind = wide ? BITS64 : SIGNED32;
        else if (strchr("ouxXUOsSp", conversion)) kind = wide ? BITS64 : UNSIGNED32;
        else return -1;
        if (argument(list, index, kind)) return -1;
    }
    return 0;
}

CFStringRef cf_format32(CFStringRef format, CFDictionaryRef options, const uint32_t *arguments)
{
    if (!format || !arguments) return NULL;
    CFIndex capacity = CFStringGetMaximumSizeForEncoding(CFStringGetLength(format), kCFStringEncodingUTF8);
    if (capacity < 0 || capacity > 1024 * 1024) return NULL;
    char *text = malloc((size_t)capacity + 1);
    if (!text) return NULL;
    struct format_arguments list = {0};
    bool valid = CFStringGetCString(format, text, capacity + 1, kCFStringEncodingUTF8) && !parse(text, &list);
    free(text);
    if (!valid) return NULL;
    uint64_t stack[512] __attribute__((aligned(16)));
    size_t count = 0;
    for (unsigned i = 0; i < list.count; ++i) {
        switch (list.kinds[i]) {
            case SIGNED32: stack[count++] = (uint64_t)(int64_t)(int32_t)*arguments++; break;
            case UNSIGNED32: stack[count++] = *arguments++; break;
            case OBJECT: stack[count++] = (uintptr_t)objc_bridge32_host_object(*arguments++); break;
            case BITS64: memcpy(stack + count++, arguments, 8); arguments += 2; break;
            case EXTENDED:
                if (count & 1) stack[count++] = 0;
                if (count + 2 > 512) return NULL;
                memcpy(stack + count, arguments, 16); count += 2; arguments += 4; break;
            default: return NULL; /* positional formats must describe every preceding argument */
        }
    }
    /* x86_64 SysV va_list. Mark register slots exhausted so CoreFoundation
       reads our correctly widened values from the overflow argument area. */
    struct { unsigned gp_offset, fp_offset; void *overflow, *registers; } state = {48, 176, stack, NULL};
    va_list native_arguments;
    _Static_assert(sizeof(native_arguments) == sizeof(state), "x86_64 varargs ABI");
    memcpy(&native_arguments, &state, sizeof(state));
    return CFStringCreateWithFormatAndArguments(NULL, options, format, native_arguments);
}
