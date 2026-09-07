#include "carbon_ui.h"
#include "carbon_text.h"
#import <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>
#include <dlfcn.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/* Carbon remains the guest's control model and event target. AppKit presents
 * that model because several Carbon widgets no longer exist in 64-bit macOS. */
static void *carbon;
static void *api(const char *name) {
  if (!carbon)
    carbon = dlopen("/System/Library/Frameworks/Carbon.framework/Carbon",
                    RTLD_NOW | RTLD_LOCAL);
  return dlsym(carbon, name);
}
@interface LP32FlippedView : NSView
@end
@implementation LP32FlippedView
- (BOOL)isFlipped {
  return YES;
}
@end
@interface LP32CarbonPanel : NSObject <NSWindowDelegate, NSTextFieldDelegate> {
@public
  void *native;
  NSWindow *window;
  NSMutableDictionary *controls;
  NSTimer *timer;
  BOOL modal, native_rendering, syncing;
}
- (void)sync;
- (void)action:(NSControl *)sender;
@end
static NSMutableDictionary *panels;
static void place_test_panel(NSWindow *window) {
  const char *setting = getenv("LP32_TEST_DISPLAY");
  if (!setting || !*setting || [window sheetParent])
    return;
  CGDirectDisplayID display = (CGDirectDisplayID)strtoul(setting, NULL, 0);
  for (NSScreen *screen in [NSScreen screens]) {
    if ([[[screen deviceDescription] objectForKey:@"NSScreenNumber"]
            unsignedIntValue] != display)
      continue;
    NSRect available = [screen visibleFrame], frame = [window frame];
    NSPoint origin = NSMakePoint(
        NSMidX(available) - frame.size.width / 2,
        NSMaxY(available) - frame.size.height -
            fmax(0, (available.size.height - frame.size.height) / 3));
    if (!NSEqualPoints(frame.origin, origin))
      [window setFrameOrigin:origin];
    return;
  }
}
static NSString *title(void *view, BOOL text) {
  CFStringRef string = NULL;
  if (text) {
    ByteCount size = 0;
    ((OSStatus (*)(void *, int16_t, uint32_t, ByteCount, void *,
                   ByteCount *))api("GetControlData"))(
        view, 0, 'cfst', sizeof(string), &string, &size);
  }
  if (!string)
    ((OSStatus (*)(void *, CFStringRef *))api("CopyControlTitleAsCFString"))(
        view, &string);
  return string ? [(NSString *)string autorelease] : @"";
}
static void sync_children(LP32CarbonPanel *panel, void *parent, void *root,
                          NSMutableSet *seen, unsigned depth) {
  if (depth > 32)
    return;
  void *view = ((void *(*)(void *))api("HIViewGetFirstSubview"))(parent);
  for (; view; view = ((void *(*)(void *))api("HIViewGetNextView"))(view)) {
    if (!((Boolean (*)(void *))api("HIViewIsLatentlyVisible"))(view))
      continue;
    ControlKind kind = {0};
    ((OSStatus (*)(void *, ControlKind *))api("GetControlKind"))(view, &kind);
    BOOL edit = carbon_text_is_control(view), label = kind.kind == 'stxt';
    BOOL button = kind.kind == 'push' || kind.kind == 'cbox' ||
                  kind.kind == 'rdio' || kind.kind == 'bevl' ||
                  kind.kind == 'rndb' || kind.kind == 'cgrp';
    BOOL popup = kind.kind == 'popb', slider = kind.kind == 'sldr';
    if (edit || label || button || popup || slider) {
      NSValue *key = [NSValue valueWithPointer:view];
      [seen addObject:key];
      NSControl *control = [panel->controls objectForKey:key];
      if (!control) {
        if (edit || label) {
          NSTextField *field = [[NSTextField alloc] initWithFrame:NSZeroRect];
          [field setEditable:edit];
          [field setSelectable:edit];
          [field setBezeled:edit];
          [field setDrawsBackground:edit];
          [field setFont:[NSFont systemFontOfSize:13]];
          if (edit)
            [field setDelegate:panel];
          control = field;
        } else if (popup)
          control = [[NSPopUpButton alloc] initWithFrame:NSZeroRect
                                               pullsDown:NO];
        else if (slider)
          control = [[NSSlider alloc] initWithFrame:NSZeroRect];
        else {
          NSButton *b = [[NSButton alloc] initWithFrame:NSZeroRect];
          [b setButtonType:kind.kind == 'cbox' || kind.kind == 'cgrp'
                               ? NSSwitchButton
                           : kind.kind == 'rdio' ? NSRadioButton
                                                 : NSMomentaryPushInButton];
          [b setBezelStyle:NSBezelStyleRounded];
          control = b;
        }
        [control setTag:(NSInteger)view];
        [control setTarget:panel];
        [control setAction:@selector(action:)];
        [[panel->window contentView] addSubview:control];
        [panel->controls setObject:control forKey:key];
        [control release];
      }
      CGRect rect = {0};
      ((OSStatus (*)(void *, CGRect *))api("HIViewGetBounds"))(view, &rect);
      ((OSStatus (*)(CGRect *, void *, void *))api("HIViewConvertRect"))(
          &rect, view, root);
      if (edit) {
        rect.origin.y -= 2;
        rect.size.height = fmax(22, rect.size.height + 4);
      } else if (button || popup) {
        rect.origin.y -= 2;
        rect.size.height = fmax(24, rect.size.height + 4);
      }
      [control setFrame:rect];
      [control setHidden:NO];
      [control setEnabled:((Boolean (*)(void *))api("HIViewIsEnabled"))(view)];
      if (edit) {
        NSString *value = (NSString *)carbon_text_copy_value(view);
        if (![[control stringValue] isEqualToString:value])
          [control setStringValue:value ?: @""];
        [value release];
      } else if (label)
        [control setStringValue:title(view, YES)];
      else if (button) {
        [(NSButton *)control setTitle:title(view, NO)];
        if (kind.kind == 'cbox' || kind.kind == 'cgrp' || kind.kind == 'rdio')
          [(NSButton *)control
              setState:((int32_t (*)(void *))api("HIViewGetValue"))(view)
                           ? NSControlStateValueOn
                           : NSControlStateValueOff];
      } else if (slider) {
        [(NSSlider *)control
            setMinValue:((int32_t (*)(void *))api("HIViewGetMinimum"))(view)];
        [(NSSlider *)control
            setMaxValue:((int32_t (*)(void *))api("HIViewGetMaximum"))(view)];
        [control
            setIntValue:((int32_t (*)(void *))api("HIViewGetValue"))(view)];
      } else if (popup) {
        void *menu = NULL;
        ByteCount size = 0;
        ((OSStatus (*)(void *, int16_t, uint32_t, ByteCount, void *,
                       ByteCount *))api("GetControlData"))(
            view, 0, 'mhan', sizeof(menu), &menu, &size);
        if (menu) {
          uint16_t count = ((uint16_t (*)(void *))api("CountMenuItems"))(menu);
          NSPopUpButton *p = (NSPopUpButton *)control;
          if ([p numberOfItems] != count) {
            [p removeAllItems];
            for (uint16_t i = 1; i <= count; ++i) {
              CFStringRef text = NULL;
              ((OSStatus (*)(void *, uint16_t, CFStringRef *))api(
                  "CopyMenuItemTextAsCFString"))(menu, i, &text);
              [p addItemWithTitle:(NSString *)text ?: @""];
              if (text)
                CFRelease(text);
            }
          }
          int32_t value = ((int32_t (*)(void *))api("HIViewGetValue"))(view);
          if (value > 0 && value <= count)
            [p selectItemAtIndex:value - 1];
        }
      }
    }
    sync_children(panel, view, root, seen, depth + 1);
  }
}
@implementation LP32CarbonPanel
- (void)dealloc {
  [timer invalidate];
  [timer release];
  [window setDelegate:nil];
  [window close];
  [window release];
  [controls release];
  [super dealloc];
}
- (void)sync {
  if (syncing || native_rendering)
    return;
  syncing = YES;
  place_test_panel(window);
  void *root = NULL;
  ((OSStatus (*)(void *, void **))api("GetRootControl"))(native, &root);
  NSMutableSet *seen = [NSMutableSet set];
  if (root)
    sync_children(self, root, root, seen, 0);
  for (NSValue *key in controls)
    if (![seen containsObject:key])
      [[controls objectForKey:key] setHidden:YES];
  syncing = NO;
}
- (void)action:(NSControl *)sender {
  [self retain];
  void *view = (void *)[sender tag];
  if ([sender isKindOfClass:[NSPopUpButton class]])
    ((OSStatus (*)(void *, int32_t))api("HIViewSetValue"))(
        view, (int32_t)[(NSPopUpButton *)sender indexOfSelectedItem] + 1);
  if ([sender isKindOfClass:[NSSlider class]])
    ((OSStatus (*)(void *, int32_t))api("HIViewSetValue"))(view,
                                                           [sender intValue]);
  ControlKind kind = {0};
  ((OSStatus (*)(void *, ControlKind *))api("GetControlKind"))(view, &kind);
  int16_t part = kind.kind == 'cbox' || kind.kind == 'cgrp' ? 11
                 : kind.kind == 'rdio'                      ? 12
                                                            : 10,
          clicked = 0;
  OSStatus (*click)(void *, int16_t, uint32_t, int16_t *) =
      api("HIViewSimulateClick");
  OSStatus status = click ? click(view, part, 0, &clicked) : eventNotHandledErr;
  if (!clicked && (status == noErr || status == eventNotHandledErr)) {
    EventRef event = NULL;
    CreateEvent(NULL, kEventClassControl, kEventControlHit,
                GetCurrentEventTime(), 0, &event);
    SetEventParameter(event, kEventParamDirectObject, typeControlRef,
                      sizeof(view), &view);
    SetEventParameter(event, kEventParamControlPart, typeControlPartCode,
                      sizeof(part), &part);
    uint32_t modifiers = 0;
    SetEventParameter(event, kEventParamKeyModifiers, typeUInt32,
                      sizeof(modifiers), &modifiers);
    status = SendEventToEventTarget(
        event, ((void *(*)(void *))api("GetControlEventTarget"))(view));
    ReleaseEvent(event);
  }
  if (status == eventNotHandledErr) {
    HICommandExtended command = {0};
    ((OSStatus (*)(void *, uint32_t *))api("HIViewGetCommandID"))(
        view, &command.commandID);
    if (command.commandID) {
      command.attributes = kHICommandFromControl;
      command.source.control = view;
      EventRef event = NULL;
      CreateEvent(NULL, kEventClassCommand, kEventCommandProcess,
                  GetCurrentEventTime(), 0, &event);
      SetEventParameter(event, kEventParamDirectObject, typeHICommand,
                        sizeof(command), &command);
      status = SendEventToEventTarget(
          event, ((void *(*)(void *))api("GetWindowEventTarget"))(native));
      ReleaseEvent(event);
    }
    fprintf(stderr, "compat32: control command=%#x status=%d\n",
            command.commandID, status);
  }
  [self sync];
  [self release];
}
- (void)controlTextDidChange:(NSNotification *)note {
  if (syncing)
    return;
  NSTextField *field = [note object];
  carbon_text_set_value((void *)[field tag], [field stringValue]);
  [self sync];
}
- (BOOL)windowShouldClose:(NSWindow *)sender {
  (void)sender;
  for (NSControl *control in [controls allValues])
    if ([control isKindOfClass:[NSButton class]] &&
        [[(NSButton *)control title] isEqualToString:@"Quit"] &&
        ![control isHidden]) {
      [self action:control];
      return NO;
    }
  if (modal)
    [NSApp stopModal];
  return YES;
}
@end
static LP32CarbonPanel *panel_for(void *native, BOOL create) {
  if (!panels && create)
    panels = [[NSMutableDictionary alloc] init];
  NSValue *key = [NSValue valueWithPointer:native];
  LP32CarbonPanel *panel = [panels objectForKey:key];
  if (panel || !create)
    return panel;
  NSApplicationLoad();
  panel = [[LP32CarbonPanel alloc] init];
  panel->native = native;
  panel->controls = [[NSMutableDictionary alloc] init];
  Rect bounds = {0};
  ((OSStatus (*)(void *, uint32_t, Rect *))api("GetWindowBounds"))(native, 33,
                                                                   &bounds);
  CGRect screen = CGDisplayBounds(CGMainDisplayID());
  NSRect rect =
      NSMakeRect(bounds.left, screen.size.height - bounds.bottom,
                 bounds.right - bounds.left, bounds.bottom - bounds.top);
  panel->window = [[NSWindow alloc]
      initWithContentRect:rect
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                  backing:NSBackingStoreBuffered
                    defer:NO];
  [panel->window setReleasedWhenClosed:NO];
  [panel->window setDelegate:panel];
  [panel->window setTitle:[[NSBundle mainBundle]
                              objectForInfoDictionaryKey:@"CFBundleName"]
                              ?: @"Game"];
  [panel->window
      setContentView:[[[LP32FlippedView alloc]
                         initWithFrame:NSMakeRect(0, 0, rect.size.width,
                                                  rect.size.height)]
                         autorelease]];
  [panels setObject:panel forKey:key];
  [panel release];
  return panel;
}
void carbon_ui_show(void *native) {
  LP32CarbonPanel *p = panel_for(native, YES);
  if (p->native_rendering) {
    ((void (*)(void *))api("ShowWindow"))(native);
    return;
  }
  [p sync];
  if (getenv("LP32_BACKGROUND_TEST"))
    [p->window orderBack:nil];
  else
    [p->window makeKeyAndOrderFront:nil];
}
void carbon_ui_hide(void *native) {
  LP32CarbonPanel *p = panel_for(native, NO);
  if (p)
    [p->window orderOut:nil];
}
void carbon_ui_dispose(void *native) {
  LP32CarbonPanel *p = panel_for(native, NO);
  if (p && p->modal)
    [NSApp stopModal];
  [panels removeObjectForKey:[NSValue valueWithPointer:native]];
}
int32_t carbon_ui_run_modal(void *native) {
  LP32CarbonPanel *p = [panel_for(native, YES) retain];
  carbon_ui_show(native);
  p->modal = YES;
  p->timer = [[NSTimer timerWithTimeInterval:0.15
                                      target:p
                                    selector:@selector(sync)
                                    userInfo:nil
                                     repeats:YES] retain];
  [[NSRunLoop mainRunLoop] addTimer:p->timer forMode:NSModalPanelRunLoopMode];
  [NSApp runModalForWindow:p->window];
  [p->timer invalidate];
  [p->timer release];
  p->timer = nil;
  p->modal = NO;
  [p release];
  return 0;
}
int carbon_ui_stop_modal(void *native) {
  LP32CarbonPanel *p = panel_for(native, NO);
  if (!p || !p->modal)
    return 0;
  [NSApp stopModal];
  return 1;
}
void carbon_ui_use_native(void *native) {
  LP32CarbonPanel *p = panel_for(native, NO);
  if (p) {
    p->native_rendering = YES;
    [p->window orderOut:nil];
  }
  ((void (*)(void *))api("ShowWindow"))(native);
}

