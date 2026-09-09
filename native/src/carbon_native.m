#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>
#import <ImageIO/ImageIO.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include "carbon_native.h"
#include "objc_bridge.h"
#include "app_termination.h"
#include "compat_runtime.h"
#include "carbon_text.h"
#include "game_profile.h"
#include "carbon_bridge.h"

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/* HIToolbox still exports these routines on current macOS, although some
   declarations are hidden by the public LP64 headers. Resolve each symbol
   explicitly so absence is reported as a missing bridge, not a loader error. */
void *carbon_native32_symbol(const char *name) {
    static void *library;
    static bool cache_enabled;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        library = dlopen("/System/Library/Frameworks/Carbon.framework/Carbon", RTLD_LAZY | RTLD_LOCAL);
        cache_enabled = getenv("LP32_NO_CARBON_SYMBOL_CACHE") == NULL;
    });
    if (!library) return NULL;
    /* ReceiveNextEvent and unhandled imports pass here thousands of times
       per second. A failed dlsym walks Carbon's entire dependency graph.
       Cache misses as well as exports; the framework remains loaded for the
       process lifetime. Copy keys because dispatch names may be temporary.
       Per-thread, bounded storage avoids adding a lock to the input loop. */
    static _Thread_local struct { char name[96]; void *symbol; } cache[64];
    size_t length = strlen(name);
    if (!cache_enabled || !length || length >= sizeof(cache[0].name)) return dlsym(library, name);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; ++i) hash = (hash ^ (unsigned char)name[i]) * 16777619u;
    unsigned slot = hash % 64;
    if (!strcmp(cache[slot].name, name)) return cache[slot].symbol;
    void *symbol = dlsym(library, name);
    memcpy(cache[slot].name, name, length + 1);
    cache[slot].symbol = symbol;
    return symbol;
}
static id object(uint32_t h) { return objc_bridge32_host_object(h); }
void carbon_native32_finish_appkit_launch(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{ [NSApp finishLaunching]; });
}
void *carbon_native32_pointer(uint32_t h) {
    id value = object(h);
    return [value isKindOfClass:[NSValue class]] && !strcmp([value objCType], @encode(void *)) ? [value pointerValue] : NULL;
}
uint32_t carbon_native32_handle(void *p) { return p ? objc_bridge32_guest_object([NSValue valueWithPointer:p]) : 0; }
static char owned_cf_key;
uint32_t carbon_native32_owned_cf(void *pointer) {
    uint32_t handle = carbon_native32_handle(pointer);
    if (handle) {
        id wrapper = object(handle);
        unsigned count = [objc_getAssociatedObject(wrapper, &owned_cf_key) unsignedIntValue];
        objc_setAssociatedObject(wrapper, &owned_cf_key, @(count + 1), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return handle;
}
static char cocoa_window_key;
static char window_refcon_key;
static char background_modal_done_key;
static char pane_draw_key, pane_hit_key, pane_track_key, pane_draw_handler_key;
static OSStatus pane_draw_event(EventHandlerCallRef next, EventRef event, void *control) {
    (void)next;
    uint32_t handle = carbon_native32_handle(control);
    uint32_t function = [objc_getAssociatedObject(object(handle), &pane_draw_key) unsignedIntValue];
    if (!function) return eventNotHandledErr;
    CGContextRef context = NULL;
    OSStatus status = GetEventParameter(event, kEventParamCGContextRef, typeCGContextRef, NULL, sizeof(context), NULL, &context);
    if (status || !context) return eventNotHandledErr;
    carbon_bridge32_draw_user_pane(function, handle, kControlEntireControl, context);
    return noErr;
}
static ControlPartCode pane_hit(ControlRef control, Point point) {
    uint32_t handle = carbon_native32_handle(control), packed; memcpy(&packed, &point, 4);
    uint32_t args[] = {handle, packed};
    return compat_runtime32_call([objc_getAssociatedObject(object(handle), &pane_hit_key) unsignedIntValue], args, 2);
}
static ControlPartCode pane_track(ControlRef control, Point point, ControlActionUPP action) {
    uint32_t handle = carbon_native32_handle(control), packed; memcpy(&packed, &point, 4);
    /* TFU's tracking routine ignores the action callback. Never expose a
       host code pointer to the guest. */
    (void)action;
    uint32_t args[] = {handle, packed, 0};
    return compat_runtime32_call([objc_getAssociatedObject(object(handle), &pane_track_key) unsignedIntValue], args, 3);
}
@interface LP32CarbonGameWindow : NSWindow {
@public void *carbonPeer;
    BOOL hasPresented;
    BOOL auxiliaryBackdrop, logicalVisible, fullscreenPresentation;
    NSRect windowedFrame;
    NSUInteger windowedStyle;
    CGDirectDisplayID fullscreenDisplay;
    int32_t surfaceSize[2];
    NSTimeInterval focusMismatchSince;
    BOOL focusMismatchReordered;
}
@end
static void restore_game_window(void);
static BOOL window_server_visible(NSWindow *window) {
    CFArrayRef entries = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow,
                                                   (CGWindowID)window.windowNumber);
    BOOL visible = NO;
    for (NSDictionary *entry in (NSArray *)entries)
        if ([entry[(id)kCGWindowNumber] unsignedIntValue] == (CGWindowID)window.windowNumber)
            visible = [entry[(id)kCGWindowIsOnscreen] boolValue];
    if (entries) CFRelease(entries);
    return visible;
}
static BOOL game_process_is_foreground(void) {
    /* NSWorkspace.frontmostApplication is notification-backed and can stay
       stale while the guest drives Carbon instead of NSApplication.run.
       Query the Process Manager directly; never raise based on cached state. */
    ProcessSerialNumber front = {0};
    pid_t pid = 0;
    return GetFrontProcess(&front) == noErr && GetProcessPID(&front, &pid) == noErr && pid == getpid();
}
@implementation LP32CarbonGameWindow
- (BOOL)canBecomeKeyWindow { return !auxiliaryBackdrop; }
- (BOOL)canBecomeMainWindow { return !auxiliaryBackdrop; }
- (void)windowFocusChanged:(NSNotification *)notification {
    (void)notification;
    carbon_native32_report_windows();
}
- (void)sendCarbonWindowEvent:(UInt32)kind {
    if (!carbonPeer) return;
    EventRef event = NULL;
    if (CreateEvent(NULL, kEventClassWindow, kind, GetCurrentEventTime(), 0, &event)) return;
    SetEventParameter(event, kEventParamDirectObject, typeWindowRef, sizeof(carbonPeer), &carbonPeer);
    void *(*target)(void *) = carbon_native32_symbol("GetWindowEventTarget");
    /* Guest activation handlers can pump events themselves. Deliver them
       from the Carbon queue after AppKit finishes its key-window change,
       never recursively from inside become/resignKeyWindow. */
    if (lp32_profile()->title == LP32_TITLE_TFU &&
        (kind == kEventWindowActivated || kind == kEventWindowDeactivated))
        PostEventToQueue(GetMainEventQueue(), event, kEventPriorityStandard);
    else if (target) SendEventToEventTarget(event, target(carbonPeer));
    ReleaseEvent(event);
}
- (void)applicationActivationChanged:(NSNotification *)notification {
    if (!hasPresented) return;
    BOOL active = [notification.name isEqualToString:NSApplicationDidBecomeActiveNotification];
    if (fullscreenPresentation && !auxiliaryBackdrop && !getenv("LP32_BACKGROUND_TEST"))
        [self setLevel:active && game_process_is_foreground() ? NSFloatingWindowLevel : NSNormalWindowLevel];
    if (active) restore_game_window();
    if (fullscreenPresentation && !getenv("LP32_BACKGROUND_TEST"))
        [NSApp setPresentationOptions:active ? NSApplicationPresentationAutoHideDock | NSApplicationPresentationAutoHideMenuBar : NSApplicationPresentationDefault];
    EventRef event = NULL;
    UInt32 kind = [notification.name isEqualToString:NSApplicationDidBecomeActiveNotification] ? kEventAppActivated : kEventAppDeactivated;
    if (!CreateEvent(NULL, kEventClassApplication, kind, GetCurrentEventTime(), 0, &event)) {
        if (lp32_profile()->title == LP32_TITLE_TFU)
            PostEventToQueue(GetMainEventQueue(), event, kEventPriorityStandard);
        else SendEventToEventTarget(event, GetApplicationEventTarget());
        ReleaseEvent(event);
    }
    carbon_native32_report_windows();
}
- (void)workspaceActivationChanged:(NSNotification *)notification {
    NSRunningApplication *application = notification.userInfo[NSWorkspaceApplicationKey];
    if (!hasPresented || auxiliaryBackdrop) return;
    if (getenv("LP32_TRACE_CARBON_EVENTS"))
        fprintf(stderr, "Carbon workspace activation selected=%d self=%d\n", application.processIdentifier, getpid());
    if (application.processIdentifier != getpid()) { carbon_native32_report_windows(); return; }
    /* Carbon can consume the activation event without updating NSApp.
       Workspace activation identifies the process selected by Cmd+Tab. */
    dispatch_async(dispatch_get_main_queue(), ^{
        if (game_process_is_foreground())
            restore_game_window();
    });
}
- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:self];
    [super dealloc];
}
- (void)becomeKeyWindow { [super becomeKeyWindow]; [self sendCarbonWindowEvent:kEventWindowActivated]; }
- (void)resignKeyWindow { [super resignKeyWindow]; [self sendCarbonWindowEvent:kEventWindowDeactivated]; }
@end
/* Carbon owns TFU's event pump, so AppKit does not always receive its usual
   activation/order-front sequence. Repair Cocoa activation only after the
   system has selected this process, then raise the logical game surface. */
