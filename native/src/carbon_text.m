#include "carbon_text.h"
#include "compat_runtime.h"
#import <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>
#include <dlfcn.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/* Carbon's editable controls were removed from the 64-bit UI. Keep the
 * HIView/event model, and use Cocoa for text storage and field rendering. */
@interface LP32CarbonText : NSObject {
@public
  void *view;
  uint32_t token;
  uint32_t key_filter, validation;
  NSMutableString *text;
  NSRange selection;
  BOOL focused, password, locked;
}
@end
@implementation LP32CarbonText
- (void)dealloc {
  [text release];
  [super dealloc];
}
@end
static void *carbon;
static NSMutableDictionary *text_controls;
static void *sym(const char *name) {
  if (!carbon)
    carbon = dlopen("/System/Library/Frameworks/Carbon.framework/Carbon",
                    RTLD_NOW | RTLD_LOCAL);
  return dlsym(carbon, name);
}
static void invalidate(LP32CarbonText *state) {
  ((OSStatus (*)(void *, Boolean))sym("HIViewSetNeedsDisplay"))(state->view,
                                                                true);
}
static void changed(LP32CarbonText *state) {
  invalidate(state);
  EventRef event = NULL;
  CreateEvent(NULL, kEventClassControl, kEventControlValueFieldChanged,
              GetCurrentEventTime(), 0, &event);
  SetEventParameter(event, kEventParamDirectObject, typeControlRef,
                    sizeof(state->view), &state->view);
  void *target = ((void *(*)(void *))sym("GetControlEventTarget"))(state->view);
  SendEventToEventTarget(event, target);
  ReleaseEvent(event);
}
static void insert(LP32CarbonText *state, NSString *value) {
  if (state->locked)
    return;
  NSCharacterSet *newlines = [NSCharacterSet newlineCharacterSet];
  value = [[value componentsSeparatedByCharactersInSet:newlines]
      componentsJoinedByString:@""];
  NSUInteger length = [state->text length];
  state->selection.location = MIN(state->selection.location, length);
  state->selection.length =
      MIN(state->selection.length, length - state->selection.location);
  [state->text replaceCharactersInRange:state->selection withString:value];
  state->selection = NSMakeRange(state->selection.location + [value length], 0);
  changed(state);
}
static OSStatus handle_text(EventHandlerCallRef call, EventRef event,
                            void *raw) {
  (void)call;
  LP32CarbonText *state = raw;
  UInt32 kind = GetEventKind(event), cls = GetEventClass(event);
  if(getenv("LP32_TRACE_CARBON_TEXT"))fprintf(stderr,"compat32: text event view=%#x class=%#x kind=%u\n",state->token,cls,kind);
  if (cls == kEventClassControl) {
    if (kind == kEventControlDispose) {
      [text_controls removeObjectForKey:[NSValue valueWithPointer:state->view]];
      [state release];
      return eventNotHandledErr;
    }
    if (kind == kEventControlDraw) {
      CGContextRef context = NULL;
      if (GetEventParameter(event, kEventParamCGContextRef, typeCGContextRef,
                            NULL, sizeof(context), NULL, &context) ||
          !context)
        return eventNotHandledErr;
      CGRect bounds;
      ((OSStatus (*)(void *, CGRect *))sym("HIViewGetBounds"))(state->view,
                                                               &bounds);
      [NSGraphicsContext saveGraphicsState];
      [NSGraphicsContext
          setCurrentContext:[NSGraphicsContext
                                graphicsContextWithCGContext:context
                                                     flipped:YES]];
      [[NSColor whiteColor] setFill];
      NSRectFill(bounds);
      [[NSColor separatorColor] setStroke];
      NSFrameRect(bounds);
      NSString *display =
          state->password ? [@"" stringByPaddingToLength:[state->text length]
                                              withString:@"•"
                                         startingAtIndex:0]
                          : state->text;
      [display drawInRect:NSInsetRect(bounds, 4, 2)
           withAttributes:@{
             NSFontAttributeName : [NSFont systemFontOfSize:13],
             NSForegroundColorAttributeName : [NSColor blackColor]
           }];
      if (state->focused) {
        [[NSColor keyboardFocusIndicatorColor] setStroke];
        NSFrameRectWithWidth(NSInsetRect(bounds, 1, 1), 2);
      }
      [NSGraphicsContext restoreGraphicsState];
      return noErr;
    }
    if (kind == kEventControlHitTest) {
      ControlPartCode part = kControlEditTextPart;
      return SetEventParameter(event, kEventParamControlPart,
                               typeControlPartCode, sizeof(part), &part);
    }
    if (kind == kEventControlHit) {
      void *window = ((void *(*)(void *))sym("HIViewGetWindow"))(state->view);
      ((OSStatus (*)(void *, void *, ControlPartCode))sym("SetKeyboardFocus"))(
          window, state->view, kControlEditTextPart);
      return noErr;
    }
    if (kind == kEventControlSetFocusPart ||
        kind == kEventControlGetFocusPart) {
      ControlPartCode part = 0;
      if (kind == kEventControlSetFocusPart) {
        GetEventParameter(event, kEventParamControlPart, typeControlPartCode,
                          NULL, sizeof(part), NULL, &part);
        state->focused = part != 0;
        if (state->focused)
          state->selection = NSMakeRange(0, [state->text length]);
        invalidate(state);
      }
      part = state->focused ? kControlEditTextPart : 0;
      return SetEventParameter(event, kEventParamControlPart,
                               typeControlPartCode, sizeof(part), &part);
    }
    if (kind == kEventControlGetData || kind == kEventControlSetData) {
      OSType tag = 0;
      ByteCount capacity = 0;
      void *buffer = NULL;
      GetEventParameter(event, kEventParamControlDataTag, typeEnumeration, NULL,
                        sizeof(tag), NULL, &tag);
      GetEventParameter(event, kEventParamControlDataBufferSize, typeByteCount,
                        NULL, sizeof(capacity), NULL, &capacity);
      GetEventParameter(event, kEventParamControlDataBuffer, typePtr, NULL,
                        sizeof(buffer), NULL, &buffer);
      BOOL set = kind == kEventControlSetData;
      ByteCount size = 0;
      if (tag == 'cfst' || tag == 'pwcf' || tag == 'incf') {
        size = sizeof(CFStringRef);
        if (buffer && capacity >= size) {
          if (set) {
            NSString *value = *(NSString **)buffer;
            if (tag == 'incf')
              insert(state, value ?: @"");
            else {
              [state->text setString:value ?: @""];
              state->selection = NSMakeRange([state->text length], 0);
              invalidate(state);
            }
          } else
            *(CFStringRef *)buffer = (CFStringRef)[state->text copy];
        }
      } else if (tag == 'text' || tag == 'pass' || tag == 'ftxt') {
        if (set) {
          NSString *value =
              [[NSString alloc] initWithBytes:buffer
                                       length:capacity
                                     encoding:NSMacOSRomanStringEncoding];
          if (value) {
            [state->text setString:value];
            [value release];
            state->selection = NSMakeRange([state->text length], 0);
            invalidate(state);
          }
          size = capacity;
        } else {
          NSData *data =
              [state->text dataUsingEncoding:NSMacOSRomanStringEncoding
                        allowLossyConversion:YES];
          size = [data length];
          if (buffer && capacity >= size)
            memcpy(buffer, [data bytes], size);
        }
      } else if (tag == 'sele') {
        size = 4;
        if (buffer && capacity >= size) {
          int16_t *range = buffer;
          if (set) {
            NSUInteger start = MIN((uint16_t)range[0], [state->text length]),
                       end = MIN((uint16_t)range[1], [state->text length]);
            state->selection = NSMakeRange(start, MAX(start, end) - start);
          } else {
            range[0] = state->selection.location;
            range[1] = NSMaxRange(state->selection);
          }
        }
      } else if (tag == 'chrc') {
        size = 4;
        if (!set && buffer && capacity >= 4)
          *(uint32_t *)buffer = (uint32_t)[state->text length];
      } else if(tag=='fltr'||tag=='vali') {
        size=4;
        if(buffer&&capacity>=size){uint32_t *callback=tag=='fltr'?&state->key_filter:&state->validation;if(set)*callback=*(uint32_t *)buffer;else *(uint32_t *)buffer=*callback;}
      } else if (tag == 'lock') {
        size = 1;
        if (buffer && capacity >= 1) {
          if (set)
            state->locked = *(Boolean *)buffer;
          else
            *(Boolean *)buffer = state->locked;
        }
      } else
        return eventNotHandledErr;
      SetEventParameter(event, kEventParamControlDataBufferSize, typeByteCount,
                        sizeof(size), &size);
      return capacity < size ? errDataSizeMismatch : noErr;
    }
  }
  if (cls == kEventClassTextInput &&
      kind == kEventTextInputUnicodeForKeyEvent) {
    UniChar chars[1024];
    ByteCount size = 0;
    if (GetEventParameter(event, kEventParamTextInputSendText, typeUnicodeText,
                          NULL, sizeof(chars), &size, chars))
      return eventNotHandledErr;
    if(state->key_filter && size==2){
      uint32_t storage=compat_runtime32_allocate(8,1);if(!storage)return memFullErr;
      int16_t *values=(void *)(uintptr_t)storage;EventRef key=NULL;uint32_t code=0,modifiers=0;
      GetEventParameter(event,kEventParamTextInputSendKeyboardEvent,typeEventRef,NULL,sizeof(key),NULL,&key);
      if(key){GetEventParameter(key,kEventParamKeyCode,typeUInt32,NULL,sizeof(code),NULL,&code);GetEventParameter(key,kEventParamKeyModifiers,typeUInt32,NULL,sizeof(modifiers),NULL,&modifiers);}
      values[0]=(int16_t)code;values[1]=(int16_t)chars[0];values[2]=(int16_t)modifiers;
      uint32_t args[]={state->token,storage,storage+2,storage+4};
      uint32_t pass=(uint32_t)compat_runtime32_call(state->key_filter,args,4);
      chars[0]=(UniChar)values[1];compat_runtime32_deallocate(storage);
      if(!pass)return noErr;
    }
    if (size == 2 && (chars[0] == 8 || chars[0] == 127)) {
      if (!state->selection.length && state->selection.location) {
        --state->selection.location;
        state->selection.length = 1;
      }
      insert(state, @"");
    } else
      insert(state, [NSString stringWithCharacters:chars length:size / 2]);
    return noErr;
  }
  return eventNotHandledErr;
}
int32_t carbon_text_attach(void *view, uint32_t token, void *text,
                           int password) {
  LP32CarbonText *state = [[LP32CarbonText alloc] init];
  state->view = view;
  state->token = token;
  state->text = [((NSString *)text ?: @"") mutableCopy];
  state->password = password;
  if(!text_controls)text_controls=[[NSMutableDictionary alloc] init];
  [text_controls setObject:state forKey:[NSValue valueWithPointer:view]];
  ((OSStatus (*)(void *,uint64_t,CFStringRef,CFTypeRef))sym("HIObjectSetAuxiliaryAccessibilityAttribute"))(view,0,CFSTR("AXRole"),CFSTR("AXTextField"));
  EventTypeSpec events[] = {
      {kEventClassControl, kEventControlDispose},
      {kEventClassControl, kEventControlDraw},
      {kEventClassControl, kEventControlHitTest},
      {kEventClassControl, kEventControlHit},
      {kEventClassControl, kEventControlSetFocusPart},
      {kEventClassControl, kEventControlGetFocusPart},
      {kEventClassControl, kEventControlSetData},
      {kEventClassControl, kEventControlGetData},
      {kEventClassTextInput, kEventTextInputUnicodeForKeyEvent}};
  void *target = ((void *(*)(void *))sym("GetControlEventTarget"))(view);
  OSStatus status = InstallEventHandler(target, handle_text,
                                        sizeof(events) / sizeof(events[0]),
                                        events, state, NULL);
  if (status) {
    [state release];
    return status;
  }
  ((OSStatus (*)(void *, uint64_t, uint64_t))sym("HIViewChangeFeatures"))(
      view, kControlSupportsFocus | kControlSupportsDataAccess | kControlGetsFocusOnClick, 0);
  return noErr;
}

int carbon_text_is_control(void *view){return [text_controls objectForKey:[NSValue valueWithPointer:view]]!=nil;}
void *carbon_text_copy_value(void *view){LP32CarbonText *s=[text_controls objectForKey:[NSValue valueWithPointer:view]];return [s->text copy];}
void carbon_text_set_value(void *view,void *value){
 LP32CarbonText *s=[text_controls objectForKey:[NSValue valueWithPointer:view]];if(!s)return;
 s->selection=NSMakeRange(0,[s->text length]);insert(s,(NSString *)value);
 if(s->validation){uint32_t a[]={s->token};compat_runtime32_call(s->validation,a,1);}
}
