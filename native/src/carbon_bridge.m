#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>
#import <objc/runtime.h>
#include "carbon_bridge.h"
#include "compat_runtime.h"
#include "objc_bridge.h"
#include "game_profile.h"
#include "carbon_ui.h"
#include "carbon_files.h"
#include "cf_format.h"
#include "movie_bridge.h"
#include "agl_bridge.h"
#include "carbon_display.h"
#include "carbon_native.h"
#include "xml_bridge.h"
#include "gl_shader_bridge.h"
#include "gl_volume_bridge.h"
#include "gl_misc_bridge.h"
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static NSBundle *game_bundle;
static NSString *game_directory;
static uint32_t quickdraw_random_seed;
static pthread_mutex_t keyboard_layout_lock = PTHREAD_MUTEX_INITIALIZER;
static CFDataRef keyboard_layout_data;
static UInt32 keyboard_layout_type;
static _Atomic bool keyboard_layout_dirty = true;

static void keyboard_layout_changed(CFNotificationCenterRef center, void *observer,
    CFStringRef name, const void *object, CFDictionaryRef info)
{
    (void)center; (void)observer; (void)name; (void)object; (void)info;
    atomic_store(&keyboard_layout_dirty, true);
}

void carbon_bridge32_service_keyboard_layout(void)
{
    /* TFU calls KeyTranslate from its movie/input worker. Current HIToolbox
       requires the main queue for TIS property lookup. Publish immutable
       layout data here instead of synchronously dispatching from a worker
       that the guest's main thread may itself be waiting for. */
    if (![NSThread isMainThread]) return;
    static bool observing;
    if (!observing) {
        CFNotificationCenterAddObserver(CFNotificationCenterGetDistributedCenter(),
            &keyboard_layout_data, keyboard_layout_changed,
            kTISNotifySelectedKeyboardInputSourceChanged, NULL,
            CFNotificationSuspensionBehaviorDeliverImmediately);
        observing = true;
    }
    if (!atomic_exchange(&keyboard_layout_dirty, false)) return;
    TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
    CFDataRef data = source ? TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData) : NULL;
    if (data) CFRetain(data);
    UInt32 type = LMGetKbdType();
    if (source) CFRelease(source);
    pthread_mutex_lock(&keyboard_layout_lock);
    CFDataRef previous = keyboard_layout_data;
    keyboard_layout_data = data;
    keyboard_layout_type = type;
    pthread_mutex_unlock(&keyboard_layout_lock);
    if (previous) CFRelease(previous);
}
static id object(uint32_t handle) { return (id)objc_bridge32_host_object(handle); }
static uint32_t handle(id value) { return objc_bridge32_guest_object(value); }
static double double_words(const uint32_t *words) { double value; memcpy(&value, words, 8); return value; }
static float float_word(uint32_t word) { float value; memcpy(&value, &word, 4); return value; }
static CGRect rect_words(const uint32_t *words) { return CGRectMake(float_word(words[0]), float_word(words[1]), float_word(words[2]), float_word(words[3])); }
static uint32_t owned(CFTypeRef value) {
    uint32_t result = objc_bridge32_owned_object((void *)value);
    if (value) CFRelease(value);
    return result;
}

static CFStringRef preferences_application(uint32_t value)
{
    CFStringRef application = (CFStringRef)object(value);
    const char *override = getenv("LP32_TFU_PREFERENCES_ID");
    if (override && override[0]) return (CFStringRef)[NSString stringWithUTF8String:override];
    if (game_bundle && (!application || CFEqual(application, kCFPreferencesCurrentApplication)))
        return (CFStringRef)[game_bundle bundleIdentifier];
    return application;
}

/* Keep native MP IDs opaque and stable across repeated CurrentTaskID calls. */
static uint32_t mp_handle(void *pointer)
{
    if (!pointer) return 0;
    static NSMutableDictionary *ids;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (!ids) ids = [[NSMutableDictionary alloc] init];
    NSNumber *key = [NSNumber numberWithUnsignedLongLong:(uintptr_t)pointer];
    NSValue *value = [ids objectForKey:key];
    if (!value) { value = [NSValue valueWithPointer:pointer]; [ids setObject:value forKey:key]; }
    uint32_t result = handle(value);
    pthread_mutex_unlock(&lock);
    return result;
}

struct mp_task_context { uint32_t function, argument; };
static OSStatus mp_task_entry(void *opaque)
{
    struct mp_task_context context = *(struct mp_task_context *)opaque;
    free(opaque);
    @autoreleasepool {
        uint32_t result = compat_runtime32_call(context.function, &context.argument, 1);
        if (compat_runtime32_last_call_trapped()) { fflush(NULL); _Exit(EXIT_FAILURE); }
        return (OSStatus)result;
    }
}

struct apple_event_handler { uint32_t event_class, event_id, function, refcon; };
static struct apple_event_handler apple_event_handlers[32];
static unsigned apple_event_handler_count;

static OSErr apple_event_callback(const AppleEvent *event, AppleEvent *reply, SRefCon opaque)
{
    const struct apple_event_handler *handler = (const void *)(uintptr_t)opaque;
    uint32_t data = compat_runtime32_allocate(16, 1);
    if (!data) return memFullErr;
    uint32_t *descriptors = (void *)(uintptr_t)data;
    descriptors[0] = event->descriptorType;
    descriptors[1] = handle([NSValue valueWithPointer:event]);
    if (reply) {
        descriptors[2] = reply->descriptorType;
        descriptors[3] = handle([NSValue valueWithPointer:reply]);
    }
    uint32_t args[] = {data, reply ? data + 8 : 0, handler->refcon};
    OSErr result = (OSErr)compat_runtime32_call(handler->function, args, 3);
    compat_runtime32_deallocate(data);
    return result;
}

int carbon_bridge32_configure(const char *image_path)
{
    gl_shader_bridge32_enable_tfu_compat();
    gl_volume_bridge32_enable_tfu_compat();
    gl_misc_bridge32_enable_tfu_compat();
    NSString *path = [[NSString stringWithUTF8String:image_path] stringByStandardizingPath];
    if (![path isAbsolutePath]) path = [[[NSFileManager defaultManager] currentDirectoryPath] stringByAppendingPathComponent:path];
    NSString *parent = [path stringByDeletingLastPathComponent];
    NSString *bundle_path;
    if ([[parent lastPathComponent] isEqualToString:@"SharedSupport"]) {
        game_directory = [[parent stringByAppendingPathComponent:@"TFU"] copy];
        bundle_path = [game_directory stringByAppendingPathComponent:@"Star Wars The Force Unleashed.app"];
    } else {
        bundle_path = [[parent stringByDeletingLastPathComponent] stringByDeletingLastPathComponent];
        game_directory = [[bundle_path stringByDeletingLastPathComponent] copy];
    }
    game_bundle = [[NSBundle bundleWithPath:bundle_path] retain];
    if (!game_bundle || chdir([game_directory fileSystemRepresentation])) {
        fprintf(stderr, "compat32: cannot locate TFU app/resources beside %s\n", image_path);
        return -1;
    }
    carbon_bridge32_service_keyboard_layout();
    return 0;
}

