/* Keeps third-party GL test binaries (piglit's GLUT tests) from disturbing
 * the desktop: every window is ordered in fully transparent, click-through
 * and behind all others (GLUT only draws once its window is on screen), the
 * app never activates or takes focus, and it has no Dock icon.
 * Linked into every piglit and glcts executable (piglit.sh, glcts.sh), so
 * no environment variable is needed: a SIP-protected launcher such as
 * /bin/sh drops DYLD_INSERT_LIBRARIES, and then windows would show. */
#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#include <Carbon/Carbon.h>

static IMP original_order_back;

static IMP original_set_alpha;
static void hide(NSWindow *window)
{
    ((void (*)(id, SEL, CGFloat))original_set_alpha)(window, @selector(setAlphaValue:), 0);
    window.ignoresMouseEvents = YES;
    window.hasShadow = NO;
    window.collectionBehavior |= NSWindowCollectionBehaviorTransient | NSWindowCollectionBehaviorIgnoresCycle;
}

static void order_in_hidden(NSWindow *self, SEL _cmd, id sender)
{
    (void)_cmd;
    hide(self);
    ((void (*)(id, SEL, id))original_order_back)(self, @selector(orderBack:), sender);
}
static void order_in_hidden_noarg(NSWindow *self, SEL _cmd) { order_in_hidden(self, _cmd, nil); }
static IMP original_order_window;
static void order_window_hidden(NSWindow *self, SEL _cmd, NSWindowOrderingMode mode, NSInteger other)
{
    if (mode != NSWindowOut) {
        hide(self);
        mode = NSWindowBelow;
        other = 0;
    }
    ((void (*)(id, SEL, NSWindowOrderingMode, NSInteger))original_order_window)(self, _cmd, mode, other);
}
static IMP original_set_visible;
static void set_visible_hidden(NSWindow *self, SEL _cmd, BOOL visible)
{
    if (visible) hide(self);
    ((void (*)(id, SEL, BOOL))original_set_visible)(self, _cmd, visible);
}
static void set_alpha_zero(NSWindow *self, SEL _cmd, CGFloat alpha)
{
    (void)alpha;
    ((void (*)(id, SEL, CGFloat))original_set_alpha)(self, _cmd, 0);
}
static void ignore(id self, SEL _cmd) { (void)self; (void)_cmd; }
static void ignore_arg(id self, SEL _cmd, id arg) { (void)self; (void)_cmd; (void)arg; }
static void ignore_bool(id self, SEL _cmd, BOOL arg) { (void)self; (void)_cmd; (void)arg; }
static BOOL refuse_options(id self, SEL _cmd, NSUInteger options) { (void)self; (void)_cmd; (void)options; return NO; }
static IMP original_set_policy;
static BOOL accessory_policy(id self, SEL _cmd, NSApplicationActivationPolicy policy)
{
    (void)policy;
    return ((BOOL (*)(id, SEL, NSApplicationActivationPolicy))original_set_policy)(self, _cmd, NSApplicationActivationPolicyAccessory);
}

static void replace(Class cls, SEL selector, IMP imp)
{
    Method m = class_getInstanceMethod(cls, selector);
    if (m) method_setImplementation(m, imp);
}

__attribute__((constructor)) static void install(void)
{
    Class window = [NSWindow class];
    original_order_back = method_getImplementation(class_getInstanceMethod(window, @selector(orderBack:)));
    replace(window, @selector(orderFront:), (IMP)order_in_hidden);
    replace(window, @selector(makeKeyAndOrderFront:), (IMP)order_in_hidden);
    replace(window, @selector(orderFrontRegardless), (IMP)order_in_hidden_noarg);
    original_order_window = method_getImplementation(class_getInstanceMethod(window, @selector(orderWindow:relativeTo:)));
    replace(window, @selector(orderWindow:relativeTo:), (IMP)order_window_hidden);
    original_set_visible = method_getImplementation(class_getInstanceMethod(window, @selector(setIsVisible:)));
    replace(window, @selector(setIsVisible:), (IMP)set_visible_hidden);
    original_set_alpha = method_getImplementation(class_getInstanceMethod(window, @selector(setAlphaValue:)));
    replace(window, @selector(setAlphaValue:), (IMP)set_alpha_zero);
    replace(window, @selector(makeKeyWindow), (IMP)ignore);
    replace(window, @selector(makeMainWindow), (IMP)ignore);
    Class app = [NSApplication class];
    replace(app, @selector(activateIgnoringOtherApps:), (IMP)ignore_bool);
    replace(app, @selector(activate), (IMP)ignore);
    replace(app, @selector(unhide:), (IMP)ignore_arg);
    original_set_policy = method_getImplementation(class_getInstanceMethod(app, @selector(setActivationPolicy:)));
    replace(app, @selector(setActivationPolicy:), (IMP)accessory_policy);
    replace([NSRunningApplication class], @selector(activateWithOptions:), (IMP)refuse_options);
}

/* GLUT's process setup: stay a background process. */
static OSStatus transform_process_type(const ProcessSerialNumber *psn, ProcessApplicationTransformState state)
{
    (void)psn; (void)state;
    return noErr;
}
static OSErr set_front_process(const ProcessSerialNumber *psn) { (void)psn; return noErr; }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
__attribute__((used, section("__DATA,__interpose"))) static struct { const void *replacement, *original; } interposers[] = {
    {(const void *)transform_process_type, (const void *)TransformProcessType},
    {(const void *)set_front_process, (const void *)SetFrontProcess},
};
#pragma clang diagnostic pop
