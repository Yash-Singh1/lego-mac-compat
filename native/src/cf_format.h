#ifndef LP32_CF_FORMAT_H
#define LP32_CF_FORMAT_H
#include <CoreFoundation/CoreFoundation.h>
#include <stdint.h>
CFStringRef cf_format32(CFStringRef format, CFDictionaryRef options, const uint32_t *arguments);
#endif