uint32_t carbon_bridge32_pointer_import(const char *name)
{
    uint32_t movie = movie_bridge32_pointer_import(name);
    if (movie) return movie;
    if (!strcmp(name, "_kCFBooleanTrue")) return handle((id)kCFBooleanTrue);
    if (!strcmp(name, "_kCFBooleanFalse")) return handle((id)kCFBooleanFalse);
    if (!strcmp(name, "_kCFBundleNameKey")) return handle((id)kCFBundleNameKey);
    if (!strcmp(name, "_kCFPreferencesCurrentApplication")) return handle((id)kCFPreferencesCurrentApplication);
    return 0;
}

int carbon_bridge32_data_import(const char *name, void *data, uint32_t capacity) {
    if (!strcmp(name, "_kHIViewWindowContentID") && capacity >= 8) {
        const void *identifier = carbon_native32_symbol("kHIViewWindowContentID");
        if (!identifier) return 0;
        memcpy(data, identifier, 8); return 1;
    }
    if (!strcmp(name, "_kCFAbsoluteTimeIntervalSince1970") && capacity >= 8) {
        double value = kCFAbsoluteTimeIntervalSince1970;
        memcpy(data, &value, sizeof(value)); return 1;
    }
    if (!strcmp(name, "_CGAffineTransformIdentity") && capacity >= 24) {
        const float identity[6] = {1, 0, 0, 1, 0, 0}; memcpy(data, identity, sizeof(identity)); return 1;
    }
    return 0;
}

static _Thread_local uint32_t current_port;
@interface LP32PortPixels : NSObject {
@public uint32_t memory;
}
@end
@implementation LP32PortPixels
- (void)dealloc { if (memory) compat_runtime32_deallocate(memory); [super dealloc]; }
@end
static char port_pixels_key;
static char port_clip_key;
static char port_color_key;
static char port_background_key;

/* Minimal rectangular QuickDraw drawing used by Aspyr's monitor chooser.
   Its public PenState is packed identically in the i386 ABI. The containing
   HIView supplies a Quartz context in local, top-left coordinates. */
