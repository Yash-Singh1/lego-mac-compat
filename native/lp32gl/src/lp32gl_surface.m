/* Window surfaces.  A context attached to an NSView renders into a
 * CAMetalLayer that Zink's swapchain presents to.  The layer lives in a
 * private subview that covers the target view, so AppKit never reorders it
 * with the target's own layers, and its geometry is entirely ours:
 *
 *   - normally the layer matches the view, in points (or in pixels when the
 *     view opted into wantsBestResolutionOpenGLSurface), like Apple's GL;
 *   - with kCGLCESurfaceBackingSize the layer is exactly the requested size
 *     in pixels and a transform scales it to the view, which is how CGL's
 *     fixed-size backing surfaces behave.
 *
 * Vulkan's surface extent is the layer's bounds times contentsScale, so the
 * size reported to Mesa is always read from the same numbers. */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>

#include "lp32gl_internal.h"

@interface LP32GLSurfaceView : NSView
@end

@implementation LP32GLSurfaceView
- (BOOL)isOpaque { return YES; }
- (NSView *)hitTest:(NSPoint)point { (void)point; return nil; }
- (BOOL)acceptsFirstResponder { return NO; }
- (BOOL)wantsUpdateLayer { return YES; }
- (void)updateLayer {}
@end

struct lp32gl_surface {
    volatile int32_t refcount;
    __unsafe_unretained NSView *view;   /* target; cleared when it goes away */
    LP32GLSurfaceView *host;            /* strong (manual retain) */
    CAMetalLayer *layer;                /* strong (manual retain) */
    id frame_observer;
    const void *config;
    struct lp32_dri_surface dri;
    void *drawable;                     /* Mesa dri_drawable */
    GLint backing_size[2];
    bool backing_size_enabled;
    bool best_resolution;
    GLint opacity;
    GLint order;
    int applied_interval;
};

static const void *kSurfacesKey = &kSurfacesKey;

static void on_main(void (^block)(void))
{
    if ([NSThread isMainThread]) block();
    else dispatch_sync(dispatch_get_main_queue(), block);
}

/* Main thread only.  Lays the metal layer out in the host view and publishes
   the pixel size Mesa will allocate at. */
static void layout(struct lp32gl_surface *surface)
{
    NSView *view = surface->view;
    LP32GLSurfaceView *host = surface->host;
    if (!view || !host) return;
    if (host.superview != view || !NSEqualRects(host.frame, view.bounds)) {
        host.frame = view.bounds;
    }
    CALayer *host_layer = host.layer;
    NSRect bounds = host.bounds;
    CGFloat window_scale = view.window ? view.window.backingScaleFactor : 1;
    CGFloat scale;
    CGSize size;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    if (surface->backing_size_enabled && surface->backing_size[0] > 0 && surface->backing_size[1] > 0) {
        scale = 1;
        size = CGSizeMake(surface->backing_size[0], surface->backing_size[1]);
        surface->layer.anchorPoint = CGPointMake(0.5, 0.5);
        surface->layer.bounds = CGRectMake(0, 0, size.width, size.height);
        surface->layer.position = CGPointMake(NSMidX(bounds), NSMidY(bounds));
        CGFloat sx = size.width > 0 ? bounds.size.width / size.width : 1;
        CGFloat sy = size.height > 0 ? bounds.size.height / size.height : 1;
        surface->layer.affineTransform = CGAffineTransformMakeScale(sx, sy);
    } else {
        scale = surface->best_resolution ? window_scale : 1;
        size = bounds.size;
        surface->layer.affineTransform = CGAffineTransformIdentity;
        surface->layer.anchorPoint = CGPointMake(0, 0);
        surface->layer.bounds = CGRectMake(0, 0, size.width, size.height);
        surface->layer.position = CGPointZero;
    }
    surface->layer.contentsScale = scale;
    surface->layer.opaque = surface->opacity != 0;
    /* The layer shows at the window's resolution even when rendered at the
       view's point size; bilinear matches how Apple's GL scaled surfaces. */
    surface->layer.magnificationFilter = kCAFilterLinear;
    if (surface->layer.superlayer != host_layer) [host_layer addSublayer:surface->layer];
    [CATransaction commit];
    int32_t width = (int32_t)lround(size.width * scale);
    int32_t height = (int32_t)lround(size.height * scale);
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (width != surface->dri.width || height != surface->dri.height) {
        surface->dri.width = width;
        surface->dri.height = height;
        if (surface->drawable) lp32_dri_invalidate_drawable(surface->drawable);
    }
}

