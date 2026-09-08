#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>
#include "carbon_text.h"
#include "carbon_native.h"
#include "objc_bridge.h"
#include "compat_runtime.h"

/* MLTE/HITextView disappeared from LP64. Draw attributed text directly into
   its Carbon HIView: NSWindow's Carbon wrapper crashes in modern AppKit. */
@interface LP32CarbonText : NSObject {
@public void *view, *window;
    NSScrollView *scroll;
    NSTextView *text;
    bool disposed;
}
@end
@implementation LP32CarbonText
- (void)dealloc { [scroll removeFromSuperview]; [scroll release]; [text release]; [super dealloc]; }
@end
static NSMutableArray *text_views;
static id object(uint32_t handle) { return objc_bridge32_host_object(handle); }
static LP32CarbonText *text_for_view(void *view) {
    for (LP32CarbonText *text in text_views) if (!text->disposed && text->view == view) return text;
    return nil;
}
static CGRect rect_from_guest(const float *rect) { return rect ? CGRectMake(rect[0], rect[1], rect[2], rect[3]) : CGRectMake(0, 0, 100, 100); }

static OSStatus draw_text(EventHandlerCallRef call, EventRef event, void *opaque) {
    (void)call;
    LP32CarbonText *entry = opaque;
    if (entry->disposed) return eventNotHandledErr;
    CGContextRef context = NULL;
    OSStatus status = GetEventParameter(event, kEventParamCGContextRef, typeCGContextRef, NULL, sizeof(context), NULL, &context);
    if (status || !context) return eventNotHandledErr;
    CGRect bounds = {0};
    ((OSStatus(*)(void *, CGRect *))carbon_native32_symbol("HIViewGetBounds"))(entry->view, &bounds);
    CGContextSaveGState(context);
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithCGContext:context flipped:YES]];
    [[entry->text textStorage] drawWithRect:CGRectMake(0, 0, bounds.size.width, MAX(bounds.size.height, [entry->text bounds].size.height))
        options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingUsesFontLeading];
    [NSGraphicsContext restoreGraphicsState];
    CGContextRestoreGState(context);
    return noErr;
}

void carbon_text32_layout(void) {
    void *(*window_for_view)(void *) = carbon_native32_symbol("HIViewGetWindow");
    void *(*root_for_window)(void *) = carbon_native32_symbol("HIViewGetRoot");
    OSStatus (*bounds)(void *, CGRect *) = carbon_native32_symbol("HIViewGetBounds");
    OSStatus (*convert)(CGRect *, void *, void *) = carbon_native32_symbol("HIViewConvertRect");
    OSStatus (*find)(void *, uint64_t, void **) = carbon_native32_symbol("HIViewFindByID");
    const uint64_t *content_id = carbon_native32_symbol("kHIViewWindowContentID");
    for (LP32CarbonText *entry in text_views) {
        if (entry->disposed) continue;
        void *window = window_for_view(entry->view);
        if (!window) continue;
        void *root = root_for_window(window), *content = NULL;
        if (content_id) find(root, *content_id, &content);
        if (!content) content = root;
        CGRect rect = {0};
        if (bounds(entry->view, &rect) || convert(&rect, entry->view, content)) continue;
        entry->window = window;
        CGFloat width = MAX(1, rect.size.width);
        [entry->text setFrameSize:NSMakeSize(width, MAX([entry->text frame].size.height, rect.size.height))];
        [[entry->text textContainer] setContainerSize:NSMakeSize(width, CGFLOAT_MAX)];
        [entry->text sizeToFit];
    }
}
void carbon_text32_remove_window(void *window) {
    for (LP32CarbonText *entry in text_views) if (entry->window == window) {
        entry->disposed = true; [entry->scroll removeFromSuperview];
    }
}

