#import <Foundation/Foundation.h>
#include <expat.h>
#include "xml_bridge.h"
#include "objc_bridge.h"
#include "compat_runtime.h"

@interface LP32XMLParser : NSObject {
@public XML_Parser parser;
    uint32_t user_data, start, end, text, allocator[3];
    bool trapped;
    NSMutableArray *callback_memory;
}
@end
static _Thread_local LP32XMLParser *active_parser;
@implementation LP32XMLParser
- (void)dealloc {
    if (parser) {
        LP32XMLParser *previous = active_parser; active_parser = self;
        XML_ParserFree(parser); active_parser = previous;
    }
    for (NSNumber *memory in callback_memory) compat_runtime32_deallocate([memory unsignedIntValue]);
    [callback_memory release];
    [super dealloc];
}
@end

static uint32_t guest_call(uint32_t function, const uint32_t *args, unsigned count) {
    uint32_t result = compat_runtime32_call(function, args, count);
    if (compat_runtime32_last_call_trapped()) active_parser->trapped = true;
    return result;
}
static void *guest_malloc(size_t size) {
    if (size > UINT32_MAX) return NULL;
    uint32_t arg = (uint32_t)size;
    return (void *)(uintptr_t)guest_call(active_parser->allocator[0], &arg, 1);
}
static void *guest_realloc(void *pointer, size_t size) {
    if (size > UINT32_MAX || (uintptr_t)pointer > UINT32_MAX) return NULL;
    uint32_t args[] = {(uint32_t)(uintptr_t)pointer, (uint32_t)size};
    return (void *)(uintptr_t)guest_call(active_parser->allocator[1], args, 2);
}
static void guest_free(void *pointer) {
    if (!pointer) return;
    uint32_t arg = (uint32_t)(uintptr_t)pointer;
    guest_call(active_parser->allocator[2], &arg, 1);
}
static uint32_t copy_bytes(const void *bytes, size_t length) {
    if (length > UINT32_MAX - 1) return 0;
    uint32_t memory = compat_runtime32_allocate((uint32_t)length + 1, 0);
    if (memory) { memcpy((void *)(uintptr_t)memory, bytes, length); *(char *)(uintptr_t)(memory + length) = 0; }
    return memory;
}
static void element_start(void *opaque, const XML_Char *name, const XML_Char **attributes) {
    LP32XMLParser *ctx = opaque;
    if (getenv("LP32_TRACE_XML_ELEMENTS") && (!strcmp(name, "technique") || !strcmp(name, "routine"))) {
        fprintf(stderr, "XML element %s callback=%08x data=%08x", name, ctx->start, ctx->user_data);
        for (unsigned i = 0; attributes[i]; i += 2) fprintf(stderr, " %s=%s", attributes[i], attributes[i+1]);
        fprintf(stderr, "\n");
    }
    unsigned count = 0; size_t bytes = 0;
    while (attributes[count]) {
        bytes += strlen(attributes[count++]) + 1;
        if (count > 1048576 || bytes > UINT32_MAX / 2) { XML_StopParser(ctx->parser, false); return; }
    }
    size_t pointers_size = (count + 1) * sizeof(uint32_t);
    uint32_t memory = compat_runtime32_allocate((uint32_t)(pointers_size + bytes), 1);
    uint32_t guest_name = copy_bytes(name, strlen(name));
    if (memory && guest_name) {
        uint32_t *pointers = (void *)(uintptr_t)memory, position = memory + (uint32_t)pointers_size;
        for (unsigned i = 0; i < count; ++i) {
            size_t size = strlen(attributes[i]) + 1;
            pointers[i] = position; memcpy((void *)(uintptr_t)position, attributes[i], size); position += size;
        }
        uint32_t args[] = {ctx->user_data, guest_name, memory};
        guest_call(ctx->start, args, 3);
    } else XML_StopParser(ctx->parser, false);
    if (guest_name) [ctx->callback_memory addObject:@(guest_name)];
    if (memory) [ctx->callback_memory addObject:@(memory)];
    if (ctx->trapped) XML_StopParser(ctx->parser, false);
}
static void element_end(void *opaque, const XML_Char *name) {
    LP32XMLParser *ctx = opaque; uint32_t memory = copy_bytes(name, strlen(name));
    if (memory) {
        uint32_t args[] = {ctx->user_data, memory}; guest_call(ctx->end, args, 2);
        [ctx->callback_memory addObject:@(memory)];
    } else XML_StopParser(ctx->parser, false);
    if (ctx->trapped) XML_StopParser(ctx->parser, false);
}
static void character_data(void *opaque, const XML_Char *data, int length) {
    LP32XMLParser *ctx = opaque; uint32_t memory = copy_bytes(data, length);
    if (memory) {
        uint32_t args[] = {ctx->user_data, memory, (uint32_t)length}; guest_call(ctx->text, args, 3);
        [ctx->callback_memory addObject:@(memory)];
    } else XML_StopParser(ctx->parser, false);
    if (ctx->trapped) XML_StopParser(ctx->parser, false);
}

