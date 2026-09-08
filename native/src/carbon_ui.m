#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>
#import <objc/runtime.h>
#include "carbon_ui.h"
#include "objc_bridge.h"
#include "compat_runtime.h"
#include "carbon_native.h"
#include <string.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static id object(uint32_t value) { return (id)objc_bridge32_host_object(value); }
static uint32_t handle(id value) { return objc_bridge32_guest_object(value); }
static char handlers_key, menu_command_key, menu_properties_key;
static id application_target;
static NSMutableArray *event_queue;
static NSWindow *game_window;
static bool quit_loop;

@interface LP32CarbonEvent : NSObject {
@public uint32_t event_class, kind, references;
    void *native_event;
    double time;
    NSMutableDictionary *parameters;
}
@end
@implementation LP32CarbonEvent
- (instancetype)init { if ((self = [super init])) { parameters = [[NSMutableDictionary alloc] init]; references = 1; time = [[NSProcessInfo processInfo] systemUptime]; } return self; }
- (void)dealloc {
    if (native_event) ((void(*)(void *))carbon_native32_symbol("ReleaseEvent"))(native_event);
    [parameters release]; [super dealloc];
}
@end

@interface LP32CarbonTimer : NSObject {
@public uint32_t callback, user_data, guest_handle; EventLoopTimerRef native_timer;
}
@end
@implementation LP32CarbonTimer
@end
static void native_timer_callback(EventLoopTimerRef timer, void *opaque) {
    (void)timer;
    @autoreleasepool {
        LP32CarbonTimer *context = [(id)opaque retain];
        uint32_t args[] = {context->guest_handle, context->user_data};
        compat_runtime32_call(context->callback, args, 2);
        [context release];
        if (compat_runtime32_last_call_trapped()) { fflush(NULL); _Exit(EXIT_FAILURE); }
    }
}

@interface LP32CarbonHandler : NSObject {
@public uint32_t callback, user_data;
    void *native_handler;
    id target;
    NSMutableArray *types;
}
@end
@implementation LP32CarbonHandler
- (instancetype)init { if ((self = [super init])) types = [[NSMutableArray alloc] init]; return self; }
- (void)dealloc { [types release]; [super dealloc]; }
@end

static id app_target(void) {
    if (!application_target) {
        void *(*get)(void) = carbon_native32_symbol("GetApplicationEventTarget");
        application_target = get ? [object(carbon_native32_handle(get())) retain] : [[NSObject alloc] init];
    }
    return application_target;
}
static NSMutableArray *handlers(id target) {
    if (!target) target = app_target();
    NSMutableArray *list = objc_getAssociatedObject(target, &handlers_key);
    if (!list) { list = [NSMutableArray array]; objc_setAssociatedObject(target, &handlers_key, list, OBJC_ASSOCIATION_RETAIN_NONATOMIC); }
    return list;
}
static NSNumber *event_type(uint32_t event_class, uint32_t kind) {
    return [NSNumber numberWithUnsignedLongLong:((uint64_t)event_class << 32) | kind];
}
static void parameter(LP32CarbonEvent *event, uint32_t name, uint32_t type, const void *data, size_t size) {
    if (event && (data || !size)) [event->parameters setObject:@{ @"type": @(type), @"data": [NSData dataWithBytes:data length:size] } forKey:@(name)];
}

struct dispatch_context { uint32_t token; NSArray *list; NSUInteger next; LP32CarbonEvent *event; struct dispatch_context *previous; void *native_call; };
static _Thread_local struct dispatch_context *current_dispatch;
static int32_t send_next(struct dispatch_context *context) {
    NSNumber *type = event_type(context->event->event_class, context->event->kind);
    while (context->next < [context->list count]) {
        LP32CarbonHandler *handler = [context->list objectAtIndex:context->next++];
        if (!handler->callback || ![handler->types containsObject:type]) continue;
        uint32_t event_handle = objc_bridge32_owned_object(context->event);
        uint32_t args[] = {context->token, event_handle, handler->user_data};
        int32_t status = compat_runtime32_call(handler->callback, args, 3);
        uint64_t ignored;
        objc_bridge32_dispatch("_CFRelease", &event_handle, &ignored);
        if (compat_runtime32_last_call_trapped()) { fflush(NULL); _Exit(EXIT_FAILURE); }
        if (status != eventNotHandledErr) return status;
    }
    return eventNotHandledErr;
}
static int32_t send_event(LP32CarbonEvent *event, id target) {
    if (!event) return paramErr;
    NSMutableArray *list = [NSMutableArray arrayWithArray:handlers(target)];
    if (target != app_target()) [list addObjectsFromArray:handlers(app_target())];
    struct dispatch_context context = {compat_runtime32_allocate(4, 1), list, 0, event, current_dispatch, NULL};
    if (!context.token) return memFullErr;
    current_dispatch = &context;
    int32_t result = send_next(&context);
    current_dispatch = context.previous;
    compat_runtime32_deallocate(context.token);
    return result;
}

