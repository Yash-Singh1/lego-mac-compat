#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>
#include "carbon_native.h"
#include "objc_bridge.h"
#include "game_profile.h"

/* Present TFU's Carbon keybinding controls in AppKit. The original NIB supplies
   geometry, titles, and command IDs; the guest still owns edits and persistence. */
@interface LP32BindingWindow : NSWindow {
@public void *peer;
    bool *finished;
    BOOL capturing;
    NSMutableArray *buttons;
}
- (void)refreshTitles;
@end

static NSString *control_title(void *control) {
    CFStringRef title = NULL;
    OSStatus (*copy)(void *, CFStringRef *) = carbon_native32_symbol("CopyControlTitleAsCFString");
    if (copy) copy(control, &title);
    if (!title || !CFStringGetLength(title)) {
        if (title) CFRelease(title);
        title = NULL;
        OSStatus (*get)(void *, int16_t, UInt32, Size, void *, Size *) = carbon_native32_symbol("GetControlData");
        if (get) get(control, 0, kControlStaticTextCFStringTag, sizeof(title), &title, NULL);
    }
    return [(NSString *)title autorelease] ?: @"";
}

@implementation LP32BindingWindow
- (void)dealloc { [buttons release]; [super dealloc]; }
- (BOOL)canBecomeKeyWindow { return YES; }
- (void)refreshTitles {
    if (*finished) return;
    for (NSButton *button in buttons) [button setTitle:control_title((void *)button.tag)];
}
- (void)bindingAction:(NSButton *)button {
    if (*finished) return;
    void *control = (void *)button.tag;
    UInt32 commandID = 0;
    OSStatus (*get)(void *, UInt32 *) = carbon_native32_symbol("HIViewGetCommandID");
    void *(*target)(void *) = carbon_native32_symbol("GetWindowEventTarget");
    if (!get || !target || get(control, &commandID) || !commandID) return;
    HICommandExtended command = {0};
    command.attributes = kHICommandFromControl;
    command.commandID = commandID;
    command.source.control = control;
    EventRef event = NULL;
    if (CreateEvent(NULL, kEventClassCommand, kEventCommandProcess, GetCurrentEventTime(), 0, &event)) return;
    SetEventParameter(event, kEventParamDirectObject, typeHICommand, sizeof(command), &command);
    capturing = commandID == 'KEYB';
    SendEventToEventTarget(event, target(peer));
    ReleaseEvent(event);
    [self refreshTitles];
    [self makeFirstResponder:nil];
}
- (void)sendEvent:(NSEvent *)event {
    if (*finished) return;
    if (capturing && event.eventRef &&
        (event.type == NSEventTypeKeyDown || event.type == NSEventTypeFlagsChanged ||
         event.type == NSEventTypeLeftMouseDown || event.type == NSEventTypeRightMouseDown ||
         event.type == NSEventTypeOtherMouseDown)) {
        void *(*target)(void *) = carbon_native32_symbol("GetWindowEventTarget");
        EventRef forwarded = CopyEvent((EventRef)event.eventRef);
        SetEventParameter(forwarded, kEventParamWindowRef, typeWindowRef, sizeof(peer), &peer);
        OSStatus status = SendEventToEventTarget(forwarded, target(peer));
        ReleaseEvent(forwarded);
        if (!status) capturing = NO;
        [self refreshTitles];
    } else [super sendEvent:event];
}
@end

static void add_controls(LP32BindingWindow *window, void *view, void *content, unsigned depth) {
    if (depth > 32) return;
    void *(*first)(void *) = carbon_native32_symbol("HIViewGetFirstSubview");
    void *(*next)(void *) = carbon_native32_symbol("HIViewGetNextView");
    OSStatus (*bounds)(void *, CGRect *) = carbon_native32_symbol("HIViewGetBounds");
    OSStatus (*convert)(CGRect *, void *, void *) = carbon_native32_symbol("HIViewConvertRect");
    OSStatus (*command)(void *, UInt32 *) = carbon_native32_symbol("HIViewGetCommandID");
    if (!first || !next || !bounds || !convert || !command) return;
    for (void *child = first(view); child; child = next(child)) {
        CGRect rect = {0}; UInt32 commandID = 0;
        bounds(child, &rect); convert(&rect, child, content);
        command(child, &commandID);
        NSString *title = control_title(child);
        NSRect frame = NSMakeRect(rect.origin.x, window.contentView.bounds.size.height - CGRectGetMaxY(rect),
                                  rect.size.width, rect.size.height);
        if (commandID) {
            NSButton *button = [[[NSButton alloc] initWithFrame:frame] autorelease];
            button.title = title; button.tag = (NSInteger)child;
            button.bezelStyle = NSBezelStyleRounded;
            button.font = [NSFont systemFontOfSize:12];
            button.target = window; button.action = @selector(bindingAction:);
            if (commandID == kHICommandOK) button.keyEquivalent = @"\r";
            if (commandID == kHICommandCancel) button.keyEquivalent = @"\033";
            [window.contentView addSubview:button]; [window->buttons addObject:button];
        } else if (title.length) {
            NSTextField *label = [NSTextField labelWithString:title];
            if (frame.size.height < 17) {
                frame.origin.y -= (17 - frame.size.height) / 2;
                frame.size.height = 17;
            }
            label.frame = frame; label.font = [NSFont systemFontOfSize:13];
            label.alignment = NSTextAlignmentCenter;
            [window.contentView addSubview:label];
        }
        add_controls(window, child, content, depth + 1);
    }
}