static void attach_host(struct lp32gl_surface *surface)
{
    NSView *view = surface->view;
    if (!view) return;
    if (!surface->host) {
        surface->host = [[LP32GLSurfaceView alloc] initWithFrame:view.bounds];
        surface->host.wantsLayer = YES;
        surface->host.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
        surface->host.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        surface->host.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    }
    if (surface->host.superview != view) {
        /* Apple's GL surface draws above the view's own content. */
        [view addSubview:surface->host positioned:NSWindowAbove relativeTo:nil];
    }
    view.postsFrameChangedNotifications = YES;
    if (!surface->frame_observer) {
        struct lp32gl_surface *weak_surface = surface;
        surface->frame_observer = [[[NSNotificationCenter defaultCenter]
            addObserverForName:NSViewFrameDidChangeNotification object:view queue:nil
                    usingBlock:^(NSNotification *note) {
                        (void)note;
                        layout(weak_surface);
                    }] retain];
    }
    layout(surface);
}

struct lp32gl_surface *lp32gl_surface_for_view(void *view_pointer, const void *config)
{
    NSView *view = (__bridge NSView *)view_pointer;
    if (!view) return NULL;
    __block struct lp32gl_surface *result = NULL;
    on_main(^{
        NSMutableArray *surfaces = objc_getAssociatedObject(view, kSurfacesKey);
        for (NSValue *value in surfaces) {
            struct lp32gl_surface *candidate = value.pointerValue;
            if (candidate->config == config) {
                lp32gl_surface_retain(candidate);
                attach_host(candidate);
                result = candidate;
                return;
            }
        }
        struct lp32gl_surface *surface = calloc(1, sizeof *surface);
        surface->refcount = 1;
        surface->view = view;
        surface->config = config;
        surface->opacity = 1;
        surface->order = 1;
        surface->applied_interval = -1;
        surface->layer = [[CAMetalLayer alloc] init];
        surface->layer.device = MTLCreateSystemDefaultDevice();
        surface->layer.opaque = YES;
        surface->dri.metal_layer = (__bridge void *)surface->layer;
        surface->dri.swap_interval = 1;
        if (!surfaces) {
            surfaces = [NSMutableArray array];
            objc_setAssociatedObject(view, kSurfacesKey, surfaces, OBJC_ASSOCIATION_RETAIN);
        }
        [surfaces addObject:[NSValue valueWithPointer:surface]];
        attach_host(surface);
        surface->drawable = lp32_dri_create_window_drawable(config, &surface->dri);
        if (!surface->drawable) lp32gl_set_error("Mesa could not create a window drawable");
        result = surface;
    });
    return result;
}

void lp32gl_surface_retain(struct lp32gl_surface *surface)
{
    if (surface) __atomic_add_fetch(&surface->refcount, 1, __ATOMIC_RELAXED);
}

void lp32gl_surface_release(struct lp32gl_surface *surface)
{
    if (!surface || __atomic_sub_fetch(&surface->refcount, 1, __ATOMIC_ACQ_REL) != 0) return;
    void *drawable = surface->drawable;
    surface->drawable = NULL;
    lp32_dri_destroy_drawable(drawable);
    on_main(^{
        NSView *view = surface->view;
        if (view) {
            NSMutableArray *surfaces = objc_getAssociatedObject(view, kSurfacesKey);
            for (NSUInteger i = 0; i < surfaces.count; ++i)
                if ([surfaces[i] pointerValue] == surface) { [surfaces removeObjectAtIndex:i]; break; }
        }
        if (surface->frame_observer) {
            [[NSNotificationCenter defaultCenter] removeObserver:surface->frame_observer];
            [surface->frame_observer release];
        }
        [surface->layer removeFromSuperlayer];
        [surface->host removeFromSuperview];
        [surface->layer release];
        [surface->host release];
        free(surface);
    });
}

void *lp32gl_surface_drawable(struct lp32gl_surface *surface)
{
    return surface ? surface->drawable : NULL;
}

void *lp32gl_surface_view(struct lp32gl_surface *surface)
{
    return surface ? (__bridge void *)surface->view : NULL;
}

void lp32gl_surface_configure(struct lp32gl_surface *surface, const GLint *backing_size,
                              bool best_resolution, GLint opacity, GLint order)
{
    if (!surface) return;
    GLint width = backing_size ? backing_size[0] : 0, height = backing_size ? backing_size[1] : 0;
    void (^apply)(void) = ^{
        surface->backing_size_enabled = backing_size != NULL;
        surface->backing_size[0] = width;
        surface->backing_size[1] = height;
        surface->best_resolution = best_resolution;
        surface->opacity = opacity;
        surface->order = order;
        attach_host(surface);
    };
    /* Called from whichever thread updates the context.  Never block a
       render thread on the main thread here: the size Mesa sees is only
       published by layout(), so a late layout just delays the resize. */
    if ([NSThread isMainThread]) apply();
    else dispatch_async(dispatch_get_main_queue(), apply);
}

void lp32gl_surface_set_swap_interval(struct lp32gl_surface *surface, int interval)
{
    if (!surface) return;
    surface->dri.swap_interval = interval;
    if (surface->applied_interval != interval && surface->drawable) {
        lp32_dri_set_swap_interval(surface->drawable, interval);
        surface->applied_interval = interval;
    }
}