/* Test input stays inside this guest. Never post events to the desktop, and
   require background-test mode so an ordinary launch cannot enable it. */
static uint8_t test_keys[16];
int carbon_ui32_test_keys(uint8_t keys[16]) {
    const char *path = getenv("LP32_CARBON_TEST_KEYS_FILE");
    if (!getenv("LP32_BACKGROUND_TEST") || !path) return 0;
    if (keys) memcpy(keys, test_keys, 16);
    return 1;
}
static void poll_test_keys(void) {
    if (!carbon_ui32_test_keys(NULL)) return;
    static double last_poll;
    double now = [[NSProcessInfo processInfo] systemUptime];
    if (now - last_poll < .01) return;
    last_poll = now;
    uint8_t next[16] = {0};
    FILE *file = fopen(getenv("LP32_CARBON_TEST_KEYS_FILE"), "r");
    unsigned key;
    if (file) {
        while (fscanf(file, "%u", &key) == 1) if (key < 128) next[key/8] |= 1u << (key%8);
        fclose(file);
    }
    uint8_t previous[16]; memcpy(previous, test_keys, 16); memcpy(test_keys, next, 16);
    for (key = 0; key < 128; ++key) if ((previous[key/8] ^ next[key/8]) & (1u << (key%8))) {
        bool down = (next[key/8] & (1u << (key%8))) != 0;
        LP32CarbonEvent *event = [[[LP32CarbonEvent alloc] init] autorelease];
        event->event_class = kEventClassKeyboard;
        event->kind = down ? kEventRawKeyDown : kEventRawKeyUp;
        static const char characters[128] = {[0]='a',[1]='s',[2]='d',[13]='w',[36]='\r',[49]=' ',[53]=27};
        uint32_t modifiers = 0; char character = characters[key];
        parameter(event, kEventParamKeyCode, typeUInt32, &key, 4);
        parameter(event, kEventParamKeyMacCharCodes, typeChar, &character, 1);
        parameter(event, kEventParamKeyModifiers, typeUInt32, &modifiers, 4);
        send_event(event, app_target());
        fprintf(stderr, "compat32: guest test key %u %s\n", key, down ? "down" : "up");
    }
}

static OSStatus call_native_event(uint32_t callback, uint32_t user_data, void *call, void *native_event) {
    @autoreleasepool {
        if (!callback) return eventNotHandledErr;
        LP32CarbonEvent *event = [[[LP32CarbonEvent alloc] init] autorelease];
        event->event_class = ((uint32_t(*)(void *))carbon_native32_symbol("GetEventClass"))(native_event);
        event->kind = ((uint32_t(*)(void *))carbon_native32_symbol("GetEventKind"))(native_event);
        if (getenv("LP32_BACKGROUND_TEST") &&
            ((event->event_class == kEventClassApplication && event->kind == kEventAppDeactivated) ||
             event->event_class == kEventClassMouse || event->event_class == kEventClassKeyboard)) return noErr;
        if (getenv("LP32_TRACE_CARBON_EVENTS")) fprintf(stderr, "Carbon event class=%08x kind=%u callback=%08x\n", event->event_class, event->kind, callback);
        event->time = ((double(*)(void *))carbon_native32_symbol("GetEventTime"))(native_event);
        event->native_event = ((void *(*)(void *))carbon_native32_symbol("RetainEvent"))(native_event);
        struct dispatch_context context = {compat_runtime32_allocate(4, 1), nil, 0, event, current_dispatch, call};
        if (!context.token) return memFullErr;
        current_dispatch = &context;
        uint32_t event_handle = objc_bridge32_owned_object(event);
        uint32_t args[] = {call ? context.token : 0, event_handle, user_data};
        OSStatus status = compat_runtime32_call(callback, args, 3);
        bool trapped = compat_runtime32_last_call_trapped();
        uint64_t ignored; objc_bridge32_dispatch("_CFRelease", &event_handle, &ignored);
        current_dispatch = context.previous;
        compat_runtime32_deallocate(context.token);
        if (trapped) { fflush(NULL); _Exit(EXIT_FAILURE); }
        return status;
    }
}
static OSStatus native_event_callback(void *call, void *event, void *opaque) {
    LP32CarbonHandler *handler = opaque;
    return call_native_event(handler->callback, handler->user_data, call, event);
}