struct __attribute__((packed, aligned(2))) pen32 {
    int16_t location[2], size[2], mode; uint8_t pattern[8];
};
_Static_assert(sizeof(struct pen32) == 18, "i386 PenState");
static _Thread_local struct qd_drawing {
    CGContextRef context; uint16_t foreground[3], background[3]; struct pen32 pen;
} qd;
void carbon_bridge32_draw_user_pane(uint32_t function, uint32_t control, int32_t part, void *context) {
    struct qd_drawing previous = qd;
    memset(&qd, 0, sizeof(qd)); qd.context = context;
    qd.background[0] = qd.background[1] = qd.background[2] = UINT16_MAX;
    qd.pen.size[0] = qd.pen.size[1] = 1; qd.pen.mode = 8;
    memset(qd.pen.pattern, 255, sizeof(qd.pen.pattern));
    CGContextSaveGState(context); CGContextSetShouldAntialias(context, false);
    uint32_t args[] = {control, (uint32_t)part}; compat_runtime32_call(function, args, 2);
    CGContextRestoreGState(context); qd = previous;
}
static int quickdraw_dispatch(const char *name, const uint32_t *a, uint64_t *result) {
    if (!qd.context) return 0;
    void *pointer = (void *)(uintptr_t)a[0];
    *result = 0;
    if (!strcmp(name, "_GetForeColor")) memcpy(pointer, qd.foreground, 6);
    else if (!strcmp(name, "_GetBackColor")) memcpy(pointer, qd.background, 6);
    else if (!strcmp(name, "_RGBForeColor")) memcpy(qd.foreground, pointer, 6);
    else if (!strcmp(name, "_RGBBackColor")) memcpy(qd.background, pointer, 6);
    else if (!strcmp(name, "_GetPenState")) memcpy(pointer, &qd.pen, sizeof(qd.pen));
    else if (!strcmp(name, "_SetPenState")) memcpy(&qd.pen, pointer, sizeof(qd.pen));
    else if (!strcmp(name, "_PenNormal")) {
        qd.pen.size[0] = qd.pen.size[1] = 1; qd.pen.mode = 8;
        memset(qd.pen.pattern, 255, sizeof(qd.pen.pattern));
    } else if (!strcmp(name, "_PenSize")) { qd.pen.size[0] = a[1]; qd.pen.size[1] = a[0]; }
    else if (!strcmp(name, "_MoveTo")) { qd.pen.location[0] = a[1]; qd.pen.location[1] = a[0]; }
    else if (!strcmp(name, "_LineTo")) {
        CGContextSetRGBStrokeColor(qd.context, qd.foreground[0]/65535., qd.foreground[1]/65535., qd.foreground[2]/65535., 1);
        CGContextSetLineWidth(qd.context, MAX(qd.pen.size[0], qd.pen.size[1]));
        CGContextMoveToPoint(qd.context, qd.pen.location[1]+0.5, qd.pen.location[0]+0.5);
        CGContextAddLineToPoint(qd.context, (int16_t)a[0]+0.5, (int16_t)a[1]+0.5); CGContextStrokePath(qd.context);
        qd.pen.location[0] = a[1]; qd.pen.location[1] = a[0];
    } else if (!strcmp(name, "_PaintRect") || !strcmp(name, "_EraseRect") || !strcmp(name, "_FrameRect")) {
        const int16_t *r = pointer; CGRect rect = CGRectMake(r[1], r[0], r[3]-r[1], r[2]-r[0]);
        const uint16_t *color = !strcmp(name, "_EraseRect") ? qd.background : qd.foreground;
        CGContextSetRGBFillColor(qd.context, color[0]/65535., color[1]/65535., color[2]/65535., 1);
        if (strcmp(name, "_FrameRect")) CGContextFillRect(qd.context, rect);
        else {
            CGFloat x = MAX(0, qd.pen.size[1]), y = MAX(0, qd.pen.size[0]);
            CGRect edges[] = {CGRectMake(rect.origin.x, rect.origin.y, rect.size.width, y),
                CGRectMake(rect.origin.x, CGRectGetMaxY(rect)-y, rect.size.width, y),
                CGRectMake(rect.origin.x, rect.origin.y, x, rect.size.height),
                CGRectMake(CGRectGetMaxX(rect)-x, rect.origin.y, x, rect.size.height)};
            CGContextFillRects(qd.context, edges, 4);
        }
    } else return 0;
    return 1;
}
int carbon_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *result)
{
    @autoreleasepool {
    if (!strncmp(name, "_gl", 3) || !strncmp(name, "gl", 2)) return 0;
    if (quickdraw_dispatch(name, a, result)) return 1;
    if (carbon_files32_dispatch(name, a, result)) return 1;
    if (movie_bridge32_dispatch(name, a, result)) return 1;
    if (agl_bridge32_dispatch(name, a, result)) return 1;
    if (carbon_display32_dispatch(name, a, result)) return 1;
    if (xml_bridge32_dispatch(name, a, result)) return 1;
#define IS(s) (!strcmp(name, "_" s))
#define PTR(i) ((void *)(uintptr_t)a[i])
#define OBJ(i) object(a[i])
#define RETURN(v) do { *result = (uint64_t)(v); return 1; } while (0)
    if (IS("X2Fix")) RETURN((uint32_t)X2Fix(double_words(a)));
    if (IS("Fix2X")) RETURN(compat_runtime32_return_double(Fix2X((int32_t)a[0])));
    if (IS("FixMul")) RETURN((uint32_t)FixMul((int32_t)a[0], (int32_t)a[1]));
    if (IS("GetGWorld")) {
        if (a[0]) *(uint32_t *)PTR(0) = current_port;
        uint64_t device = 0; carbon_display32_dispatch("_GetGDevice", a, &device);
        if (a[1]) *(uint32_t *)PTR(1) = device; RETURN(0);
    }
    if (IS("SetGWorld")) {
        current_port = a[0]; uint64_t ignored;
        if (a[1]) carbon_display32_dispatch("_SetGDevice", a + 1, &ignored);
        RETURN(0);
    }
    if (IS("GetPort")) { if (a[0]) *(uint32_t *)PTR(0) = current_port; RETURN(0); }
    if (IS("GetMouse")) {
        if (!a[0]) RETURN(0);
        NSPoint point = NSZeroPoint;
        if (!getenv("LP32_HEADLESS")) {
            point = [NSEvent mouseLocation];
            NSView *view = object(current_port);
            if ([view isKindOfClass:[NSView class]] && [view window]) {
                point = [view convertPoint:[[view window] convertPointFromScreen:point] fromView:nil];
                if (![view isFlipped]) point.y = NSMaxY([view bounds]) - point.y;
            } else point.y = CGDisplayBounds(CGMainDisplayID()).size.height - point.y;
        }
        int16_t *out = PTR(0); out[0] = point.y; out[1] = point.x; RETURN(0);
    }
    if (IS("LocalToGlobal") || IS("GlobalToLocal")) {
        int16_t *point = PTR(0); NSView *view = object(current_port);
        if (!point || ![view isKindOfClass:[NSView class]] || ![view window]) RETURN(0);
        NSPoint converted = NSMakePoint(point[1], point[0]);
        CGFloat screen_height = CGDisplayBounds(CGMainDisplayID()).size.height;
        if (IS("LocalToGlobal")) {
            if (![view isFlipped]) converted.y = NSMaxY([view bounds]) - converted.y;
            converted = [[view window] convertPointToScreen:[view convertPoint:converted toView:nil]];
            converted.y = screen_height - converted.y;
        } else {
            converted.y = screen_height - converted.y;
            converted = [view convertPoint:[[view window] convertPointFromScreen:converted] fromView:nil];
            if (![view isFlipped]) converted.y = NSMaxY([view bounds]) - converted.y;
        }
        point[0] = converted.y; point[1] = converted.x; RETURN(0);
    }
    if (IS("SetPort")) { current_port = a[0]; RETURN(0); }
    if (IS("SetPortWindowPort")) {
        uint64_t port = 0; carbon_native32_dispatch("_GetWindowPort", a, &port);
        current_port = port; RETURN(0);
    }
    if (IS("CreateNewPort") || IS("CreateNewPortForCGDisplayID")) {
        CGRect bounds = IS("CreateNewPortForCGDisplayID") ? CGDisplayBounds(a[0]) : CGRectZero;
        NSView *view = [[NSView alloc] initWithFrame:bounds];
        uint32_t port = objc_bridge32_owned_object(view); [view release]; RETURN(port);
    }
    if (IS("DisposePort")) {
        if (current_port == a[0]) current_port = 0;
        return objc_bridge32_dispatch("_CFRelease", a, result);
    }
    if (IS("GetPortBounds") || IS("SetPortBounds")) {
        NSView *view = OBJ(0); int16_t *rect = PTR(1);
        if (![view isKindOfClass:[NSView class]] || !rect) RETURN(0);
        if (IS("SetPortBounds")) [view setBounds:NSMakeRect(rect[1], rect[0], rect[3] - rect[1], rect[2] - rect[0])];
        else { NSRect bounds = carbon_display32_port_bounds(view); rect[0] = NSMinY(bounds); rect[1] = NSMinX(bounds); rect[2] = NSMaxY(bounds); rect[3] = NSMaxX(bounds); }
        RETURN(IS("GetPortBounds") ? a[1] : 0);
    }
    if (IS("GetPortPixMap") || IS("GetGWorldPixMap")) {
        NSView *view = OBJ(0);
        if (![view isKindOfClass:[NSView class]]) RETURN(0);
        LP32PortPixels *pixels = objc_getAssociatedObject(view, &port_pixels_key);
        if (!pixels) {
            pixels = [[[LP32PortPixels alloc] init] autorelease];
            pixels->memory = compat_runtime32_allocate(54, 1);
            if (!pixels->memory) RETURN(0);
            *(uint32_t *)(uintptr_t)pixels->memory = pixels->memory + 4;
            objc_setAssociatedObject(view, &port_pixels_key, pixels, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }
        uint8_t *map = (void *)(uintptr_t)(pixels->memory + 4);
        NSRect bounds = carbon_display32_port_bounds(view);
        int16_t rect[] = {NSMinY(bounds), NSMinX(bounds), NSMaxY(bounds), NSMaxX(bounds)};
        memcpy(map + 6, rect, 8);
        *(uint16_t *)(map + 4) = 0x8000 | (((uint32_t)bounds.size.width * 4) & 0x3fff);
        *(uint32_t *)(map + 22) = *(uint32_t *)(map + 26) = 72 << 16;
        *(int16_t *)(map + 30) = 16; *(int16_t *)(map + 32) = 32;
        *(int16_t *)(map + 34) = 3; *(int16_t *)(map + 36) = 8;
        *(uint32_t *)(map + 38) = 'BGRA';
        RETURN(pixels->memory);
    }
    if (IS("PaintRect") || IS("EraseRect") || IS("FrameRect") || IS("ClipRect")) {
        NSView *view = object(current_port); const int16_t *r = PTR(0);
        if (![view isKindOfClass:[NSView class]] || !r) RETURN(0);
        NSRect rect = NSMakeRect(r[1], r[0], r[3] - r[1], r[2] - r[0]);
        if (![view isFlipped]) rect.origin.y = NSMaxY([view bounds]) - NSMaxY(rect);
        if (IS("ClipRect")) objc_setAssociatedObject(view, &port_clip_key, [NSValue valueWithRect:rect], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        else if (!getenv("LP32_HEADLESS") && [view lockFocusIfCanDraw]) {
            [NSGraphicsContext saveGraphicsState];
            NSValue *clip = objc_getAssociatedObject(view, &port_clip_key);
            if (clip) NSRectClip([clip rectValue]);
            NSColor *color = IS("EraseRect") ? (objc_getAssociatedObject(view, &port_background_key) ?: [NSColor whiteColor]) : (objc_getAssociatedObject(view, &port_color_key) ?: [NSColor blackColor]);
            [color set];
            if (IS("PaintRect") || IS("EraseRect")) NSRectFill(rect); else NSFrameRect(rect);
            [NSGraphicsContext restoreGraphicsState]; [view unlockFocus];
        }
        RETURN(0);
    }
    if (IS("RGBForeColor") || IS("RGBBackColor")) {
        id port = object(current_port); const uint16_t *rgb = PTR(0);
        if (port && rgb) objc_setAssociatedObject(port, IS("RGBBackColor") ? &port_background_key : &port_color_key,
            [NSColor colorWithSRGBRed:rgb[0]/65535.0 green:rgb[1]/65535.0 blue:rgb[2]/65535.0 alpha:1], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        RETURN(0);
    }
    if (IS("GetPortForeColor") || IS("GetPortBackColor")) {
        NSColor *color = IS("GetPortBackColor") ? (objc_getAssociatedObject(OBJ(0), &port_background_key) ?: [NSColor whiteColor]) : (objc_getAssociatedObject(OBJ(0), &port_color_key) ?: [NSColor blackColor]);
        color = [color colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
        uint16_t *rgb = PTR(1);
        if (rgb) { rgb[0] = [color redComponent]*65535; rgb[1] = [color greenComponent]*65535; rgb[2] = [color blueComponent]*65535; }
        RETURN(a[1]);
    }
    if (IS("GetScriptManagerVariable")) {
        // KCHR is obsolete. KeyTranslate below uses the current Unicode
        // keyboard layout; this stable token represents that live layout.
        if ((int16_t)a[0] == smKCHRCache) {
            static uint32_t keyboard_token;
            if (!keyboard_token) keyboard_token = compat_runtime32_allocate(4, 1);
            RETURN(keyboard_token);
        }
        RETURN((uint32_t)GetScriptManagerVariable((int16_t)a[0]));
    }
    if (IS("KeyTranslate")) {
        carbon_bridge32_service_keyboard_layout();
        pthread_mutex_lock(&keyboard_layout_lock);
        CFDataRef data = keyboard_layout_data;
        UInt32 keyboard_type = keyboard_layout_type;
        if (data) CFRetain(data);
        pthread_mutex_unlock(&keyboard_layout_lock);
        if (!data) RETURN(0);
        UInt32 state = a[2] ? *(uint32_t *)PTR(2) : 0;
        UniChar chars[2]; UniCharCount length = 0;
        OSStatus status = UCKeyTranslate((const UCKeyboardLayout *)CFDataGetBytePtr(data), a[1] & 0x7f,
            a[1] & 0x80 ? kUCKeyActionUp : kUCKeyActionDown, (a[1] >> 8) & 0xff,
            keyboard_type, 0, &state, 2, &length, chars);
        if (a[2]) *(uint32_t *)PTR(2) = state;
        uint8_t bytes[2] = {0}; CFIndex used = 0;
        if (!status && length) {
            CFStringRef text = CFStringCreateWithCharacters(NULL, chars, length);
            CFStringGetBytes(text, CFRangeMake(0, length), kCFStringEncodingMacRoman, '?', false, bytes, 2, &used);
            CFRelease(text);
        }
        CFRelease(data);
        RETURN(used == 2 ? ((uint32_t)bytes[0] << 16) | bytes[1] : used ? bytes[0] : 0);
    }
    if (IS("GetCurrentEventButtonState") || IS("Button")) {
        uint32_t buttons = 0;
        if (!getenv("LP32_HEADLESS") && [NSApp isActive])
            for (unsigned i = 0; i < 32; ++i)
                if (CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, i)) buttons |= UINT32_C(1) << i;
        RETURN(IS("Button") ? !!(buttons & 1) : buttons);
    }
    if (IS("ValidWindowRect")) {
        NSWindow *window = carbon_native32_cocoa_window(a[0]);
        [[window contentView] setNeedsDisplay:NO]; RETURN(0);
    }
    if (IS("CGRectInset") || IS("CGRectOffset")) {
        CGRect rect = rect_words(a + 1);
        rect = IS("CGRectInset") ? CGRectInset(rect, float_word(a[5]), float_word(a[6])) : CGRectOffset(rect, float_word(a[5]), float_word(a[6]));
        float values[] = {rect.origin.x, rect.origin.y, rect.size.width, rect.size.height};
        memcpy(PTR(0), values, sizeof(values)); RETURN(a[0]);
    }
    if (IS("CGRectGetMinX") || IS("CGRectGetMaxX") || IS("CGRectGetMinY") || IS("CGRectGetMaxY")) {
        CGRect rect = rect_words(a);
        float value = IS("CGRectGetMinX") ? CGRectGetMinX(rect) : IS("CGRectGetMaxX") ? CGRectGetMaxX(rect) :
            IS("CGRectGetMinY") ? CGRectGetMinY(rect) : CGRectGetMaxY(rect);
        RETURN(compat_runtime32_return_float(value));
    }
    if (IS("CGRectContainsRect")) RETURN(CGRectContainsRect(rect_words(a), rect_words(a + 4)));
    if (IS("CGAffineTransformScale")) {
        CGAffineTransform transform = {float_word(a[1]), float_word(a[2]), float_word(a[3]), float_word(a[4]), float_word(a[5]), float_word(a[6])};
        transform = CGAffineTransformScale(transform, float_word(a[7]), float_word(a[8]));
        float values[] = {transform.a, transform.b, transform.c, transform.d, transform.tx, transform.ty};
        memcpy(PTR(0), values, sizeof(values)); RETURN(a[0]);
    }
    if (IS("SetRect")) { int16_t *rect = PTR(0); rect[0] = a[2]; rect[1] = a[1]; rect[2] = a[4]; rect[3] = a[3]; RETURN(0); }
    if (IS("InsetRect") || IS("OffsetRect")) {
        int16_t *rect = PTR(0), dx = a[1], dy = a[2];
        rect[0] += dy; rect[1] += dx;
        rect[2] += IS("InsetRect") ? -dy : dy; rect[3] += IS("InsetRect") ? -dx : dx; RETURN(0);
    }
    if (IS("EqualRect")) RETURN(!memcmp(PTR(0), PTR(1), 8));
    if (IS("PtInRect")) { const int16_t *point = (const void *)a, *rect = PTR(1); RETURN(point[0] >= rect[0] && point[0] < rect[2] && point[1] >= rect[1] && point[1] < rect[3]); }
    if (IS("Gestalt")) {
        int32_t value = 0;
        OSStatus status;
        if (a[0] == gestaltNativeCPUtype || a[0] == gestaltNativeCPUfamily) {
            /* Describe the i386 execution environment. The host's ARM family
               code is not understood by this Intel-only guest. */
            value = gestaltCPUX86; status = noErr;
        } else if (a[0] == gestaltQuickTimeVersion) {
            /* The QuickTime movie entry points are implemented by the
               AVFoundation adapter; no system QuickTime installation. */
            value = 0x07000000; status = noErr;
        } else status = Gestalt(a[0], &value);
        if (a[1]) *(int32_t *)PTR(1) = value;
        if (getenv("LP32_TRACE_SYSTEM")) fprintf(stderr, "compat32: Gestalt %08x -> %d status=%d\n", a[0], value, (int)status);
        RETURN((uint32_t)status);
    }
    if (IS("IsMenuBarVisible")) RETURN(!([NSApp presentationOptions] &
        (NSApplicationPresentationHideMenuBar | NSApplicationPresentationAutoHideMenuBar)));
    if (IS("HideMenuBar") || IS("ShowMenuBar")) {
        if (!getenv("LP32_HEADLESS")) {
            NSApplicationPresentationOptions options = [NSApp presentationOptions];
            options &= ~(NSApplicationPresentationHideMenuBar | NSApplicationPresentationAutoHideMenuBar);
            if (IS("HideMenuBar")) options |= NSApplicationPresentationAutoHideMenuBar | NSApplicationPresentationAutoHideDock;
            [NSApp setPresentationOptions:options];
        }
        RETURN(0);
    }
    if (IS("CreateStandardAlert")) {
        fprintf(stderr, "compat32: TFU alert: %s — %s\n",
            [OBJ(1) UTF8String] ?: "", [OBJ(2) UTF8String] ?: "");
    }
    if (IS("CreateWindowFromNib")) fprintf(stderr, "compat32: Carbon window from nib: %s\n", [OBJ(1) UTF8String] ?: "?");
    if (IS("GetStandardAlertDefaultParams")) {
        struct __attribute__((packed, aligned(2))) {
            uint32_t version;
            uint8_t movable, help;
            uint32_t ok, cancel, other;
            int16_t default_button, cancel_button;
            uint16_t position;
            uint32_t flags;
        } defaults = {a[1], 1, 0, (uint32_t)(uintptr_t)kAlertDefaultOKText,
            0, 0, kAlertStdAlertOKButton, 0, kWindowDefaultPosition, 0};
        _Static_assert(sizeof(defaults) == 28, "i386 alert parameters");
        if (!a[0] || (a[1] != 1 && a[1] != 2)) RETURN((uint32_t)paramErr);
        memcpy(PTR(0), &defaults, sizeof(defaults));
        if (a[1] == 2) memset((char *)PTR(0) + 28, 0, 4);
        RETURN(0);
    }
    bool create_hidden_window = getenv("LP32_BUILD_UI_ONLY") &&
        (IS("CreateNewWindow") || IS("CreateWindowFromNib") || IS("RunApplicationEventLoop"));
    if (getenv("LP32_HEADLESS") && !create_hidden_window &&
        (IS("CreateNewWindow") || IS("CreateWindowFromNib") || IS("CreateStandardAlert") ||
         IS("SetSystemUIMode") || IS("NSApplicationMain") || IS("RunApplicationEventLoop") ||
         IS("CGCaptureAllDisplays") || IS("CGDisplayCapture") ||
         IS("SetFrontProcess") || IS("TransformProcessType") || IS("ShowWindow") ||
         IS("SelectWindow") || IS("ShowSheetWindow") || IS("RunAppModalLoopForWindow"))) {
        fprintf(stderr, "compat32: headless startup reached UI call %s\n", name);
        fflush(NULL);
        _Exit(77);
    }
    if (create_hidden_window) {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
    }
    if (IS("CGCaptureAllDisplays")) {
        // AGL fullscreen is presented through the window compositor. Do not
        // take exclusive ownership of the user's other displays.
        RETURN(kCGErrorSuccess);
    }
    if (IS("CGReleaseAllDisplays")) return objc_bridge32_dispatch("_CGDisplayRelease", a, result);
    if (IS("MPProcessorsScheduled")) RETURN([[NSProcessInfo processInfo] activeProcessorCount]);
    if (IS("MPProcessors")) RETURN([[NSProcessInfo processInfo] processorCount]);
    if (IS("_MPIsFullyInitialized")) RETURN(1);
    if (IS("SetQDGlobalsRandomSeed")) { quickdraw_random_seed = a[0]; RETURN(0); }
    if (IS("GetQDGlobalsRandomSeed")) RETURN(quickdraw_random_seed);
    if (IS("GetDateTime")) {
        if (a[0]) *(uint32_t *)PTR(0) = (uint32_t)(CFAbsoluteTimeGetCurrent() + kCFAbsoluteTimeIntervalSince1904);
        RETURN(0);
    }
    if (IS("TickCount")) RETURN((uint32_t)([[NSProcessInfo processInfo] systemUptime] * 60.0));
    if (IS("GetCurrentEventTime")) RETURN(compat_runtime32_return_double([[NSProcessInfo processInfo] systemUptime]));
    if (IS("InitCursor")) {
        if (!getenv("LP32_HEADLESS")) [[NSCursor arrowCursor] set];
        RETURN(0);
    }
    if (IS("FlushEvents")) {
        if (!getenv("LP32_HEADLESS")) [NSApp discardEventsMatchingMask:NSEventMaskAny beforeEvent:nil];
        RETURN(0);
    }
    if (IS("GetCurrentProcess")) RETURN((uint32_t)GetCurrentProcess(PTR(0)));
    if (IS("GetProcessInformation")) {
        struct __attribute__((packed, aligned(2))) process_info32 {
            uint32_t length, name;
            ProcessSerialNumber number;
            uint32_t type, signature, mode, location, size, free_memory;
            ProcessSerialNumber launcher;
            uint32_t launch_date, active_time, app_spec;
        } *guest = PTR(1);
        _Static_assert(sizeof(*guest) == 60, "i386 process information");
        if (!guest || guest->length != 60) RETURN((uint32_t)paramErr);
        FSRef reference;
        ProcessInfoRec info = { .processInfoLength = sizeof(info),
            .processName = (void *)(uintptr_t)guest->name, .processAppRef = &reference };
        OSStatus status = GetProcessInformation(PTR(0), &info);
        if (status) RETURN((uint32_t)status);
        guest->number = info.processNumber; guest->type = info.processType;
        guest->signature = info.processSignature; guest->mode = info.processMode;
        guest->location = 0; guest->size = info.processSize; guest->free_memory = info.processFreeMem;
        guest->launcher = info.processLauncher; guest->launch_date = info.processLaunchDate;
        guest->active_time = info.processActiveTime;
        pid_t pid = 0; GetProcessPID(PTR(0), &pid);
        if (game_bundle && pid == getpid()) {
            FSPathMakeRef((const UInt8 *)[[game_bundle bundlePath] fileSystemRepresentation], &reference, NULL);
            if (guest->name) CFStringGetPascalString((CFStringRef)[game_bundle objectForInfoDictionaryKey:@"CFBundleName"],
                (void *)(uintptr_t)guest->name, 256, kCFStringEncodingMacRoman);
        }
        carbon_files32_make_spec(&reference, (void *)(uintptr_t)guest->app_spec);
        RETURN(0);
    }
    if (IS("GetFrontProcess")) RETURN((uint32_t)GetFrontProcess(PTR(0)));
    if (IS("SameProcess")) RETURN((uint32_t)SameProcess(PTR(0), PTR(1), PTR(2)));
    if (IS("GetProcessPID")) RETURN((uint32_t)GetProcessPID(PTR(0), PTR(1)));
    if (IS("GetProcessBundleLocation") && game_bundle)
        RETURN((uint32_t)FSPathMakeRef((const UInt8 *)[[game_bundle bundlePath] fileSystemRepresentation], PTR(1), NULL));
    if (IS("AEInstallEventHandler")) {
        unsigned i;
        for (i = 0; i < apple_event_handler_count; ++i)
            if (apple_event_handlers[i].event_class == a[0] && apple_event_handlers[i].event_id == a[1]) break;
        if (i == 32) RETURN((uint32_t)memFullErr);
        if (i == apple_event_handler_count) ++apple_event_handler_count;
        apple_event_handlers[i] = (struct apple_event_handler){a[0], a[1], a[2], a[3]};
        RETURN((uint32_t)AEInstallEventHandler(a[0], a[1], apple_event_callback,
            (SRefCon)(uintptr_t)&apple_event_handlers[i], a[4] != 0));
    }
    if (IS("AEGetParamPtr")) {
        const uint32_t *descriptor = PTR(0);
        const AppleEvent *event = descriptor ? [object(descriptor[1]) pointerValue] : NULL;
        Size size = 0;
        OSErr status = AEGetParamPtr(event, a[1], a[2], PTR(3), PTR(4), (int32_t)a[5], &size);
        if (a[6]) *(uint32_t *)PTR(6) = (uint32_t)size;
        RETURN((uint32_t)status);
    }
    if (IS("GetKeys")) {
        uint8_t *keys = PTR(0);
        if (keys) {
            memset(keys, 0, 16);
            if (carbon_ui32_test_keys(keys)) RETURN(0);
            if (!getenv("LP32_HEADLESS") && [NSApp isActive]) for (unsigned key = 0; key < 128; ++key) {
                if (CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, key)) keys[key / 8] |= 1u << (key % 8);
            }
        }
        RETURN(0);
    }
    if (IS("HideCursor") || IS("ShowCursor")) {
        uint32_t display = CGMainDisplayID();
        return objc_bridge32_dispatch(IS("HideCursor") ? "_CGDisplayHideCursor" : "_CGDisplayShowCursor", &display, result);
    }
    if (IS("CGDisplayMoveCursorToPoint")) {
        CGRect display = CGDisplayBounds(a[0]);
        float point[] = {display.origin.x + float_word(a[1]), display.origin.y + float_word(a[2])};
        return objc_bridge32_dispatch("_CGWarpMouseCursorPosition", (void *)point, result);
    }
    if (IS("CGGetDisplaysWithOpenGLDisplayMask")) RETURN(CGGetDisplaysWithOpenGLDisplayMask(a[0], a[1], PTR(2), PTR(3)));
    if (IS("CGDisplayBitsPerPixel")) RETURN(32); // BGRA presentation surface exposed to the guest
    if (IS("CGGetDisplaysWithPoint")) RETURN(CGGetDisplaysWithPoint(CGPointMake(float_word(a[0]), float_word(a[1])), a[2], PTR(3), PTR(4)));
    if (IS("MPCurrentTaskID")) RETURN(mp_handle(MPCurrentTaskID()));
    if (IS("MPAllocateTaskStorageIndex")) {
        TaskStorageIndex index = 0;
        OSStatus status = MPAllocateTaskStorageIndex(&index);
        if (a[0]) *(uint32_t *)PTR(0) = (uint32_t)index;
        RETURN((uint32_t)status);
    }
    if (IS("MPGetTaskStorageValue")) RETURN((uint32_t)(uintptr_t)MPGetTaskStorageValue(a[0]));
    if (IS("MPSetTaskStorageValue")) RETURN((uint32_t)MPSetTaskStorageValue(a[0], PTR(1)));
    if (IS("MPCreateTask")) {
        if (a[3] || !a[7]) RETURN((uint32_t)paramErr);
        struct mp_task_context *context = malloc(sizeof(*context));
        if (!context) RETURN((uint32_t)memFullErr);
        *context = (struct mp_task_context){a[0], a[1]};
        MPTaskID task = NULL;
        OSStatus status = MPCreateTask(mp_task_entry, context, a[2], NULL, PTR(4), PTR(5), a[6], &task);
        if (status) free(context);
        *(uint32_t *)PTR(7) = mp_handle(task);
        RETURN((uint32_t)status);
    }
    if (IS("MPExit")) { MPExit((OSStatus)a[0]); RETURN(0); }
    if (IS("MPCreateEvent")) {
        MPEventID event = NULL;
        OSStatus status = MPCreateEvent(&event);
        if (a[0]) *(uint32_t *)PTR(0) = mp_handle(event);
        RETURN((uint32_t)status);
    }
    if (IS("MPSetEvent")) RETURN((uint32_t)MPSetEvent([OBJ(0) pointerValue], a[1]));
    if (IS("MPWaitForEvent")) RETURN((uint32_t)MPWaitForEvent([OBJ(0) pointerValue], PTR(1), (Duration)a[2]));
    if (IS("MPDeleteEvent")) RETURN((uint32_t)MPDeleteEvent([OBJ(0) pointerValue]));
    if (IS("MPDelayUntil")) RETURN((uint32_t)MPDelayUntil(PTR(0)));
    if (IS("MPAllocateAligned")) {
        unsigned exponent = a[1] & 0xff;
        unsigned alignment = exponent == 254 ? 4096 : exponent == 255 ? 64 : exponent <= 16 ? 1u << exponent : 0;
        if (!alignment || a[0] > 1024u * 1024u * 1024u) RETURN(0);
        if (alignment < 16) alignment = 16;
        uint32_t base = compat_runtime32_allocate((size_t)a[0] + alignment + 4, a[2] & 1);
        uint32_t aligned = base ? (base + 4 + alignment - 1) & ~(alignment - 1) : 0;
        if (aligned) ((uint32_t *)(uintptr_t)aligned)[-1] = base;
        RETURN(aligned);
    }
    if (IS("MPFree")) {
        if (a[0]) compat_runtime32_deallocate(((uint32_t *)PTR(0))[-1]);
        RETURN(0);
    }
    if (IS("CFURLCreateFromFSRef")) RETURN(owned(CFURLCreateFromFSRef(NULL, PTR(1))));
    if (IS("CFURLGetFSRef")) RETURN(CFURLGetFSRef((CFURLRef)OBJ(0), PTR(1)));
    if (IS("FSPathMakeRef")) RETURN((uint32_t)FSPathMakeRef(PTR(0), PTR(1), PTR(2)));
    if (IS("FSFindFolder")) {
        const char *test_root = getenv("LP32_TFU_USER_DATA_ROOT");
        NSString *component = a[1] == 'docs' ? @"Documents" : a[1] == 'pref' ? @"Preferences" : a[1] == 'asup' ? @"Application Support" : nil;
        if (test_root && component) {
            NSString *path = [[NSString stringWithUTF8String:test_root] stringByAppendingPathComponent:component];
            if (a[2]) [[NSFileManager defaultManager] createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:NULL];
            RETURN((uint32_t)FSPathMakeRef((const UInt8 *)[path fileSystemRepresentation], PTR(3), NULL));
        }
    }
    if (IS("FSCompareFSRefs")) RETURN((uint32_t)FSCompareFSRefs(PTR(0), PTR(1)));
    if (IS("FSMakeFSRefUnicode")) RETURN((uint32_t)FSMakeFSRefUnicode(PTR(0), a[1], PTR(2), a[3], PTR(4)));
    if (IS("CFURLCopyFileSystemPath")) RETURN(owned(CFURLCopyFileSystemPath((CFURLRef)OBJ(0), a[1])));
    if (IS("CFURLCreateCopyAppendingPathComponent")) RETURN(owned(CFURLCreateCopyAppendingPathComponent(NULL, (CFURLRef)OBJ(1), (CFStringRef)OBJ(2), a[3] != 0)));
    if (IS("CFURLCreateCopyDeletingLastPathComponent")) RETURN(owned(CFURLCreateCopyDeletingLastPathComponent(NULL, (CFURLRef)OBJ(1))));
    if (IS("CFURLCreateWithFileSystemPathRelativeToBase")) RETURN(owned(CFURLCreateWithFileSystemPathRelativeToBase(NULL, (CFStringRef)OBJ(1), a[2], a[3] != 0, (CFURLRef)OBJ(4))));
    if (IS("CFURLCreateWithString")) RETURN(owned(CFURLCreateWithString(NULL, (CFStringRef)OBJ(1), (CFURLRef)OBJ(2))));
    if (IS("CFURLGetString")) RETURN(handle((id)CFURLGetString((CFURLRef)OBJ(0))));
    if (IS("CFURLCreateDataAndPropertiesFromResource")) {
        CFDataRef data = NULL; CFDictionaryRef properties = NULL;
        Boolean ok = CFURLCreateDataAndPropertiesFromResource(NULL, (CFURLRef)OBJ(1),
            a[2] ? &data : NULL, a[3] ? &properties : NULL, (CFArrayRef)OBJ(4), PTR(5));
        if (a[2]) *(uint32_t *)PTR(2) = owned(data);
        if (a[3]) *(uint32_t *)PTR(3) = owned(properties);
        RETURN(ok);
    }
    if (IS("CFURLWriteDataAndPropertiesToResource")) RETURN(CFURLWriteDataAndPropertiesToResource((CFURLRef)OBJ(0), (CFDataRef)OBJ(1), (CFDictionaryRef)OBJ(2), PTR(3)));
    if (IS("CFBundleGetMainBundle") && game_bundle) RETURN(handle(game_bundle));
    if (IS("CFBundleLoadExecutable")) RETURN([OBJ(0) load]);
    if (IS("CreateNibReferenceWithCFBundle")) {
        return carbon_native32_dispatch(name, a, result);
    }
    if (IS("CFBundleCopyBundleURL")) RETURN(handle([OBJ(0) bundleURL]));
    if (IS("CFBundleGetInfoDictionary")) RETURN(handle([OBJ(0) infoDictionary]));
    if (IS("CFBundleGetValueForInfoDictionaryKey")) RETURN(handle([OBJ(0) objectForInfoDictionaryKey:OBJ(1)]));
    if (IS("CFBundleCopyLocalizedString")) RETURN(handle([OBJ(0) localizedStringForKey:OBJ(1) value:OBJ(2) table:OBJ(3)]));
    if (IS("CFBundleGetPackageInfo")) {
        NSDictionary *info = [OBJ(0) infoDictionary];
        const char *type = [[info objectForKey:@"CFBundlePackageType"] UTF8String];
        const char *creator = [[info objectForKey:@"CFBundleSignature"] UTF8String];
        if (a[1]) *(uint32_t *)PTR(1) = type && strlen(type) == 4 ? CFSwapInt32BigToHost(*(const uint32_t *)type) : 'APPL';
        if (a[2]) *(uint32_t *)PTR(2) = creator && strlen(creator) == 4 ? CFSwapInt32BigToHost(*(const uint32_t *)creator) : 0x3f3f3f3f;
        RETURN(0);
    }
    if (IS("CFAllocatorGetDefault")) RETURN(0);
    if (IS("CFStringCreateWithFormat") || IS("CFStringCreateWithFormatAndArguments") ||
        IS("CFStringAppendFormat") || IS("CFStringAppendFormatAndArguments")) {
        bool indirect = IS("CFStringCreateWithFormatAndArguments") || IS("CFStringAppendFormatAndArguments");
        CFStringRef formatted = cf_format32((CFStringRef)OBJ(2), (CFDictionaryRef)OBJ(1), indirect ? PTR(3) : a + 3);
        if (IS("CFStringAppendFormat") || IS("CFStringAppendFormatAndArguments")) {
            if (formatted) { CFStringAppend((CFMutableStringRef)OBJ(0), formatted); CFRelease(formatted); }
            RETURN(0);
        }
        RETURN(owned(formatted));
    }
    if (IS("CFDictionaryCreateMutable")) RETURN(owned(CFDictionaryCreateMutable(NULL, (int32_t)a[1], &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)));
    if (IS("CFDictionaryCreateMutableCopy")) RETURN(owned(CFDictionaryCreateMutableCopy(NULL, (int32_t)a[1], (CFDictionaryRef)OBJ(2))));
    if (IS("CFDictionaryCreate")) {
        if ((int32_t)a[3] < 0 || a[3] > 1048576) RETURN(0);
        const uint32_t *keys = PTR(1), *values = PTR(2);
        NSMutableDictionary *dictionary = [NSMutableDictionary dictionaryWithCapacity:a[3]];
        for (uint32_t i = 0; i < a[3]; ++i) {
            id key = object(keys[i]), value = object(values[i]);
            if (!key || !value) RETURN(0);
            [dictionary setObject:value forKey:key];
        }
        RETURN(objc_bridge32_owned_object(dictionary));
    }
    if (IS("CFDictionaryGetCount")) RETURN([OBJ(0) count]);
    if (IS("CFDictionaryGetKeysAndValues")) {
        uint32_t *keys = PTR(1), *values = PTR(2); unsigned i = 0;
        for (id key in OBJ(0)) {
            if (keys) keys[i] = handle(key);
            if (values) values[i] = handle([OBJ(0) objectForKey:key]);
            ++i;
        }
        RETURN(0);
    }
    if (IS("CFArrayCreateMutable")) RETURN(owned(CFArrayCreateMutable(NULL, (int32_t)a[1], &kCFTypeArrayCallBacks)));
    if (IS("CFArrayAppendValue")) { if (OBJ(1)) [OBJ(0) addObject:OBJ(1)]; RETURN(0); }
    if (IS("CFArraySetValueAtIndex")) { CFArraySetValueAtIndex((CFMutableArrayRef)OBJ(0), (int32_t)a[1], OBJ(2)); RETURN(0); }
    if (IS("CFArrayGetFirstIndexOfValue")) RETURN((uint32_t)CFArrayGetFirstIndexOfValue((CFArrayRef)OBJ(0), CFRangeMake((int32_t)a[1], (int32_t)a[2]), OBJ(3)));
    if (IS("CFArrayApplyFunction")) {
        NSArray *array = OBJ(0);
        if ((int32_t)a[1] < 0 || (uint64_t)a[1] + a[2] > [array count]) RETURN(0);
        for (uint32_t i = 0; i < a[2]; ++i) { uint32_t args[] = {handle(array[a[1] + i]), a[4]}; compat_runtime32_call(a[3], args, 2); }
        RETURN(0);
    }
    if (IS("CFStringCreateCopy")) RETURN(owned(CFStringCreateCopy(NULL, (CFStringRef)OBJ(1))));
    if (IS("CFStringCreateWithCharacters")) RETURN(owned(CFStringCreateWithCharacters(NULL, PTR(1), (int32_t)a[2])));
    if (IS("CFStringCreateWithPascalString")) RETURN(owned(CFStringCreateWithPascalString(NULL, PTR(1), a[2])));
    if (IS("CFStringCreateMutable")) RETURN(owned(CFStringCreateMutable(NULL, (int32_t)a[1])));
    if (IS("CFStringCreateMutableCopy")) RETURN(owned(CFStringCreateMutableCopy(NULL, (int32_t)a[1], (CFStringRef)OBJ(2))));
    if (IS("CFStringAppend")) { CFStringAppend((CFMutableStringRef)OBJ(0), (CFStringRef)OBJ(1)); RETURN(0); }
    if (IS("CFStringTrimWhitespace")) { CFStringTrimWhitespace((CFMutableStringRef)OBJ(0)); RETURN(0); }
    if (IS("CFStringReplace")) { CFStringReplace((CFMutableStringRef)OBJ(0), CFRangeMake((int32_t)a[1], (int32_t)a[2]), (CFStringRef)OBJ(3)); RETURN(0); }
    if (IS("CFStringGetCharacters")) { CFStringGetCharacters((CFStringRef)OBJ(0), CFRangeMake((int32_t)a[1], (int32_t)a[2]), PTR(3)); RETURN(0); }
    if (IS("CFStringGetCStringPtr")) RETURN(0); /* permitted: caller uses CFStringGetCString */
    if (IS("CFStringGetIntValue")) RETURN((uint32_t)CFStringGetIntValue((CFStringRef)OBJ(0)));
    if (IS("CFStringGetMaximumSizeForEncoding")) RETURN((uint32_t)CFStringGetMaximumSizeForEncoding((int32_t)a[0], a[1]));
    if (IS("CFStringGetTypeID")) RETURN(CFStringGetTypeID());
    if (IS("CFNumberGetTypeID")) RETURN(CFNumberGetTypeID());
    if (IS("CFDateGetTypeID")) RETURN(CFDateGetTypeID());
    if (IS("CFDateCreate")) RETURN(owned(CFDateCreate(NULL, double_words(a + 1))));
    if (IS("CFDateCompare")) RETURN((uint32_t)CFDateCompare((CFDateRef)OBJ(0), (CFDateRef)OBJ(1), NULL));
    if (IS("CFTimeZoneCopyDefault")) RETURN(owned(CFTimeZoneCopyDefault()));
    if (IS("CFAbsoluteTimeGetDayOfWeek")) RETURN(CFAbsoluteTimeGetDayOfWeek(double_words(a), (CFTimeZoneRef)OBJ(2)));
    if (IS("CFAbsoluteTimeGetGregorianDate")) {
        CFGregorianDate date = CFAbsoluteTimeGetGregorianDate(double_words(a + 1), (CFTimeZoneRef)OBJ(3));
        memcpy(PTR(0), &date, sizeof(date));
        RETURN(a[0]);
    }
    if (IS("CFGregorianDateGetAbsoluteTime")) {
        CFGregorianDate date; memcpy(&date, a, sizeof(date));
        RETURN(compat_runtime32_return_double(CFGregorianDateGetAbsoluteTime(date, (CFTimeZoneRef)OBJ(4))));
    }
    if (IS("CFAbsoluteTimeAddGregorianUnits")) {
        CFGregorianUnits units = {(int32_t)a[3], (int32_t)a[4], (int32_t)a[5], (int32_t)a[6], (int32_t)a[7], double_words(a + 8)};
        RETURN(compat_runtime32_return_double(CFAbsoluteTimeAddGregorianUnits(double_words(a), (CFTimeZoneRef)OBJ(2), units)));
    }
    if (IS("CFDataCreate")) RETURN(owned(CFDataCreate(NULL, PTR(1), (int32_t)a[2])));
    if (IS("CFPropertyListCreateFromXMLData")) {
        CFStringRef error = NULL;
        CFPropertyListRef value = CFPropertyListCreateFromXMLData(NULL, (CFDataRef)OBJ(1), a[2], &error);
        if (a[3]) *(uint32_t *)PTR(3) = owned(error); else if (error) CFRelease(error);
        RETURN(owned(value));
    }
    if (IS("CFPreferencesCopyAppValue")) RETURN(owned(CFPreferencesCopyAppValue((CFStringRef)OBJ(0), preferences_application(a[1]))));
    if (IS("CFPreferencesSetAppValue")) { CFPreferencesSetAppValue((CFStringRef)OBJ(0), (CFPropertyListRef)OBJ(1), preferences_application(a[2])); RETURN(0); }
    if (IS("CFPreferencesAppSynchronize")) RETURN(CFPreferencesAppSynchronize(preferences_application(a[0])));
    if (IS("CFPreferencesGetAppBooleanValue")) RETURN(CFPreferencesGetAppBooleanValue((CFStringRef)OBJ(0), preferences_application(a[1]), PTR(2)));
    if (IS("CFPreferencesGetAppIntegerValue")) RETURN((uint32_t)CFPreferencesGetAppIntegerValue((CFStringRef)OBJ(0), preferences_application(a[1]), PTR(2)));
    return carbon_ui32_dispatch(name, a, result);
#undef IS
#undef PTR
#undef OBJ
#undef RETURN
    }
}

struct keyboard_test_context {
    uint64_t expected[256];
    _Atomic bool done;
    bool failed;
};
static void *keyboard_test_worker(void *opaque)
{
    struct keyboard_test_context *test = opaque;
    @autoreleasepool {
        for (unsigned repeat = 0; repeat < 16; ++repeat) {
            for (unsigned key = 0; key < 256; ++key) {
                uint32_t args[] = {0, key, 0};
                uint64_t result = 0;
                if (!carbon_bridge32_dispatch("_KeyTranslate", args, &result) ||
                    result != test->expected[key]) test->failed = true;
            }
        }
    }
    atomic_store(&test->done, true);
    return NULL;
}
int carbon_bridge32_keyboard_self_test(void)
{
    /* Exercise the actual guest import on a worker while the main thread
       replaces its layout snapshot. The previous TIS lookup aborts here. */
    struct keyboard_test_context test = {0};
    carbon_bridge32_service_keyboard_layout();
    for (unsigned key = 0; key < 256; ++key) {
        uint32_t args[] = {0, key, 0};
        if (!carbon_bridge32_dispatch("_KeyTranslate", args, &test.expected[key])) return -1;
    }
    if (test.expected[36] != '\r' || test.expected[53] != 27) return -1;
    pthread_t thread;
    if (pthread_create(&thread, NULL, keyboard_test_worker, &test)) return -1;
    while (!atomic_load(&test.done)) {
        keyboard_layout_changed(NULL, NULL, NULL, NULL, NULL);
        carbon_bridge32_service_keyboard_layout();
        usleep(1000);
    }
    pthread_join(thread, NULL);
    fprintf(stderr, "TFU keyboard self-test: %s worker key-down/up translation, Return/Escape, concurrent layout refresh\n",
        test.failed ? "FAIL" : "PASS");
    return test.failed ? -1 : 0;
}