int xml_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *result) {
    if (strncmp(name, "_XML_", 5)) return 0;
    @autoreleasepool {
        if (!strcmp(name, "_XML_ErrorString")) {
            const char *text = XML_ErrorString(a[0]);
            // Error strings are immutable and may be retained by the caller.
            static NSMutableDictionary *strings;
            if (!strings) strings = [[NSMutableDictionary alloc] init];
            NSNumber *key = @(a[0]); uint32_t handle = [strings[key] unsignedIntValue];
            if (!handle && text) { handle = copy_bytes(text, strlen(text)); strings[key] = @(handle); }
            *result = handle; return 1;
        }
        if (!strcmp(name, "_XML_ParserCreate_MM")) {
            LP32XMLParser *ctx = [[[LP32XMLParser alloc] init] autorelease];
            ctx->callback_memory = [[NSMutableArray alloc] init];
            if (a[1]) memcpy(ctx->allocator, (void *)(uintptr_t)a[1], sizeof(ctx->allocator));
            if (a[1] && (!ctx->allocator[0] || !ctx->allocator[1] || !ctx->allocator[2])) { *result = 0; return 1; }
            XML_Memory_Handling_Suite suite = {guest_malloc, guest_realloc, guest_free};
            LP32XMLParser *previous = active_parser; active_parser = ctx;
            ctx->parser = XML_ParserCreate_MM((void *)(uintptr_t)a[0], a[1] ? &suite : NULL, (void *)(uintptr_t)a[2]);
            active_parser = previous;
            if (ctx->parser) XML_SetUserData(ctx->parser, ctx);
            *result = ctx->parser ? objc_bridge32_owned_object(ctx) : 0; return 1;
        }
        LP32XMLParser *ctx = objc_bridge32_host_object(a[0]);
        if (![ctx isKindOfClass:[LP32XMLParser class]]) { *result = 0; return 1; }
        if (!strcmp(name, "_XML_ParserFree")) return objc_bridge32_dispatch("_CFRelease", a, result);
        if (!strcmp(name, "_XML_SetUserData")) { ctx->user_data = a[1]; *result = 0; return 1; }
        if (!strcmp(name, "_XML_SetElementHandler")) {
            ctx->start = a[1]; ctx->end = a[2];
            XML_SetElementHandler(ctx->parser, a[1] ? element_start : NULL, a[2] ? element_end : NULL);
            *result = 0; return 1;
        }
        if (!strcmp(name, "_XML_SetCharacterDataHandler")) {
            ctx->text = a[1]; XML_SetCharacterDataHandler(ctx->parser, a[1] ? character_data : NULL); *result = 0; return 1;
        }
        if (!strcmp(name, "_XML_GetErrorCode")) { *result = XML_GetErrorCode(ctx->parser); return 1; }
        if (!strcmp(name, "_XML_GetCurrentLineNumber")) { *result = (uint32_t)XML_GetCurrentLineNumber(ctx->parser); return 1; }
        if (!strcmp(name, "_XML_Parse")) {
            LP32XMLParser *previous = active_parser; active_parser = ctx;
            *result = XML_Parse(ctx->parser, (void *)(uintptr_t)a[1], a[2], a[3]);
            if (getenv("LP32_TRACE_XML") || !*result) fprintf(stderr,
                "XML: parse bytes=%u final=%u result=%llu error=%s input=%.*s\n", a[2], a[3],
                (unsigned long long)*result, XML_ErrorString(XML_GetErrorCode(ctx->parser)) ?: "none", (int)(a[2] < 100 ? a[2] : 100), (const char *)(uintptr_t)a[1]);
            active_parser = previous;
            if (ctx->trapped) { fprintf(stderr, "compat32: trapped XML callback\n"); fflush(NULL); _Exit(EXIT_FAILURE); }
            return 1;
        }
    }
    return 0;
}