/* HIObject replaces its construction userData with the instance pointer
   supplied by the constructor. A separate trampoline identifies the guest
   class callback independently of that changing userData. */
static LP32CarbonHandler *native_classes[32];
static unsigned native_class_count;
static OSStatus class_event(unsigned index, void *call, void *event, void *opaque) {
    LP32CarbonHandler *handler = native_classes[index];
    uint32_t event_class = ((uint32_t(*)(void *))carbon_native32_symbol("GetEventClass"))(event);
    uint32_t kind = ((uint32_t(*)(void *))carbon_native32_symbol("GetEventKind"))(event);
    uint32_t data = event_class == 'hiob' && kind == 1 ? handler->user_data : (uint32_t)(uintptr_t)opaque;
    return call_native_event(handler->callback, data, call, event);
}
#define CLASS_CALLBACK(n) static OSStatus class_callback_##n(void *call, void *event, void *opaque) { return class_event(n, call, event, opaque); }
CLASS_CALLBACK(0) CLASS_CALLBACK(1) CLASS_CALLBACK(2) CLASS_CALLBACK(3)
CLASS_CALLBACK(4) CLASS_CALLBACK(5) CLASS_CALLBACK(6) CLASS_CALLBACK(7)
CLASS_CALLBACK(8) CLASS_CALLBACK(9) CLASS_CALLBACK(10) CLASS_CALLBACK(11)
CLASS_CALLBACK(12) CLASS_CALLBACK(13) CLASS_CALLBACK(14) CLASS_CALLBACK(15)
CLASS_CALLBACK(16) CLASS_CALLBACK(17) CLASS_CALLBACK(18) CLASS_CALLBACK(19)
CLASS_CALLBACK(20) CLASS_CALLBACK(21) CLASS_CALLBACK(22) CLASS_CALLBACK(23)
CLASS_CALLBACK(24) CLASS_CALLBACK(25) CLASS_CALLBACK(26) CLASS_CALLBACK(27)
CLASS_CALLBACK(28) CLASS_CALLBACK(29) CLASS_CALLBACK(30) CLASS_CALLBACK(31)
#undef CLASS_CALLBACK
static void *class_callbacks[] = {
    class_callback_0, class_callback_1, class_callback_2, class_callback_3,
    class_callback_4, class_callback_5, class_callback_6, class_callback_7,
    class_callback_8, class_callback_9, class_callback_10, class_callback_11,
    class_callback_12, class_callback_13, class_callback_14, class_callback_15,
    class_callback_16, class_callback_17, class_callback_18, class_callback_19,
    class_callback_20, class_callback_21, class_callback_22, class_callback_23,
    class_callback_24, class_callback_25, class_callback_26, class_callback_27,
    class_callback_28, class_callback_29, class_callback_30, class_callback_31
};