int carbon_text32_dispatch(const char *name, const uint32_t *a, uint64_t *result) {
#define IS(s) (!strcmp(name, "_" s))
#define PTR(i) ((void *)(uintptr_t)a[i])
#define RETURN(v) do { *result = (uint64_t)(v); return 1; } while (0)
    if (IS("HITextViewCreate")) {
        if (!a[3]) RETURN((uint32_t)paramErr);
        OSStatus (*create)(CFStringRef, void *, void **) = carbon_native32_symbol("HIObjectCreate");
        if (!create) RETURN((uint32_t)unimpErr);
        LP32CarbonText *entry = [[[LP32CarbonText alloc] init] autorelease];
        OSStatus status = create(CFSTR("com.apple.hiview"), NULL, &entry->view);
        if (status) RETURN((uint32_t)status);
        CGRect rect = rect_from_guest(PTR(0));
        ((OSStatus(*)(void *, const CGRect *))carbon_native32_symbol("HIViewSetFrame"))(entry->view, &rect);
        entry->scroll = [[NSScrollView alloc] initWithFrame:rect];
        [entry->scroll setHasVerticalScroller:YES]; [entry->scroll setAutohidesScrollers:YES];
        entry->text = [[NSTextView alloc] initWithFrame:CGRectMake(0, 0, rect.size.width, rect.size.height)];
        [entry->text setEditable:NO]; [entry->text setSelectable:YES]; [entry->text setRichText:YES];
        [entry->text setVerticallyResizable:YES]; [entry->text setHorizontallyResizable:NO];
        [entry->text setMinSize:NSMakeSize(0, 0)]; [entry->text setMaxSize:NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX)];
        [[entry->text textContainer] setWidthTracksTextView:YES];
        [entry->text setFont:[NSFont systemFontOfSize:13]];
        [entry->scroll setDocumentView:entry->text];
        if (!text_views) text_views = [[NSMutableArray alloc] init];
        [text_views addObject:entry];
        EventTypeSpec draw_event = {kEventClassControl, kEventControlDraw};
        void *(*target)(void *) = carbon_native32_symbol("GetControlEventTarget");
        OSStatus (*install)(void *, EventHandlerUPP, ItemCount, const EventTypeSpec *, void *, EventHandlerRef *) = carbon_native32_symbol("InstallEventHandler");
        status = install(target(entry->view), draw_text, 1, &draw_event, entry, NULL);
        if (status) { entry->disposed = true; RETURN((uint32_t)status); }
        *(uint32_t *)PTR(3) = carbon_native32_owned_cf(entry->view); RETURN(0);
    }
    if (IS("HITextViewGetTXNObject")) RETURN(objc_bridge32_guest_object(text_for_view(carbon_native32_pointer(a[0]))));
    if (IS("TXNSetDataFromCFURLRef")) {
        LP32CarbonText *entry = object(a[0]); NSURL *url = object(a[1]);
        if (![entry isKindOfClass:[LP32CarbonText class]] || ![url isFileURL]) RETURN((uint32_t)paramErr);
        NSError *error = nil;
        NSAttributedString *text = [[[NSAttributedString alloc] initWithURL:url options:@{} documentAttributes:NULL error:&error] autorelease];
        if (!text) { fprintf(stderr, "compat32: cannot load dialog text: %s\n", [[error localizedDescription] UTF8String]); RETURN((uint32_t)fnfErr); }
        NSUInteger length = [[entry->text textStorage] length];
        NSUInteger start = MIN((NSUInteger)a[2], length), end = a[3] == UINT32_MAX ? length : MIN((NSUInteger)a[3], length);
        if (end < start) RETURN((uint32_t)paramErr);
        [[entry->text textStorage] replaceCharactersInRange:NSMakeRange(start, end - start) withAttributedString:text];
        [entry->text sizeToFit]; carbon_text32_layout();
        fprintf(stderr, "compat32: dialog text loaded %s (%lu characters)\n", [[[url path] lastPathComponent] UTF8String], (unsigned long)[text length]);
        RETURN(0);
    }
    if (IS("TXNGetHIRect")) {
        LP32CarbonText *entry = object(a[0]); if (![entry isKindOfClass:[LP32CarbonText class]] || !a[2]) RETURN((uint32_t)paramErr);
        NSRect rect = [entry->text bounds]; float *out = PTR(2);
        out[0] = rect.origin.x; out[1] = rect.origin.y; out[2] = rect.size.width; out[3] = rect.size.height; RETURN(0);
    }
    if (IS("TXNSetHIRectBounds")) {
        LP32CarbonText *entry = object(a[0]); if (![entry isKindOfClass:[LP32CarbonText class]]) RETURN((uint32_t)paramErr);
        if (a[1]) [entry->scroll setFrame:rect_from_guest(PTR(1))];
        if (a[2]) [entry->text setFrame:rect_from_guest(PTR(2))];
        [entry->text setNeedsDisplay:a[3] != 0]; RETURN(0);
    }
    if (IS("TXNDeleteObject")) {
        LP32CarbonText *entry = object(a[0]);
        if ([entry isKindOfClass:[LP32CarbonText class]]) { entry->disposed = true; [entry->scroll removeFromSuperview]; }
        RETURN(0);
    }
    return 0;
#undef IS
#undef PTR
#undef RETURN
}