static void restore_game_window(void) {
    static BOOL restoring;
    static BOOL reported_incomplete;
    if (getenv("LP32_BACKGROUND_TEST") || getenv("LP32_HEADLESS") || [NSApp modalWindow]) return;
    if (restoring || !game_process_is_foreground()) return;
    for (NSWindow *candidate in [NSApp windows]) {
        if (![candidate isKindOfClass:[LP32CarbonGameWindow class]]) continue;
        LP32CarbonGameWindow *window = (id)candidate;
        if (!window->hasPresented || !window->logicalVisible || window->auxiliaryBackdrop || [window isMiniaturized]) continue;
        restoring = YES;
        /* makeKeyAndOrderFront alone does not repair an inactive NSApp.
           Both calls can synchronously notify our activation observers. */
        if ([NSApp isHidden]) [NSApp unhideWithoutActivation];
        if (![NSApp isActive]) [NSApp activateIgnoringOtherApps:YES];
        /* The production Carbon preferences/fullscreen launch can leave
           normal-level Cocoa windows below other applications even when
           WindowServer has selected this process. Fullscreen presentation
           uses the floating level only while actually foreground. Both
           deactivation notifications and the Process Manager poll lower it
           again, including when Carbon consumes AppKit's activation event. */
        if (window->fullscreenPresentation && game_process_is_foreground())
            [window setLevel:NSFloatingWindowLevel];
        [window makeKeyAndOrderFront:nil];
        /* The Carbon activation path can leave Cocoa's key/visible flags
           set while WindowServer still orders another process above us.
           Normal orderFront then fails to raise the surface. The direct
           foreground guard above makes this unconditional ordering safe. */
        if (game_process_is_foreground()) [window orderFrontRegardless];
        [NSApp updateWindows];
        /* We have observed NSApp retaining this exact key/main window while
           both window flags remain false, even after makeKeyAndOrderFront.
           Allow activation to settle first, then remove the stale ordering
           entry through the public window API and present the same surface.
           Retain the window/view/context; never recreate the game drawable.
           Try only once per foreground visit so a failed recovery cannot
           produce an order-out/order-in loop during gameplay. */
        BOOL stale_identity = [NSApp isActive] &&
            (([NSApp keyWindow] == window && ![window isKeyWindow]) ||
             ([NSApp mainWindow] == window && ![window isMainWindow]) ||
             !window_server_visible(window));
        if (stale_identity) {
            NSTimeInterval now = GetCurrentEventTime();
            if (!window->focusMismatchSince) window->focusMismatchSince = now;
            if (!window->focusMismatchReordered && now - window->focusMismatchSince >= 0.25 &&
                game_process_is_foreground()) {
                window->focusMismatchReordered = YES;
                fprintf(stderr, "Carbon focus rebuilding stale window order window=%ld\n", (long)[window windowNumber]);
                [window orderOut:nil];
                if (game_process_is_foreground()) {
                    [window makeKeyAndOrderFront:nil];
                    [window makeMainWindow];
                    [window orderFrontRegardless];
                } else {
                    [window orderBack:nil];
                }
                [NSApp updateWindows];
            }
        } else {
            window->focusMismatchSince = 0;
        }
        if (window->fullscreenPresentation)
            [NSApp setPresentationOptions:NSApplicationPresentationAutoHideDock | NSApplicationPresentationAutoHideMenuBar];
        if (getenv("LP32_TRACE_CARBON_EVENTS"))
            fprintf(stderr, "Carbon restored game window key=%d main=%d active=%d\n",
                [window isKeyWindow], [window isMainWindow], [NSApp isActive]);
        BOOL incomplete = [NSApp isActive] && game_process_is_foreground() &&
            (![window isKeyWindow] || ![window isMainWindow]);
        if (incomplete && !reported_incomplete)
            fprintf(stderr, "Carbon focus restore incomplete window=%ld app-key=%ld app-main=%ld key=%d main=%d\n",
                (long)[window windowNumber], (long)[[NSApp keyWindow] windowNumber],
                (long)[[NSApp mainWindow] windowNumber], [window isKeyWindow], [window isMainWindow]);
        else if (reported_incomplete && [NSApp isActive] && [window isKeyWindow] && [window isMainWindow]) {
            fprintf(stderr, "Carbon focus restore recovered window=%ld\n", (long)[window windowNumber]);
            reported_incomplete = NO;
        }
        if (incomplete) reported_incomplete = YES;
        carbon_native32_report_windows();
        restoring = NO;
        return;
    }
}
static void reconcile_game_activation(void) {
    static NSTimeInterval last_check;
    static BOOL initialized, was_foreground;
    if (getenv("LP32_BACKGROUND_TEST") || getenv("LP32_HEADLESS")) return;
    NSTimeInterval now = GetCurrentEventTime();
    if (now - last_check < 0.1) return;
    last_check = now;
    BOOL foreground = game_process_is_foreground();
    BOOL returned = initialized && foreground && !was_foreground;
    if (!initialized || foreground != was_foreground) {
        fprintf(stderr, "Carbon focus foreground=%d workspace-foreground=%d cocoa-active=%d hidden=%d key=%d\n",
            foreground, [NSWorkspace sharedWorkspace].frontmostApplication.processIdentifier == getpid(),
            [NSApp isActive], [NSApp isHidden], [[NSApp keyWindow] isKindOfClass:[LP32CarbonGameWindow class]]);
        for (NSWindow *candidate in [NSApp windows]) {
            if (![candidate isKindOfClass:[LP32CarbonGameWindow class]]) continue;
            LP32CarbonGameWindow *window = (id)candidate;
            if (window->auxiliaryBackdrop) continue;
            fprintf(stderr, "Carbon focus surface time=%.3f window=%ld visible=%d logical=%d presented=%d minimized=%d space=%d key=%d\n",
                now, (long)[window windowNumber], [window isVisible], window->logicalVisible,
                window->hasPresented, [window isMiniaturized], [window isOnActiveSpace], [window isKeyWindow]);
        }
        carbon_native32_report_windows();
    }
    initialized = YES;
    was_foreground = foreground;
    if (!foreground) {
        for (NSWindow *candidate in [NSApp windows]) {
            if (![candidate isKindOfClass:[LP32CarbonGameWindow class]]) continue;
            LP32CarbonGameWindow *window = (id)candidate;
            if (window->fullscreenPresentation && !window->auxiliaryBackdrop &&
                [window level] != NSNormalWindowLevel) {
                [window setLevel:NSNormalWindowLevel];
                carbon_native32_report_windows();
            }
            window->focusMismatchSince = 0;
            window->focusMismatchReordered = NO;
        }
        return;
    }
    if ([NSApp modalWindow]) return;
    /* Native Carbon activation can consume the AppKit event and the main
       dispatch queue need not run before the next guest frame. Check the
       actual foreground process from the guest's event pump as well. */
    for (NSWindow *candidate in [NSApp windows]) {
        if (![candidate isKindOfClass:[LP32CarbonGameWindow class]]) continue;
        LP32CarbonGameWindow *window = (id)candidate;
        if (!window->hasPresented || !window->logicalVisible || window->auxiliaryBackdrop || [window isMiniaturized]) continue;
        if (returned || ![NSApp isActive] || [NSApp isHidden] || ![window isVisible] ||
            ![window isKeyWindow] || ![window isMainWindow] || ![window isOnActiveSpace] ||
            (window->fullscreenPresentation && [window level] != NSFloatingWindowLevel) ||
            !window_server_visible(window))
            restore_game_window();
        break;
    }
}
static OSStatus game_activation_event(EventHandlerCallRef next, EventRef event, void *data) {
    (void)next; (void)event; (void)data;
    restore_game_window();
    return eventNotHandledErr;
}
static void install_game_activation_handler(void) {
    static EventHandlerRef handler;
    if (handler || lp32_profile()->title != LP32_TITLE_TFU) return;
    EventTypeSpec spec = {kEventClassApplication, kEventAppActivated};
    InstallApplicationEventHandler(game_activation_event, 1, &spec, NULL, &handler);
}
void carbon_native32_pump_appkit_events(void) {
    static BOOL pumping;
    if (lp32_profile()->title != LP32_TITLE_TFU || pumping || ![NSThread isMainThread]) return;
    /* AppKit's activation and window-management events must reach NSApp
       even while the guest drives Carbon's ReceiveNextEvent loop. Leave
       keyboard, mouse and controller input in the Carbon queue. */
    pumping = YES;
    app_termination_pump();
    carbon_bridge32_service_keyboard_layout();
    for (unsigned i = 0; i < 32; ++i) {
        NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAppKitDefined
            untilDate:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES];
        if (!event) break;
        [NSApp sendEvent:event];
    }
    reconcile_game_activation();
    /* NSApplication.run normally does this after dispatching events. TFU
       owns the loop, so it must also commit AppKit's pending window work. */
    [NSApp updateWindows];
    pumping = NO;
}
static NSRect cocoa_rect(const Rect *r) {
    CGFloat desktopTop = NSMaxY([[[NSScreen screens] firstObject] frame]);
    return NSMakeRect(r->left, desktopTop-r->bottom, r->right-r->left, r->bottom-r->top);
}
static LP32CarbonGameWindow *game_window(uint32_t handle) {
    id window = objc_getAssociatedObject(object(handle), &cocoa_window_key);
    return [window isKindOfClass:[LP32CarbonGameWindow class]] ? window : nil;
}
void carbon_native32_display_mode(uint32_t display, int32_t width, int32_t height) {
    NSScreen *screen = nil;
    for (NSScreen *candidate in [NSScreen screens])
        if ([[candidate deviceDescription][@"NSScreenNumber"] unsignedIntValue] == display) screen = candidate;
    if (width > 0 && height > 0 && !screen) return;
    for (NSWindow *candidate in [NSApp windows]) {
        if (![candidate isKindOfClass:[LP32CarbonGameWindow class]]) continue;
        LP32CarbonGameWindow *window = (id)candidate;
        if (window->auxiliaryBackdrop) continue;
        if (width > 0 && height > 0) {
            if (!window->fullscreenPresentation) {
                window->windowedFrame = [window frame]; window->windowedStyle = [window styleMask];
            }
            window->fullscreenPresentation = YES; window->fullscreenDisplay = display;
            window->surfaceSize[0] = width; window->surfaceSize[1] = height;
            [window setStyleMask:NSWindowStyleMaskBorderless];
            [window setFrame:[screen frame] display:YES];
            if ([NSApp isActive] && !getenv("LP32_BACKGROUND_TEST"))
                [NSApp setPresentationOptions:NSApplicationPresentationAutoHideDock | NSApplicationPresentationAutoHideMenuBar];
        } else if (window->fullscreenPresentation && (!display || window->fullscreenDisplay == display)) {
            window->fullscreenPresentation = NO;
            [window setLevel:NSNormalWindowLevel];
            [window setStyleMask:window->windowedStyle]; [window setFrame:window->windowedFrame display:YES];
            if (!getenv("LP32_BACKGROUND_TEST")) [NSApp setPresentationOptions:NSApplicationPresentationDefault];
        }
    }
}
bool carbon_native32_surface_size(void *value, int32_t *size) {
    LP32CarbonGameWindow *window = value;
    if (![window isKindOfClass:[LP32CarbonGameWindow class]] || !window->fullscreenPresentation) return false;
    memcpy(size, window->surfaceSize, sizeof(window->surfaceSize)); return true;
}
void carbon_native32_report_windows(void) {
    const char *path = getenv("LP32_CARBON_WINDOW_REPORT");
    if (!path || !path[0]) return;
    NSMutableArray *report = [NSMutableArray array];
    for (NSWindow *candidate in [NSApp windows]) {
        if (![candidate isKindOfClass:[LP32CarbonGameWindow class]]) continue;
        LP32CarbonGameWindow *window = (id)candidate;
        NSRect frame = [window frame];
        [report addObject:@{@"backdrop": @(window->auxiliaryBackdrop), @"visible": @([window isVisible]),
            @"windowServerVisible": @(window_server_visible(window)),
            @"windowNumber": @([window windowNumber]), @"logicalVisible": @(window->logicalVisible),
            @"presented": @(window->hasPresented), @"minimized": @([window isMiniaturized]),
            @"key": @([window isKeyWindow]), @"main": @([window isMainWindow]), @"active": @([NSApp isActive]),
            @"appKeyWindow": @([[NSApp keyWindow] windowNumber]),
            @"appMainWindow": @([[NSApp mainWindow] windowNumber]),
            @"canBecomeKey": @([window canBecomeKeyWindow]),
            @"canBecomeMain": @([window canBecomeMainWindow]),
            @"hidesOnDeactivate": @([window hidesOnDeactivate]),
            @"level": @([window level]),
            @"onActiveSpace": @([window isOnActiveSpace]), @"collectionBehavior": @([window collectionBehavior]),
            @"foreground": @(game_process_is_foreground()),
            @"workspaceForeground": @([NSWorkspace sharedWorkspace].frontmostApplication.processIdentifier == getpid()),
            @"fullscreen": @(window->fullscreenPresentation), @"style": @([window styleMask]),
            @"display": @([[window screen].deviceDescription[@"NSScreenNumber"] unsignedIntValue]),
            @"frame": @[@(frame.origin.x), @(frame.origin.y), @(frame.size.width), @(frame.size.height)],
            @"surface": @[@(window->surfaceSize[0]), @(window->surfaceSize[1])]}];
    }
    NSData *data = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:NULL];
    [data writeToFile:[NSString stringWithUTF8String:path] atomically:YES];
    const char *history = getenv("LP32_CARBON_FOCUS_HISTORY");
    if (history && *history) {
        NSData *entry = [NSJSONSerialization dataWithJSONObject:@{@"time": @(GetCurrentEventTime()), @"windows": report}
            options:0 error:NULL];
        FILE *file = fopen(history, "a");
        if (file) { fwrite(entry.bytes, 1, entry.length, file); fputc('\n', file); fclose(file); }
    }
}
void *carbon_native32_cocoa_window(uint32_t h) {
    id value = object(h);
    if ([value isKindOfClass:[NSWindow class]]) return value;
    if ([value isKindOfClass:[NSView class]]) return [value window];
    void *pointer = carbon_native32_pointer(h);
    if (!pointer) return nil;
    NSWindow *window = objc_getAssociatedObject(value, &cocoa_window_key);
    if (!window) {
        SEL selector = NSSelectorFromString(@"initWithWindowRef:");
        if (![NSWindow instancesRespondToSelector:selector]) return nil;
        window = ((id(*)(id, SEL, void *))objc_msgSend)([NSWindow alloc], selector, pointer);
        [window setReleasedWhenClosed:NO];
        objc_setAssociatedObject(value, &cocoa_window_key, window, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [window release];
    }
    return window;
}

static bool pointer_event_type(uint32_t type) {
    switch (type) {
        case 'wind': case 'cntl': case 'ctrl': case 'cntx': case 'menu': case 'hiob': case 'etrg': case 'evnt': return true;
        default: return false;
    }
}
static bool cf_event_type(uint32_t type) { return type == 'cfst' || type == 'cfur' || type == 'cfob' || type == 'cfdc'; }
int32_t carbon_native32_set_event_parameter(void *event, uint32_t name, uint32_t type, const void *data, uint32_t size) {
    OSStatus (*set)(void *, uint32_t, uint32_t, ByteCount, const void *) = carbon_native32_symbol("SetEventParameter");
    if (!set) return unimpErr;
    if (size == 4 && (pointer_event_type(type) || cf_event_type(type) || type == 'void')) {
        uint32_t value; memcpy(&value, data, 4);
        void *pointer = cf_event_type(type) ? object(value) : pointer_event_type(type) ? carbon_native32_pointer(value) : (void *)(uintptr_t)value;
        return set(event, name, type, sizeof(pointer), &pointer);
    }
    if (size == 8 && (type == 'hipt' || type == 'cgpt' || type == 'hisz')) {
        const float *values = data; CGPoint point = CGPointMake(values[0], values[1]);
        return set(event, name, type, sizeof(point), &point);
    }
    if (size == 16 && (type == 'hirc' || type == 'cgrc')) {
        const float *values = data; CGRect rect = CGRectMake(values[0], values[1], values[2], values[3]);
        return set(event, name, type, sizeof(rect), &rect);
    }
    return set(event, name, type, size, data);
}
int32_t carbon_native32_event_parameter(void *event, const uint32_t *a) {
    OSStatus (*get)(void *, uint32_t, uint32_t, uint32_t *, ByteCount, ByteCount *, void *) = carbon_native32_symbol("GetEventParameter");
    if (!get) return unimpErr;
    uint32_t type = 0; ByteCount native_size = 0;
    OSStatus status = get(event, a[1], a[2], &type, 0, &native_size, NULL);
    if (getenv("LP32_TRACE_CARBON_EVENTS") && a[2] == 'hcmd')
        fprintf(stderr, "Carbon command parameter query status=%d type=%08x native-size=%lu guest-size=%u\n",
            (int)status, type, (unsigned long)native_size, a[4]);
    if (status) return status;
    uint8_t converted[64]; size_t size = native_size;
    if (pointer_event_type(type) || cf_event_type(type) || type == 'void') {
        void *pointer = NULL;
        status = get(event, a[1], type, NULL, sizeof(pointer), NULL, &pointer);
        uint32_t handle = cf_event_type(type) ? objc_bridge32_guest_object(pointer) :
            type == 'void' && (uintptr_t)pointer <= UINT32_MAX ? (uint32_t)(uintptr_t)pointer : carbon_native32_handle(pointer);
        memcpy(converted, &handle, 4); size = 4;
    } else if (type == 'hcmd') {
        HICommandExtended command = {0};
        status = get(event, a[1], type, NULL, sizeof(command), NULL, &command);
        if (getenv("LP32_TRACE_CARBON_EVENTS"))
            fprintf(stderr, "Carbon command parameter read status=%d command=%08x host-size=%zu\n",
                (int)status, (unsigned)command.commandID, sizeof(command));
        memset(converted, 0, sizeof(converted));
        memcpy(converted, &command.attributes, 4); memcpy(converted + 4, &command.commandID, 4);
        uint32_t handle = carbon_native32_handle(command.source.menu.menuRef);
        memcpy(converted + 8, &handle, 4); memcpy(converted + 12, &command.source.menu.menuItemIndex, 2); size = 14;
    } else if (type == 'hipt' || type == 'cgpt' || type == 'hisz') {
        CGPoint point = {0}; status = get(event, a[1], type, NULL, sizeof(point), NULL, &point);
        float values[] = {point.x, point.y}; memcpy(converted, values, sizeof(values)); size = sizeof(values);
    } else if (type == 'hirc' || type == 'cgrc') {
        CGRect rect = {0}; status = get(event, a[1], type, NULL, sizeof(rect), NULL, &rect);
        float values[] = {rect.origin.x, rect.origin.y, rect.size.width, rect.size.height}; memcpy(converted, values, sizeof(values)); size = sizeof(values);
    } else {
        if (a[3]) *(uint32_t *)(uintptr_t)a[3] = type;
        status = get(event, a[1], a[2], (void *)(uintptr_t)a[3], a[4], &native_size, (void *)(uintptr_t)a[6]);
        if (a[5]) *(uint32_t *)(uintptr_t)a[5] = (uint32_t)native_size;
        return status;
    }
    if (a[3]) *(uint32_t *)(uintptr_t)a[3] = type;
    if (a[5]) *(uint32_t *)(uintptr_t)a[5] = (uint32_t)size;
    if (status) return status;
    if (a[6] && a[4] < size) return eventParameterNotFoundErr;
    if (a[6]) memcpy((void *)(uintptr_t)a[6], converted, size);
    return noErr;
}

static void capture_window(void *window) {
    const char *capture = getenv("LP32_CARBON_CAPTURE_WINDOW");
    if (capture && capture[0]) {
        void *(*root)(void *) = carbon_native32_symbol("HIViewGetRoot");
        OSStatus (*render)(void *, OptionBits, CGRect *, CGImageRef *) = carbon_native32_symbol("HIViewCreateOffscreenImage");
        CGImageRef image = NULL;
        OSStatus status = render(root(window), 0, NULL, &image);
        if (!status && image) {
            NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:capture]];
            CGImageDestinationRef output = CGImageDestinationCreateWithURL((CFURLRef)url, CFSTR("public.png"), 1, NULL);
            if (output) { CGImageDestinationAddImage(output, image, NULL); CGImageDestinationFinalize(output); CFRelease(output); }
            CGImageRelease(image);
        }
        fprintf(stderr, "compat32: Carbon dialog render status=%d path=%s\n", status, capture);
    }
}