int carbon_ui_is_visible(void *native) {
  LP32CarbonPanel *p = panel_for(native, NO);
  return !p || p->native_rendering ? -1 : [p->window isVisible];
}

int32_t carbon_ui_show_sheet(void *sheet, void *parent) {
  LP32CarbonPanel *p = panel_for(sheet, YES), *owner = panel_for(parent, YES);
  [p sync];
  if (!p->timer) {
    p->timer = [[NSTimer timerWithTimeInterval:0.15
                                        target:p
                                      selector:@selector(sync)
                                      userInfo:nil
                                       repeats:YES] retain];
    [[NSRunLoop mainRunLoop] addTimer:p->timer forMode:NSModalPanelRunLoopMode];
    [[NSRunLoop mainRunLoop] addTimer:p->timer forMode:NSDefaultRunLoopMode];
  }
  [owner->window beginSheet:p->window completionHandler:nil];
  return 0;
}
int32_t carbon_ui_hide_sheet(void *sheet) {
  LP32CarbonPanel *p = panel_for(sheet, NO);
  if (!p)
    return -50;
  NSWindow *parent = [p->window sheetParent];
  if (parent)
    [parent endSheet:p->window];
  [p->window orderOut:nil];
  [p->timer invalidate];
  [p->timer release];
  p->timer = nil;
  return 0;
}