static void *materialize_event(LP32CarbonEvent *event) {
    if (!event || event->native_event) return event ? event->native_event : NULL;
    OSStatus (*create)(void *, uint32_t, uint32_t, double, uint32_t, void **) = carbon_native32_symbol("CreateEvent");
    if (!create || create(NULL, event->event_class, event->kind, event->time, 0, &event->native_event)) return NULL;
    for (NSNumber *name in event->parameters) {
        NSDictionary *value = event->parameters[name]; NSData *data = value[@"data"];
        OSStatus status = carbon_native32_set_event_parameter(event->native_event, [name unsignedIntValue],
            [value[@"type"] unsignedIntValue], [data bytes], (uint32_t)[data length]);
        if (status) { fprintf(stderr, "compat32: native event parameter conversion failed: %d\n", (int)status); return NULL; }
    }
    return event->native_event;
}

static NSString *node_value(NSXMLElement *node, NSString *name) {
    for (NSXMLElement *child in [node children]) {
        if ([child kind] == NSXMLElementKind && [[[child attributeForName:@"name"] stringValue] isEqualToString:name]) return [child stringValue];
    }
    return nil;
}
static NSXMLElement *named_node(NSXMLDocument *document, NSString *name) {
    NSArray *tables = [document nodesForXPath:@"/object/dictionary[@name='nameTable']" error:NULL];
    NSString *identifier = nil;
    bool matched = false;
    for (NSXMLElement *child in [[tables firstObject] children]) {
        if ([child kind] != NSXMLElementKind) continue;
        if (matched) { identifier = [[child attributeForName:@"idRef"] stringValue]; break; }
        matched = [[child name] isEqualToString:@"string"] && [[child stringValue] isEqualToString:name];
    }
    if (!identifier) return nil;
    NSString *path = [NSString stringWithFormat:@"//object[@id='%@']", identifier];
    return [[document nodesForXPath:path error:NULL] firstObject];
}
static uint32_t fourcc(NSString *string) {
    const char *bytes = [string UTF8String];
    if (!bytes || strlen(bytes) != 4) return 0;
    return (uint32_t)(uint8_t)bytes[0] << 24 | (uint32_t)(uint8_t)bytes[1] << 16 | (uint32_t)(uint8_t)bytes[2] << 8 | (uint8_t)bytes[3];
}

@interface LP32CarbonActions : NSObject
- (void)invoke:(id)sender;
@end
@implementation LP32CarbonActions
- (void)invoke:(id)sender {
    uint32_t command = [objc_getAssociatedObject(sender, &menu_command_key) unsignedIntValue];
    LP32CarbonEvent *event = [[[LP32CarbonEvent alloc] init] autorelease];
    event->event_class = 'cmds'; event->kind = 1;
    uint32_t words[4] = {0, command, 0, 0};
    parameter(event, '----', 'hcmd', words, 14);
    int32_t status = send_event(event, [sender isKindOfClass:[NSMenuItem class]] ? app_target() : sender);
    if (command == 'quit' && status == eventNotHandledErr) { quit_loop = true; [game_window close]; }
}
@end
static LP32CarbonActions *actions(void) { static LP32CarbonActions *target; if (!target) target = [[LP32CarbonActions alloc] init]; return target; }