int carbon_native32_dispatch(const char *name, const uint32_t *a, uint64_t *result) {
#define IS(s) (!strcmp(name, "_" s))
#define PTR(i) ((void *)(uintptr_t)a[i])
#define REF(i) carbon_native32_pointer(a[i])
#define OBJ(i) object(a[i])
#define RETURN(v) do { *result = (uint64_t)(v); return 1; } while (0)
#define FN(ret, sig) ((ret (*)sig)function)
    if (carbon_text32_dispatch(name, a, result)) return 1;
    if (carbon_dialog32_dispatch(name, a, result)) return 1;
    if (getenv("LP32_TRACE_CARBON_NATIVE")) fprintf(stderr, "carbon-native: %s %08x %08x %08x %08x\n", name, a[0], a[1], a[2], a[3]);
    void *function = name[0] == '_' ? carbon_native32_symbol(name + 1) : NULL;
    if (IS("CGContextRelease")) { CGContextRelease(REF(0)); RETURN(0); }
    if (IS("SetWRefCon") && !function) {
        objc_setAssociatedObject(OBJ(0), &window_refcon_key, @(a[1]), OBJC_ASSOCIATION_RETAIN_NONATOMIC); RETURN(0);
    }
    if (IS("GetWRefCon") && (objc_getAssociatedObject(OBJ(0), &window_refcon_key) || !function))
        RETURN([objc_getAssociatedObject(OBJ(0), &window_refcon_key) unsignedIntValue]);
    if (IS("GetControlPopupMenuHandle") && !function) {
        void *menu = NULL;
        OSStatus (*get)(void *, int16_t, uint32_t, Size, void *, Size *) = carbon_native32_symbol("GetControlData");
        OSStatus status = get(REF(0), 0, 'mhan', sizeof(menu), &menu, NULL);
        RETURN(status ? 0 : carbon_native32_handle(menu));
    }
    if (IS("GetControlID") && !function) {
        OSStatus (*get)(void *, void *) = carbon_native32_symbol("HIViewGetID");
        if (!get) return 0;
        RETURN((uint32_t)get(REF(0), PTR(1)));
    }
    if (IS("SetControlMaximum") && !function) {
        void (*set)(void *, int32_t) = carbon_native32_symbol("HIViewSetMaximum");
        if (!set) return 0;
        set(REF(0), (int16_t)a[1]); RETURN(0);
    }
    if (IS("Draw1Control") && !function) {
        OSStatus (*dirty)(void *, Boolean) = carbon_native32_symbol("HIViewSetNeedsDisplay");
        if (!dirty) return 0;
        dirty(REF(0), true); RETURN(0);
    }
    if (IS("DisposeControl") && !function) {
        void *control = REF(0);
        if (control) {
            OSStatus (*remove)(void *) = carbon_native32_symbol("HIViewRemoveFromSuperview");
            CFRetain(control);
            if (remove) remove(control);
            CFRelease(control);
        }
        RETURN(0);
    }
    if ((IS("CFRetain") || IS("CFRelease")) && REF(0) && objc_getAssociatedObject(OBJ(0), &owned_cf_key)) {
        unsigned count = [objc_getAssociatedObject(OBJ(0), &owned_cf_key) unsignedIntValue];
        if (IS("CFRetain")) { CFRetain(REF(0)); ++count; }
        else if (count) {
            objc_setAssociatedObject(OBJ(0), &owned_cf_key, @(count - 1), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            CFRelease(REF(0)); RETURN(0);
        }
        objc_setAssociatedObject(OBJ(0), &owned_cf_key, @(count), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        RETURN(IS("CFRetain") ? a[0] : 0);
    }
    if (IS("CreateNibReferenceWithCFBundle")) {
        if (!function || !a[2]) return 0;
        NSBundle *bundle = OBJ(0);
        CFBundleRef cf_bundle = CFBundleCreate(NULL, (CFURLRef)[NSURL fileURLWithPath:[bundle bundlePath]]);
        void *nib = NULL;
        OSStatus status = FN(OSStatus, (CFBundleRef, CFStringRef, void **))(cf_bundle, (CFStringRef)OBJ(1), &nib);
        if (cf_bundle) CFRelease(cf_bundle);
        *(uint32_t *)PTR(2) = carbon_native32_handle(nib); RETURN((uint32_t)status);
    }
    if (IS("CreateWindowFromNib")) {
        if (!function || !REF(0) || !a[2]) return 0;
        void *window = NULL;
        OSStatus status = FN(OSStatus, (void *, CFStringRef, void **))(REF(0), (CFStringRef)OBJ(1), &window);
        *(uint32_t *)PTR(2) = carbon_native32_handle(window); RETURN((uint32_t)status);
    }
    if (IS("CreateNewWindow")) {
        if (!function || !a[3]) return 0;
        void *window = NULL;
        // LP64 Carbon windows require compositing. Legacy drawing is routed
        // through their Cocoa content view by the port/AGL adapters.
        uint32_t attributes = a[1] | kWindowCompositingAttribute;
        OSStatus status = FN(OSStatus, (uint32_t, uint32_t, const Rect *, void **))(a[0], attributes, PTR(2), &window);
        if (getenv("LP32_TRACE_CARBON_NATIVE") || status) fprintf(stderr, "carbon-native: CreateNewWindow class=%u attributes=%08x status=%d\n", a[0], attributes, (int)status);
        uint32_t handle = carbon_native32_handle(window);
        if (!status) {
            // NSCarbonWindow cannot create a layer context on current macOS.
            // Keep the native Carbon peer hidden for event targets/properties,
            // and present its game surface in an ordinary Cocoa window.
            NSUInteger style = a[0] == 13 ? NSWindowStyleMaskBorderless : NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
            LP32CarbonGameWindow *cocoa = [[LP32CarbonGameWindow alloc] initWithContentRect:cocoa_rect(PTR(2)) styleMask:style backing:NSBackingStoreBuffered defer:NO];
            cocoa->carbonPeer = window;
            cocoa->auxiliaryBackdrop = lp32_profile()->title == LP32_TITLE_TFU && a[0] == 13 && a[1] == 0x80000000;
            if (lp32_profile()->title == LP32_TITLE_TFU && !cocoa->auxiliaryBackdrop) {
                [cocoa setCollectionBehavior:NSWindowCollectionBehaviorMoveToActiveSpace];
                [[[NSWorkspace sharedWorkspace] notificationCenter] addObserver:cocoa
                    selector:@selector(workspaceActivationChanged:)
                    name:NSWorkspaceDidActivateApplicationNotification object:nil];
            }
            [[NSNotificationCenter defaultCenter] addObserver:cocoa selector:@selector(applicationActivationChanged:) name:NSApplicationDidBecomeActiveNotification object:NSApp];
            [[NSNotificationCenter defaultCenter] addObserver:cocoa selector:@selector(applicationActivationChanged:) name:NSApplicationDidResignActiveNotification object:NSApp];
            for (NSString *notification in @[NSWindowDidBecomeKeyNotification, NSWindowDidResignKeyNotification,
                    NSWindowDidBecomeMainNotification, NSWindowDidResignMainNotification])
                [[NSNotificationCenter defaultCenter] addObserver:cocoa selector:@selector(windowFocusChanged:)
                    name:notification object:cocoa];
            [cocoa setReleasedWhenClosed:NO];
            install_game_activation_handler();
            objc_setAssociatedObject(object(handle), &cocoa_window_key, cocoa, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            [cocoa release];
        }
        *(uint32_t *)PTR(3) = handle; RETURN((uint32_t)status);
    }
    if (IS("GetWindowPort")) {
        NSWindow *window = carbon_native32_cocoa_window(a[0]);
        RETURN(objc_bridge32_guest_object([window contentView]));
    }
    if (IS("GetWindowFromPort")) {
        id port = OBJ(0); NSWindow *window = [port isKindOfClass:[NSView class]] ? [port window] : nil;
        if ([window isKindOfClass:[LP32CarbonGameWindow class]]) RETURN(carbon_native32_handle(((LP32CarbonGameWindow *)window)->carbonPeer));
        SEL selector = NSSelectorFromString(@"windowRef");
        void *pointer = [window respondsToSelector:selector] ? ((void *(*)(id,SEL))objc_msgSend)(window, selector) : NULL;
        RETURN(carbon_native32_handle(pointer));
    }
    if (!function && IS("SetWindowProxyCreatorAndType")) {
        NSWindow *window = carbon_native32_cocoa_window(a[0]);
        if (!window) RETURN((uint32_t)paramErr);
        NSImage *icon = [[NSWorkspace sharedWorkspace] iconForFileType:NSFileTypeForHFSTypeCode(a[2])];
        [[window standardWindowButton:NSWindowDocumentIconButton] setImage:icon];
        RETURN(0);
    }
    if (!function && IS("SetWindowContentColor")) {
        NSWindow *window = carbon_native32_cocoa_window(a[0]); const uint16_t *rgb = PTR(1);
        if (!window || !rgb) RETURN((uint32_t)paramErr);
        [window setBackgroundColor:[NSColor colorWithSRGBRed:rgb[0] / 65535.0 green:rgb[1] / 65535.0 blue:rgb[2] / 65535.0 alpha:1]];
        RETURN(0);
    }
    if (!function) return 0;
    if ((IS("SetMenuBarFromNib") || IS("CreateMenuFromNib")) && REF(0)) {
        if (IS("SetMenuBarFromNib")) RETURN((uint32_t)FN(OSStatus, (void *, CFStringRef))(REF(0), (CFStringRef)OBJ(1)));
        void *menu = NULL; OSStatus status = FN(OSStatus, (void *, CFStringRef, void **))(REF(0), (CFStringRef)OBJ(1), &menu);
        if (a[2]) *(uint32_t *)PTR(2) = carbon_native32_handle(menu); RETURN((uint32_t)status);
    }
    if (IS("DisposeNibReference") && REF(0)) { FN(void, (void *))(REF(0)); RETURN(0); }
    if (IS("GetWindowEventTarget") || IS("GetControlEventTarget") || IS("HIViewGetRoot") || IS("GetControlOwner") || IS("GetControlPopupMenuHandle")) {
        if (!REF(0)) return 0;
        RETURN(carbon_native32_handle(FN(void *, (void *))(REF(0))));
    }
    if (IS("FrontWindow")) {
        NSWindow *key = [NSApp keyWindow];
        if ([key isKindOfClass:[LP32CarbonGameWindow class]]) RETURN(carbon_native32_handle(((LP32CarbonGameWindow *)key)->carbonPeer));
        RETURN(carbon_native32_handle(FN(void *, (void))( )));
    }
    if (IS("GetNextWindow")) RETURN(carbon_native32_handle(FN(void *, (void *))(REF(0))));
    if (IS("GetWindowGroupOfClass")) RETURN(carbon_native32_handle(FN(void *, (uint32_t))(a[0])));
    if (IS("GetWindowGroup") || IS("GetWindowGroupParent"))
        RETURN(carbon_native32_handle(FN(void *, (void *))(REF(0))));
    if (IS("CreateWindowGroup") || IS("HIScrollViewCreate")) {
        void *value = NULL; OSStatus status = FN(OSStatus, (uint32_t, void **))(a[0], &value);
        if (a[1]) *(uint32_t *)PTR(1) = carbon_native32_handle(value); RETURN((uint32_t)status);
    }
    if (IS("HIViewFindByID")) {
        void *view = NULL; uint64_t id; memcpy(&id, a + 1, 8);
        OSStatus status = FN(OSStatus, (void *, uint64_t, void **))(REF(0), id, &view);
        if (a[3]) *(uint32_t *)PTR(3) = carbon_native32_handle(view); RETURN((uint32_t)status);
    }
    if (IS("GetControlByID")) {
        void *control = NULL; OSStatus status = FN(OSStatus, (void *, const void *, void **))(REF(0), PTR(1), &control);
        if (a[2]) *(uint32_t *)PTR(2) = carbon_native32_handle(control); RETURN((uint32_t)status);
    }
    if (IS("GetWindowBounds") || IS("SetWindowBounds")) {
        OSStatus status = FN(OSStatus, (void *, uint32_t, void *))(REF(0), a[1], PTR(2));
        LP32CarbonGameWindow *window = game_window(a[0]);
        if (!status && window && !window->fullscreenPresentation && IS("SetWindowBounds")) {
            NSRect rect = cocoa_rect(PTR(2));
            if (a[1] == 33) rect = [window frameRectForContentRect:rect];
            [window setFrame:rect display:NO];
        }
        RETURN((uint32_t)status);
    }
    if (IS("SetWindowTitleWithCFString") || IS("SetControlTitleWithCFString")) {
        if (IS("SetWindowTitleWithCFString")) [game_window(a[0]) setTitle:OBJ(1) ?: @""];
        RETURN((uint32_t)FN(OSStatus, (void *, CFStringRef))(REF(0), (CFStringRef)OBJ(1)));
    }
    if (IS("CopyWindowTitleAsCFString")) {
        CFStringRef title = NULL; OSStatus status = FN(OSStatus, (void *, CFStringRef *))(REF(0), &title);
        if (a[1]) *(uint32_t *)PTR(1) = objc_bridge32_owned_object((id)title);
        if (title) CFRelease(title); RETURN((uint32_t)status);
    }
    if (IS("ShowWindow") || IS("HideWindow") || IS("SelectWindow") || IS("DisposeWindow") || IS("DisposeControl") || IS("Draw1Control")) {
        if (IS("SelectWindow") && getenv("LP32_BACKGROUND_TEST")) RETURN(0);
        LP32CarbonGameWindow *window = game_window(a[0]);
        if (window) {
            if (IS("ShowWindow")) {
                window->logicalVisible = YES;
                if (window->auxiliaryBackdrop) {
                    [window sendCarbonWindowEvent:kEventWindowShown]; RETURN(0);
                }
                if (getenv("LP32_BACKGROUND_TEST")) [window orderBack:nil]; else [window orderFront:nil];
                if (!window->hasPresented && !getenv("LP32_HEADLESS")) {
                    window->hasPresented = YES;
                    if (getenv("LP32_BACKGROUND_TEST")) {
                        // Exercise rendering without taking keyboard or cursor focus.
                        [window applicationActivationChanged:[NSNotification notificationWithName:NSApplicationDidBecomeActiveNotification object:NSApp]];
                    } else {
                    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
                    carbon_native32_finish_appkit_launch();
                    [NSApp activateIgnoringOtherApps:YES];
                    [window makeKeyWindow];
                    if ([NSApp isActive]) [window applicationActivationChanged:
                        [NSNotification notificationWithName:NSApplicationDidBecomeActiveNotification object:NSApp]];
                    if (getenv("LP32_TRACE_CARBON_EVENTS")) fprintf(stderr, "Carbon presentation active=%d key=%d\n", [NSApp isActive], [window isKeyWindow]);
                    }
                }
                [window sendCarbonWindowEvent:kEventWindowShown];
                RETURN(0);
            }
            if (IS("SelectWindow")) { if (!window->auxiliaryBackdrop && !getenv("LP32_BACKGROUND_TEST")) [window makeKeyAndOrderFront:nil]; RETURN(0); }
            if (IS("HideWindow")) { window->logicalVisible = NO; [window orderOut:nil]; RETURN(0); }
            if (IS("DisposeWindow")) {
                [window close];
                objc_setAssociatedObject(object(a[0]), &cocoa_window_key, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            }
        }
        if (IS("DisposeWindow")) carbon_text32_remove_window(REF(0));
        if (IS("ShowWindow")) carbon_text32_layout();
        FN(void, (void *))(REF(0));
        if (IS("ShowWindow") && getenv("LP32_BACKGROUND_TEST")) {
            void (*behind)(void *, void *) = carbon_native32_symbol("SendBehind");
            if (behind) behind(REF(0), NULL);
        }
        if (IS("ShowWindow")) capture_window(REF(0));
        RETURN(0);
    }
    if (IS("IsWindowVisible")) { if (game_window(a[0])) RETURN(game_window(a[0])->logicalVisible); RETURN(FN(Boolean, (void *))(REF(0))); }
    if (IS("GetControlValue")) RETURN((uint32_t)(int32_t)FN(int16_t, (void *))(REF(0)));
    if (IS("SetControlValue") || IS("SetControlMaximum")) { FN(void, (void *, int16_t))(REF(0), a[1]); RETURN(0); }
    if (IS("DisableControl") || IS("EnableControl")) RETURN((uint32_t)FN(OSStatus, (void *))(REF(0)));
    if (IS("GetControlBounds")) { FN(void *, (void *, void *))(REF(0), PTR(1)); RETURN(a[1]); }
    if (IS("GetControlID") || IS("SetControlID") || IS("SetWindowContentColor")) RETURN((uint32_t)FN(OSStatus, (void *, void *))(REF(0), PTR(1)));
    if (IS("SetWRefCon")) { FN(void, (void *, intptr_t))(REF(0), (int32_t)a[1]); RETURN(0); }
    if (IS("GetWRefCon")) RETURN((uint32_t)FN(intptr_t, (void *))(REF(0)));
    if (IS("SetWindowModified") || IS("HIViewSetVisible") || IS("HIViewSetNeedsDisplay")) RETURN((uint32_t)FN(OSStatus, (void *, Boolean))(REF(0), a[1]));
    if (IS("SetWindowGroupLevel")) RETURN((uint32_t)FN(OSStatus, (void *, int32_t))(REF(0), a[1]));
    if (IS("SetWindowGroup") || IS("HIViewAddSubview")) {
        OSStatus status = FN(OSStatus, (void *, void *))(REF(0), REF(1));
        if (IS("HIViewAddSubview")) carbon_text32_layout();
        RETURN((uint32_t)status);
    }
    if (IS("SendBehind")) {
        LP32CarbonGameWindow *window = game_window(a[0]), *other = game_window(a[1]);
        if (window && !window->auxiliaryBackdrop) [window orderWindow:NSWindowBelow relativeTo:other ? [other windowNumber] : 0];
        FN(void, (void *, void *))(REF(0), REF(1)); RETURN(0);
    }
    if (IS("RepositionWindow")) RETURN((uint32_t)FN(OSStatus, (void *, void *, uint32_t))(REF(0), REF(1), a[2]));
    if (IS("SetWindowProxyCreatorAndType")) RETURN((uint32_t)FN(OSStatus, (void *, uint32_t, uint32_t, int16_t))(REF(0), a[1], a[2], a[3]));
    if (IS("SetWindowProperty")) RETURN((uint32_t)FN(OSStatus, (void *, uint32_t, uint32_t, ByteCount, const void *))(REF(0), a[1], a[2], a[3], PTR(4)));
    if (IS("GetWindowProperty")) {
        ByteCount actual = 0;
        OSStatus status = FN(OSStatus, (void *, uint32_t, uint32_t, ByteCount, ByteCount *, void *))(REF(0), a[1], a[2], a[3], &actual, PTR(5));
        if (a[4]) *(uint32_t *)PTR(4) = (uint32_t)actual;
        RETURN((uint32_t)status);
    }
    if (IS("GetControlData")) {
        Size actual = 0;
        if (a[2] == 'draw' || a[2] == 'hitt' || a[2] == 'trak') {
            void *key = a[2] == 'draw' ? &pane_draw_key : a[2] == 'hitt' ? &pane_hit_key : &pane_track_key;
            if (a[5]) *(int32_t *)PTR(5) = 4;
            if (a[3] < 4 || !a[4]) RETURN((uint32_t)paramErr);
            *(uint32_t *)PTR(4) = [objc_getAssociatedObject(OBJ(0), key) unsignedIntValue]; RETURN(0);
        }
        if (a[2] == 'cfst' || a[2] == 'pwcf') {
            CFStringRef string = NULL;
            OSStatus status = FN(OSStatus, (void *, int16_t, uint32_t, Size, void *, Size *))(REF(0), a[1], a[2], sizeof(string), &string, &actual);
            if (a[5]) *(int32_t *)PTR(5) = 4;
            if (!status && a[4] && a[3] >= 4) *(uint32_t *)PTR(4) = objc_bridge32_owned_object((id)string);
            if (string) CFRelease(string);
            RETURN((uint32_t)status);
        }
        OSStatus status = FN(OSStatus, (void *, int16_t, uint32_t, Size, void *, Size *))(REF(0), a[1], a[2], (int32_t)a[3], PTR(4), &actual);
        if (a[5]) *(int32_t *)PTR(5) = (int32_t)actual;
        RETURN((uint32_t)status);
    }
    if (IS("SetControlData")) {
        if ((a[2] == 'draw' || a[2] == 'hitt' || a[2] == 'trak') && a[3] == 4 && a[4]) {
            uint32_t guest = *(uint32_t *)PTR(4);
            void *key = a[2] == 'draw' ? &pane_draw_key : a[2] == 'hitt' ? &pane_hit_key : &pane_track_key;
            objc_setAssociatedObject(OBJ(0), key, @(guest), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            if (a[2] == 'draw') {
                /* Carbon's old QuickDraw drawing callback has no usable port
                   in LP64. Draw through the HIView's Quartz event instead. */
                if (objc_getAssociatedObject(OBJ(0), &pane_draw_handler_key)) RETURN(0);
                void *(*target)(void *) = carbon_native32_symbol("GetControlEventTarget");
                EventTypeSpec type = {kEventClassControl, kEventControlDraw}; EventHandlerRef handler = NULL;
                OSStatus status = InstallEventHandler(target(REF(0)), pane_draw_event, 1, &type, REF(0), &handler);
                if (!status) objc_setAssociatedObject(OBJ(0), &pane_draw_handler_key, [NSValue valueWithPointer:handler], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
                RETURN((uint32_t)status);
            }
            void *callback = !guest ? NULL : a[2] == 'hitt' ? (void *)pane_hit : (void *)pane_track;
            RETURN((uint32_t)FN(OSStatus, (void *, int16_t, uint32_t, Size, const void *))(REF(0), a[1], a[2], sizeof(callback), &callback));
        }
        if ((a[2] == 'cfst' || a[2] == 'pwcf') && a[3] == 4 && a[4]) {
            CFStringRef string = (CFStringRef)object(*(uint32_t *)PTR(4));
            RETURN((uint32_t)FN(OSStatus, (void *, int16_t, uint32_t, Size, const void *))(REF(0), a[1], a[2], sizeof(string), &string));
        }
        RETURN((uint32_t)FN(OSStatus, (void *, int16_t, uint32_t, Size, const void *))(REF(0), a[1], a[2], (int32_t)a[3], PTR(4)));
    }
    if (IS("HIViewGetBounds") || IS("HIViewSetFrame")) {
        float *guest = PTR(1); CGRect rect = {0};
        if (IS("HIViewSetFrame")) rect = CGRectMake(guest[0], guest[1], guest[2], guest[3]);
        OSStatus status = FN(OSStatus, (void *, CGRect *))(REF(0), &rect);
        if (!status && IS("HIViewGetBounds")) { guest[0] = rect.origin.x; guest[1] = rect.origin.y; guest[2] = rect.size.width; guest[3] = rect.size.height; }
        if (!status && IS("HIViewSetFrame")) carbon_text32_layout();
        RETURN((uint32_t)status);
    }
    if (IS("HIViewSetBoundsOrigin")) {
        float x, y; memcpy(&x, a + 1, 4); memcpy(&y, a + 2, 4);
        RETURN((uint32_t)FN(OSStatus, (void *, CGFloat, CGFloat))(REF(0), x, y));
    }
    if (IS("RunApplicationEventLoop")) { FN(void, (void))( ); RETURN(0); }
    if (IS("QuitApplicationEventLoop")) { FN(void, (void))(); RETURN(0); }
    if (IS("GetCurrentEventLoop") || IS("GetMainEventLoop") || IS("GetCurrentEventQueue") || IS("GetMainEventQueue") || IS("GetEventDispatcherTarget"))
        RETURN(carbon_native32_handle(FN(void *, (void))()));
    if (IS("RunCurrentEventLoop")) {
        double timeout; memcpy(&timeout, a, 8); RETURN((uint32_t)FN(OSStatus, (double))(timeout));
    }
    if (IS("QuitEventLoop")) RETURN((uint32_t)FN(OSStatus, (void *))(REF(0)));
    if (IS("RunAppModalLoopForWindow") || IS("QuitAppModalLoopForWindow")) {
        if (getenv("LP32_BACKGROUND_TEST")) {
            if (IS("QuitAppModalLoopForWindow")) {
                objc_setAssociatedObject(OBJ(0), &background_modal_done_key, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            } else {
                objc_setAssociatedObject(OBJ(0), &background_modal_done_key, @NO, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
                OSStatus (*run)(double) = carbon_native32_symbol("RunCurrentEventLoop");
                /* App-internal dialog regression tests; never inject input
                   during a normal launch or into another application. */
                static unsigned test_command_index;
                while (![objc_getAssociatedObject(OBJ(0), &background_modal_done_key) boolValue]) {
                    const char *commands = getenv("LP32_CARBON_TEST_COMMANDS");
                    if (commands && strlen(commands) >= (test_command_index + 1) * 4) {
                        const unsigned char *token = (const unsigned char *)commands + test_command_index++ * 4;
                        HICommand command = {0};
                        command.commandID = ((uint32_t)token[0] << 24) | ((uint32_t)token[1] << 16) | ((uint32_t)token[2] << 8) | token[3];
                        EventRef event = NULL;
                        if (!CreateEvent(NULL, kEventClassCommand, kEventCommandProcess, GetCurrentEventTime(), 0, &event)) {
                            SetEventParameter(event, kEventParamDirectObject, typeHICommand, sizeof(command), &command);
                            void *(*target)(void *) = carbon_native32_symbol("GetWindowEventTarget");
                            fprintf(stderr, "compat32: dialog test command %.4s\n", token);
                            SendEventToEventTarget(event, target(REF(0)));
                            ReleaseEvent(event);
                        }
                    }
                    if (![objc_getAssociatedObject(OBJ(0), &background_modal_done_key) boolValue]) run(0.1);
                }
            }
            RETURN(0);
        }
        RETURN((uint32_t)FN(OSStatus, (void *))(REF(0)));
    }
    if (IS("ShowSheetWindow")) {
        OSStatus status = FN(OSStatus, (void *, void *))(REF(0), REF(1));
        const char *point = getenv("LP32_CARBON_TEST_PANE_POINT");
        int x, y;
        if (!status && getenv("LP32_BACKGROUND_TEST") && point && sscanf(point, "%d,%d", &x, &y) == 2) {
            OSStatus (*get)(void *, const void *, void **) = carbon_native32_symbol("GetControlByID");
            ControlID id = {'MONI', 1}; void *control = NULL;
            if (!get(REF(0), &id, &control) && control) {
                Point position = {(int16_t)y, (int16_t)x};
                fprintf(stderr, "compat32: dialog test pane point %d,%d hit=%d\n", x, y, pane_hit(control, position));
                pane_track(control, position, NULL);
            }
        }
        if (!status) capture_window(REF(0)); RETURN((uint32_t)status);
    }
    if (IS("HideSheetWindow")) RETURN((uint32_t)FN(OSStatus, (void *))(REF(0)));
    /* Menus from native nibs and popup controls use native MenuRefs. */
    if (IS("GetIndMenuItemWithCommandID")) {
        void *menu = NULL; OSStatus status = FN(OSStatus, (void *, uint32_t, uint32_t, void **, uint16_t *))(REF(0), a[1], a[2], &menu, PTR(4));
        if (a[3]) *(uint32_t *)PTR(3) = carbon_native32_handle(menu); RETURN((uint32_t)status);
    }
    if (IS("DisableMenuCommand") || IS("EnableMenuCommand")) { FN(void, (void *, uint32_t))(REF(0), a[1]); RETURN(0); }
    if (IS("CountMenuItems") && REF(0)) RETURN(FN(uint16_t, (void *))(REF(0)));
    if (IS("SetMenuItemTextWithCFString") && REF(0)) RETURN((uint32_t)FN(OSStatus, (void *, uint16_t, CFStringRef))(REF(0), a[1], (CFStringRef)OBJ(2)));
    if (IS("CopyMenuItemTextAsCFString") && REF(0)) {
        CFStringRef text = NULL; OSStatus status = FN(OSStatus, (void *, uint16_t, CFStringRef *))(REF(0), a[1], &text);
        if (a[2]) *(uint32_t *)PTR(2) = objc_bridge32_owned_object((id)text);
        if (text) CFRelease(text); RETURN((uint32_t)status);
    }
    if ((IS("EnableMenuItem") || IS("DisableMenuItem")) && REF(0)) { FN(void, (void *, uint16_t))(REF(0), a[1]); RETURN(0); }
    if (IS("IsMenuItemEnabled") && REF(0)) RETURN(FN(Boolean, (void *, uint16_t))(REF(0), a[1]));
    if (IS("DeleteMenuItems") && REF(0)) RETURN((uint32_t)FN(OSStatus, (void *, uint16_t, uint16_t))(REF(0), a[1], a[2]));
    if (IS("InsertMenuItemTextWithCFString") && REF(0)) RETURN((uint32_t)FN(OSStatus, (void *, CFStringRef, uint16_t, uint32_t, uint32_t, uint16_t *))(REF(0), (CFStringRef)OBJ(1), a[2], a[3], a[4], PTR(5)));
    if (IS("SetMenuItemProperty") && REF(0)) RETURN((uint32_t)FN(OSStatus, (void *, uint16_t, uint32_t, uint32_t, ByteCount, const void *))(REF(0), a[1], a[2], a[3], a[4], PTR(5)));
    if (IS("GetMenuItemProperty") && REF(0)) {
        ByteCount actual = 0;
        OSStatus status = FN(OSStatus, (void *, uint16_t, uint32_t, uint32_t, ByteCount, ByteCount *, void *))(REF(0), a[1], a[2], a[3], a[4], &actual, PTR(6));
        if (a[5]) *(uint32_t *)PTR(5) = (uint32_t)actual;
        RETURN((uint32_t)status);
    }
    return 0;
#undef IS
#undef PTR
#undef REF
#undef OBJ
#undef RETURN
#undef FN
}