void *carbon_native32_binding_window(uint32_t handle, void *done) {
    void *peer = carbon_native32_pointer(handle);
    void *(*root)(void *) = carbon_native32_symbol("HIViewGetRoot");
    OSStatus (*find)(void *, uint64_t, void **) = carbon_native32_symbol("HIViewFindByID");
    const uint64_t *contentID = carbon_native32_symbol("kHIViewWindowContentID");
    OSStatus (*bounds)(void *, CGRect *) = carbon_native32_symbol("HIViewGetBounds");
    if (!peer || !root || !find || !contentID || !bounds) return nil;
    void *content = NULL;
    if (find(root(peer), *contentID, &content) || !content) return nil;
    CGRect rect = {0};
    if (bounds(content, &rect) || rect.size.width <= 0 || rect.size.height <= 0) return nil;
    LP32BindingWindow *window = [[LP32BindingWindow alloc] initWithContentRect:rect
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    window->peer = peer; window->finished = done; window->buttons = [[NSMutableArray alloc] init];
    window.title = @"Keybindings";
    window.releasedWhenClosed = NO;
    add_controls(window, content, content, 0);
    [window center];
    return window;
}

@interface LP32LegacyAlert : NSObject {
@public NSAlert *alert;
    int16_t identifiers[3];
}
@end
@implementation LP32LegacyAlert
- (void)dealloc { [alert release]; [super dealloc]; }
@end

int carbon_dialog32_dispatch(const char *name, const uint32_t *a, uint64_t *result) {
    if (lp32_profile()->title != LP32_TITLE_TFU) return 0;
    if (!strcmp(name, "_CreateStandardAlert")) {
        if (!a[4]) { *result = (uint32_t)paramErr; return 1; }
        struct __attribute__((packed, aligned(2))) {
            uint32_t version; uint8_t movable, help;
            uint32_t titles[3]; int16_t defaultButton, cancelButton;
            uint16_t position; uint32_t flags;
        } params = {1, 1, 0, {UINT32_MAX, 0, 0}, 1, 0, 0, 0};
        if (a[3]) memcpy(&params, (void *)(uintptr_t)a[3], sizeof(params));
        LP32LegacyAlert *entry = [[[LP32LegacyAlert alloc] init] autorelease];
        entry->alert = [[NSAlert alloc] init];
        entry->alert.messageText = objc_bridge32_host_object(a[1]) ?: @"";
        entry->alert.informativeText = objc_bridge32_host_object(a[2]) ?: @"";
        entry->alert.alertStyle = a[0] == kAlertStopAlert ? NSAlertStyleCritical : NSAlertStyleInformational;
        unsigned count = 0;
        for (unsigned i = 0; i < 3; ++i) {
            if (!params.titles[i]) continue;
            NSString *title = params.titles[i] >= UINT32_MAX - 2 ?
                (@[@"OK", @"Cancel", @"Other"])[i] : objc_bridge32_host_object(params.titles[i]);
            NSButton *button = [entry->alert addButtonWithTitle:title ?: @""];
            button.keyEquivalent = params.defaultButton == (int)i + 1 ? @"\r" :
                params.cancelButton == (int)i + 1 ? @"\033" : @"";
            entry->identifiers[count++] = i + 1;
        }
        if (!count) { [entry->alert addButtonWithTitle:@"OK"]; entry->identifiers[0] = 1; }
        *(uint32_t *)(uintptr_t)a[4] = objc_bridge32_owned_object(entry);
        *result = noErr; return 1;
    }
    if (!strcmp(name, "_RunStandardAlert")) {
        LP32LegacyAlert *entry = objc_bridge32_host_object(a[0]);
        if (![entry isKindOfClass:[LP32LegacyAlert class]] || a[1]) { *result = (uint32_t)paramErr; return 1; }
        NSInteger choice = [entry->alert runModal] - NSAlertFirstButtonReturn;
        if (a[2]) *(int16_t *)(uintptr_t)a[2] = choice >= 0 && choice < 3 ? entry->identifiers[choice] : 0;
        uint64_t ignored; objc_bridge32_dispatch("_CFRelease", a, &ignored);
        *result = noErr; return 1;
    }
    return 0;
}