static NSMenu *menu_from_node(NSXMLElement *node) {
    NSMenu *menu = [[[NSMenu alloc] initWithTitle:node_value(node, @"title") ?: @""] autorelease];
    [menu setAutoenablesItems:NO];
    NSArray *items = [node nodesForXPath:@"array[@name='items']/object" error:NULL];
    for (NSXMLElement *item in items) {
        if ([node_value(item, @"separator") isEqualToString:@"TRUE"]) { [menu addItem:[NSMenuItem separatorItem]]; continue; }
        NSMenuItem *native = [[[NSMenuItem alloc] initWithTitle:node_value(item, @"title") ?: @"" action:@selector(invoke:) keyEquivalent:node_value(item, @"keyEquivalent") ?: @""] autorelease];
        [native setTarget:actions()];
        objc_setAssociatedObject(native, &menu_command_key, @(fourcc(node_value(item, @"command"))), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        if ([node_value(item, @"checked") isEqualToString:@"TRUE"]) [native setState:NSControlStateValueOn];
        NSXMLElement *submenu = [[item nodesForXPath:@"object[@name='submenu']" error:NULL] firstObject];
        if (submenu) [native setSubmenu:menu_from_node(submenu)];
        [menu addItem:native];
    }
    return menu;
}
static NSMenuItem *menu_item(uint32_t menu, uint32_t index) {
    NSMenu *native = object(menu);
    return index && index <= [native numberOfItems] ? [native itemAtIndex:index - 1] : nil;
}
static NSMenuItem *find_command(NSMenu *menu, uint32_t command, unsigned *skip) {
    for (NSMenuItem *item in [menu itemArray]) {
        if ([objc_getAssociatedObject(item, &menu_command_key) unsignedIntValue] == command && (*skip)-- == 0) return item;
        NSMenuItem *found = [item submenu] ? find_command([item submenu], command, skip) : nil;
        if (found) return found;
    }
    return nil;
}

int carbon_ui32_dispatch(const char *name, const uint32_t *a, uint64_t *result)
{
#define IS(s) (!strcmp(name, "_" s))
#define PTR(i) ((void *)(uintptr_t)a[i])
#define OBJ(i) object(a[i])
#define RETURN(v) do { *result = (uint64_t)(v); return 1; } while (0)
    if (carbon_native32_dispatch(name, a, result)) return 1;
    if (IS("InstallEventLoopTimer")) {
        if (!a[5] || !a[7]) RETURN((uint32_t)paramErr);
        double delay, interval; memcpy(&delay, a + 1, 8); memcpy(&interval, a + 3, 8);
        LP32CarbonTimer *timer = [[[LP32CarbonTimer alloc] init] autorelease];
        timer->callback = a[5]; timer->user_data = a[6];
        timer->guest_handle = objc_bridge32_owned_object(timer);
        OSStatus status = InstallEventLoopTimer(carbon_native32_pointer(a[0]), delay, interval,
            native_timer_callback, timer, &timer->native_timer);
        *(uint32_t *)PTR(7) = status ? 0 : timer->guest_handle;
        if (status) { uint64_t ignored; objc_bridge32_dispatch("_CFRelease", &timer->guest_handle, &ignored); }
        RETURN((uint32_t)status);
    }
    if (IS("RemoveEventLoopTimer") || IS("SetEventLoopTimerNextFireTime")) {
        LP32CarbonTimer *timer = OBJ(0);
        if (![timer isKindOfClass:[LP32CarbonTimer class]] || !timer->native_timer) RETURN((uint32_t)paramErr);
        if (IS("RemoveEventLoopTimer")) {
            OSStatus status = RemoveEventLoopTimer(timer->native_timer);
            if (!status) { timer->native_timer = NULL; uint64_t ignored; objc_bridge32_dispatch("_CFRelease", a, &ignored); }
            RETURN((uint32_t)status);
        }
        double delay; memcpy(&delay, a + 1, 8);
        RETURN((uint32_t)SetEventLoopTimerNextFireTime(timer->native_timer, delay));
    }
    if (IS("HIObjectRegisterSubclass")) {
        OSStatus (*register_class)(CFStringRef, CFStringRef, uint32_t, void *, ItemCount, const void *, void *, void **) = carbon_native32_symbol("HIObjectRegisterSubclass");
        if (!register_class) RETURN((uint32_t)unimpErr);
        if (native_class_count == 32) RETURN((uint32_t)memFullErr);
        LP32CarbonHandler *handler = [[LP32CarbonHandler alloc] init];
        handler->callback = a[3]; handler->user_data = a[6];
        unsigned index = native_class_count++;
        native_classes[index] = handler;
        void *class_ref = NULL;
        OSStatus status = register_class((CFStringRef)OBJ(0), (CFStringRef)OBJ(1), a[2], a[3] ? class_callbacks[index] : NULL,
            a[4], PTR(5), handler, &class_ref);
        if (status) { native_classes[index] = nil; --native_class_count; [handler release]; }
        if (a[7]) *(uint32_t *)PTR(7) = carbon_native32_handle(class_ref);
        RETURN((uint32_t)status);
    }
    if (IS("HIObjectCreate")) {
        OSStatus (*create)(CFStringRef, void *, void **) = carbon_native32_symbol("HIObjectCreate");
        if (!create || !a[2]) RETURN((uint32_t)paramErr);
        void *native_event = a[1] ? materialize_event(OBJ(1)) : NULL;
        if (a[1] && !native_event) RETURN((uint32_t)paramErr);
        void *created = NULL;
        OSStatus status = create((CFStringRef)OBJ(0), native_event, &created);
        *(uint32_t *)PTR(2) = carbon_native32_owned_cf(created); RETURN((uint32_t)status);
    }
    if (IS("SetMenuBarFromNib")) {
        NSXMLElement *node = named_node(OBJ(0), OBJ(1));
        if (!node) RETURN((uint32_t)fnfErr);
        NSMenu *menu = menu_from_node(node);
        if (!getenv("LP32_HEADLESS")) {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            [NSApp setMainMenu:menu];
        } else { /* Keep the parsed menu accessible to GetIndMenuItemWithCommandID. */
            objc_setAssociatedObject(app_target(), &menu_properties_key, menu, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }
        RETURN(0);
    }
    if (IS("GetIndMenuItemWithCommandID")) {
        NSMenu *menu = a[0] ? OBJ(0) : [NSApp mainMenu] ?: objc_getAssociatedObject(app_target(), &menu_properties_key);
        unsigned skip = a[2] ? a[2] - 1 : 0;
        NSMenuItem *item = find_command(menu, a[1], &skip);
        if (a[3]) *(uint32_t *)PTR(3) = handle([item menu]);
        if (a[4]) *(uint16_t *)PTR(4) = item ? [[item menu] indexOfItem:item] + 1 : 0;
        RETURN(item ? 0 : (uint32_t)menuItemNotFoundErr);
    }
    if (IS("CountMenuItems")) RETURN([OBJ(0) numberOfItems]);
    if (IS("InsertMenuItemTextWithCFString")) {
        NSMenu *menu = OBJ(0);
        NSMenuItem *item = [[[NSMenuItem alloc] initWithTitle:OBJ(1) ?: @"" action:@selector(invoke:) keyEquivalent:@""] autorelease];
        [item setTarget:actions()]; [item setEnabled:!(a[3] & kMenuItemAttrDisabled)];
        objc_setAssociatedObject(item, &menu_command_key, @(a[4]), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        NSUInteger index = MIN(a[2], [menu numberOfItems]);
        [menu insertItem:item atIndex:index];
        if (a[5]) *(uint16_t *)PTR(5) = index + 1;
        RETURN(0);
    }
    if (IS("DisableMenuCommand")) {
        NSMenu *menu = a[0] ? OBJ(0) : [NSApp mainMenu] ?: objc_getAssociatedObject(app_target(), &menu_properties_key);
        unsigned skip = 0; NSMenuItem *item = find_command(menu, a[1], &skip);
        [item setEnabled:NO]; RETURN(0);
    }
    if (IS("SetMenuItemProperty") || IS("GetMenuItemProperty")) {
        NSMenuItem *item = menu_item(a[0], a[1]);
        if (!item) RETURN((uint32_t)menuItemNotFoundErr);
        NSMutableDictionary *properties = objc_getAssociatedObject(item, &menu_properties_key);
        NSNumber *key = event_type(a[2], a[3]);
        if (IS("SetMenuItemProperty")) {
            if (!properties) { properties = [NSMutableDictionary dictionary]; objc_setAssociatedObject(item, &menu_properties_key, properties, OBJC_ASSOCIATION_RETAIN_NONATOMIC); }
            [properties setObject:[NSData dataWithBytes:PTR(5) length:a[4]] forKey:key];
        } else {
            NSData *data = [properties objectForKey:key];
            if (a[5]) *(uint32_t *)PTR(5) = (uint32_t)[data length];
            if (!data || a[4] < [data length]) RETURN((uint32_t)fnfErr);
            if (a[6]) memcpy(PTR(6), [data bytes], [data length]);
        }
        RETURN(0);
    }
    if (IS("SetMenuItemTextWithCFString")) { [menu_item(a[0], a[1]) setTitle:OBJ(2) ?: @""]; RETURN(0); }
    if (IS("CopyMenuItemTextAsCFString")) { if (a[2]) *(uint32_t *)PTR(2) = objc_bridge32_owned_object([menu_item(a[0], a[1]) title]); RETURN(0); }
    if (IS("DisableMenuItem") || IS("EnableMenuItem")) { [menu_item(a[0], a[1]) setEnabled:IS("EnableMenuItem")]; RETURN(0); }
    if (IS("IsMenuItemEnabled")) RETURN([menu_item(a[0], a[1]) isEnabled]);
    if (IS("DeleteMenuItems")) {
        NSMenu *menu = OBJ(0);
        for (unsigned i = 0; a[1] && i < a[2] && a[1] <= [menu numberOfItems]; ++i) [menu removeItemAtIndex:a[1] - 1];
        RETURN(0);
    }
    if (IS("GetApplicationEventTarget") || IS("GetEventDispatcherTarget")) RETURN(handle(app_target()));
    if (IS("GetWindowEventTarget") || IS("GetControlEventTarget")) RETURN(a[0]);
    if (IS("GetCurrentEventLoop") || IS("GetCurrentEventQueue") || IS("GetMainEventQueue")) RETURN(handle(app_target()));
    if (IS("InstallEventHandler")) {
        LP32CarbonHandler *handler = [[[LP32CarbonHandler alloc] init] autorelease];
        handler->callback = a[1]; handler->user_data = a[4]; handler->target = OBJ(0) ?: app_target();
        const uint32_t *types = PTR(3);
        if (!types && a[2]) RETURN((uint32_t)paramErr);
        for (uint32_t i = 0; i < a[2]; ++i) [handler->types addObject:event_type(types[2*i], types[2*i+1])];
        void *target = carbon_native32_pointer(handle(handler->target));
        if (target) {
            OSStatus (*install)(void *, void *, uint32_t, const void *, void *, void **) = carbon_native32_symbol("InstallEventHandler");
            OSStatus status = install ? install(target, native_event_callback, a[2], types, handler, &handler->native_handler) : unimpErr;
            if (status) RETURN((uint32_t)status);
        }
        [handlers(handler->target) insertObject:handler atIndex:0];
        if (a[5]) *(uint32_t *)PTR(5) = handle(handler);
        RETURN(0);
    }
    if (IS("RemoveEventHandler")) { LP32CarbonHandler *handler = OBJ(0); if (handler) {
        if (handler->native_handler) ((OSStatus(*)(void *))carbon_native32_symbol("RemoveEventHandler"))(handler->native_handler);
        handler->native_handler = NULL; [handlers(handler->target) removeObject:handler]; handler->callback = 0;
    } RETURN(0); }
    if (IS("AddEventTypesToHandler") || IS("RemoveEventTypesFromHandler")) {
        LP32CarbonHandler *handler = OBJ(0); const uint32_t *types = PTR(2);
        if (handler && handler->native_handler) {
            OSStatus (*update)(void *, uint32_t, const void *) = carbon_native32_symbol(name + 1);
            OSStatus status = update ? update(handler->native_handler, a[1], types) : unimpErr;
            if (status) RETURN((uint32_t)status);
        }
        for (uint32_t i = 0; handler && types && i < a[1]; ++i) {
            NSNumber *type = event_type(types[2*i], types[2*i+1]);
            if (IS("AddEventTypesToHandler")) [handler->types addObject:type]; else [handler->types removeObject:type];
        }
        RETURN(0);
    }
    if (IS("CreateEvent")) {
        if (!a[6]) RETURN((uint32_t)paramErr);
        LP32CarbonEvent *event = [[[LP32CarbonEvent alloc] init] autorelease];
        event->event_class = a[1]; event->kind = a[2]; memcpy(&event->time, a + 3, 8);
        *(uint32_t *)PTR(6) = objc_bridge32_owned_object(event);
        RETURN(0);
    }
    if (IS("GetEventClass")) { LP32CarbonEvent *event = OBJ(0); RETURN(event ? event->event_class : 0); }
    if (IS("GetEventKind")) { LP32CarbonEvent *event = OBJ(0); RETURN(event ? event->kind : 0); }
    if (IS("GetEventTime")) { LP32CarbonEvent *event = OBJ(0); RETURN(compat_runtime32_return_double(event ? event->time : 0)); }
    if (IS("GetEventRetainCount")) { LP32CarbonEvent *event = OBJ(0); RETURN(event ? event->references : 0); }
    if (IS("RetainEvent") || IS("ReleaseEvent")) {
        LP32CarbonEvent *event = OBJ(0);
        if (event) { if (IS("RetainEvent")) ++event->references; else --event->references; }
        uint64_t ignored; objc_bridge32_dispatch(IS("RetainEvent") ? "_CFRetain" : "_CFRelease", a, &ignored);
        RETURN(IS("RetainEvent") ? a[0] : 0);
    }
    if (IS("SetEventParameter")) {
        LP32CarbonEvent *event = OBJ(0); parameter(event, a[1], a[2], PTR(4), a[3]);
        RETURN((uint32_t)(event && event->native_event ? carbon_native32_set_event_parameter(event->native_event, a[1], a[2], PTR(4), a[3]) : noErr));
    }
    if (IS("GetEventParameter")) {
        LP32CarbonEvent *event = OBJ(0);
        if (event && event->native_event) RETURN((uint32_t)carbon_native32_event_parameter(event->native_event, a));
        NSDictionary *value = event ? [event->parameters objectForKey:@(a[1])] : nil;
        if (!value) RETURN((uint32_t)eventParameterNotFoundErr);
        uint32_t type = [value[@"type"] unsignedIntValue]; NSData *data = value[@"data"];
        if (a[3]) *(uint32_t *)PTR(3) = type;
        if (a[5]) *(uint32_t *)PTR(5) = (uint32_t)[data length];
        if (a[2] != '****' && a[2] != type) RETURN((uint32_t)eventParameterNotFoundErr);
        if (a[6] && a[4] < [data length]) RETURN((uint32_t)eventParameterNotFoundErr);
        if (a[6]) memcpy(PTR(6), [data bytes], [data length]);
        RETURN(0);
    }
    if (IS("SendEventToEventTarget")) {
        void *target = carbon_native32_pointer(a[1]);
        if (target) {
            void *event = materialize_event(OBJ(0));
            RETURN((uint32_t)(event ? SendEventToEventTarget(event, target) : paramErr));
        }
        RETURN((uint32_t)send_event(OBJ(0), OBJ(1)));
    }
    if (IS("CallNextEventHandler")) {
        struct dispatch_context *context = current_dispatch;
        while (context && context->token != a[0]) context = context->previous;
        if (context && context->native_call) {
            RETURN((uint32_t)((OSStatus(*)(void *, void *))carbon_native32_symbol("CallNextEventHandler"))(context->native_call, context->event->native_event));
        }
        RETURN((uint32_t)(context ? send_next(context) : eventNotHandledErr));
    }
    if (IS("PostEventToQueue")) {
        void *queue = carbon_native32_pointer(a[0]);
        if (queue) {
            void *event = materialize_event(OBJ(1));
            OSStatus (*post)(void *, void *, int16_t) = carbon_native32_symbol("PostEventToQueue");
            RETURN((uint32_t)(event && post ? post(queue, event, a[2]) : paramErr));
        }
        if (!event_queue) event_queue = [[NSMutableArray alloc] init];
        id event = OBJ(1); if (!event) RETURN((uint32_t)paramErr);
        [event_queue addObject:event]; RETURN(0);
    }
    if (IS("ReceiveNextEvent")) {
        if (!a[5]) RETURN((uint32_t)paramErr);
        carbon_native32_pump_appkit_events();
        poll_test_keys();
        double timeout; memcpy(&timeout, a + 2, 8); EventRef native = NULL;
        OSStatus status = ReceiveNextEvent(a[0], PTR(1), timeout, a[4], &native);
        *(uint32_t *)PTR(5) = 0;
        if (!status && native) {
            LP32CarbonEvent *event = [[[LP32CarbonEvent alloc] init] autorelease];
            event->native_event = a[4] ? native : RetainEvent(native);
            event->event_class = GetEventClass(native); event->kind = GetEventKind(native); event->time = GetEventTime(native);
            *(uint32_t *)PTR(5) = objc_bridge32_owned_object(event);
        }
        RETURN((uint32_t)status);
    }
    if (IS("QuitApplicationEventLoop")) { quit_loop = true; RETURN(0); }
    return 0;
#undef IS
#undef PTR
#undef OBJ
#undef RETURN
}
