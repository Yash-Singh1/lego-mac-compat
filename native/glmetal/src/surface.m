/* Window surfaces: a context attached to an NSView presents into a
 * CAMetalLayer hosted by a private subview covering the view (the layout
 * LP32GL uses; see lp32gl_surface.m), so AppKit never reorders it with the
 * view's own layers:
 *
 *   - normally the layer matches the view in points, or in pixels when the
 *     view opted into wantsBestResolutionOpenGLSurface, like Apple's GL;
 *   - with kCGLCESurfaceBackingSize the layer is exactly the requested size
 *     in pixels, scaled to the view, as CGL's fixed-size surfaces are.
 *
 * Layout runs on the main thread and publishes the pixel size; the render
 * thread resizes the default framebuffer to it when presenting. */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>

#include "surface.h"

@interface GLMSurfaceView : NSView
@end

@implementation GLMSurfaceView
- (BOOL)isOpaque { return YES; }
- (NSView *)hitTest:(NSPoint)point { (void)point; return nil; }
- (BOOL)acceptsFirstResponder { return NO; }
- (BOOL)wantsUpdateLayer { return YES; }
- (void)updateLayer {}
/* Apple's GLUT sends this to every subview of its GLUTView (a private
   NSView method GLUT relies on); the surface has none to collect. */
- (void)recursiveCollectViewsIntoSet:(NSMutableSet *)set { (void)set; }
@end

/* Owns the Objective-C side; the C struct points at it. */
@interface GLMSurfaceBox : NSObject {
@public
    struct glm_surface *surface;
    __weak NSView *view;
    GLMSurfaceView *host;
    CAMetalLayer *layer;
    id frame_observer;
}
@end
@implementation GLMSurfaceBox
@end

struct glm_surface {
    volatile int32_t refcount;
    void *box;                 /* GLMSurfaceBox, retained */
    volatile int32_t width, height; /* published pixel size */
    GLint backing_size[2];
    bool backing_size_enabled, best_resolution;
    GLint opacity;
    int swap_interval;
};

static const void *kSurfaceKey = &kSurfaceKey;

static GLMSurfaceBox *box_of(struct glm_surface *s) { return (__bridge GLMSurfaceBox *)s->box; }

static void on_main(void (^block)(void))
{
    if ([NSThread isMainThread]) block();
    else dispatch_sync(dispatch_get_main_queue(), block);
}

/* Main thread only. */
static void layout(struct glm_surface *s)
{
    GLMSurfaceBox *box = box_of(s);
    NSView *view = box->view;
    GLMSurfaceView *host = box->host;
    if (!view || !host) return;
    if (host.superview != view || !NSEqualRects(host.frame, view.bounds)) host.frame = view.bounds;
    CALayer *host_layer = host.layer;
    NSRect bounds = host.bounds;
    CGFloat window_scale = view.window ? view.window.backingScaleFactor : 1;
    CGFloat scale;
    CGSize size;
    CAMetalLayer *layer = box->layer;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    if (s->backing_size_enabled && s->backing_size[0] > 0 && s->backing_size[1] > 0) {
        scale = 1;
        size = CGSizeMake(s->backing_size[0], s->backing_size[1]);
        layer.anchorPoint = CGPointMake(0.5, 0.5);
        layer.bounds = CGRectMake(0, 0, size.width, size.height);
        layer.position = CGPointMake(NSMidX(bounds), NSMidY(bounds));
        CGFloat sx = size.width > 0 ? bounds.size.width / size.width : 1;
        CGFloat sy = size.height > 0 ? bounds.size.height / size.height : 1;
        layer.affineTransform = CGAffineTransformMakeScale(sx, sy);
    } else {
        scale = s->best_resolution ? window_scale : 1;
        size = bounds.size;
        layer.affineTransform = CGAffineTransformIdentity;
        layer.anchorPoint = CGPointMake(0, 0);
        layer.bounds = CGRectMake(0, 0, size.width, size.height);
        layer.position = CGPointZero;
    }
    layer.contentsScale = scale;
    layer.opaque = s->opacity != 0;
    layer.magnificationFilter = kCAFilterLinear;
    if (layer.superlayer != host_layer) [host_layer addSublayer:layer];
    int32_t width = (int32_t)lround(size.width * scale), height = (int32_t)lround(size.height * scale);
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    layer.drawableSize = CGSizeMake(width, height);
    [CATransaction commit];
    __atomic_store_n(&s->width, width, __ATOMIC_RELEASE);
    __atomic_store_n(&s->height, height, __ATOMIC_RELEASE);
}

static void attach_host(struct glm_surface *s)
{
    GLMSurfaceBox *box = box_of(s);
    NSView *view = box->view;
    if (!view) return;
    if (!box->host) {
        box->host = [[GLMSurfaceView alloc] initWithFrame:view.bounds];
        box->host.wantsLayer = YES;
        box->host.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
        box->host.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        box->host.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    }
    /* Apple's GL surface draws above the view's own content. */
    if (box->host.superview != view) [view addSubview:box->host positioned:NSWindowAbove relativeTo:nil];
    view.postsFrameChangedNotifications = YES;
    if (!box->frame_observer) {
        struct glm_surface *weak_surface = s;
        box->frame_observer = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSViewFrameDidChangeNotification object:view queue:nil
                    usingBlock:^(NSNotification *note) {
                        (void)note;
                        layout(weak_surface);
                    }];
    }
    layout(s);
}

struct glm_surface *glm_surface_for_view(void *view_pointer)
{
    NSView *view = (__bridge NSView *)view_pointer;
    if (!view) return NULL;
    __block struct glm_surface *result = NULL;
    on_main(^{
        NSValue *existing = objc_getAssociatedObject(view, kSurfaceKey);
        if (existing) {
            result = existing.pointerValue;
            glm_surface_retain(result);
            attach_host(result);
            return;
        }
        struct glm_surface *s = calloc(1, sizeof *s);
        s->refcount = 1;
        s->opacity = 1;
        s->swap_interval = 1;
        GLMSurfaceBox *box = [GLMSurfaceBox new];
        box->surface = s;
        box->view = view;
        box->layer = [CAMetalLayer layer];
        box->layer.device = MTLCreateSystemDefaultDevice();
        box->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        box->layer.framebufferOnly = YES;
        box->layer.opaque = YES;
        box->layer.displaySyncEnabled = YES;
        s->box = (__bridge_retained void *)box;
        objc_setAssociatedObject(view, kSurfaceKey, [NSValue valueWithPointer:s], OBJC_ASSOCIATION_RETAIN);
        attach_host(s);
        result = s;
    });
    return result;
}

void glm_surface_retain(struct glm_surface *s)
{
    if (s) __atomic_add_fetch(&s->refcount, 1, __ATOMIC_RELAXED);
}

void glm_surface_release(struct glm_surface *s)
{
    if (!s || __atomic_sub_fetch(&s->refcount, 1, __ATOMIC_ACQ_REL) != 0) return;
    on_main(^{
        /* glm_surface_for_view (main thread) may have taken the view's
           surface again while this release waited for the main thread. */
        if (__atomic_load_n(&s->refcount, __ATOMIC_ACQUIRE) != 0) return;
        GLMSurfaceBox *box = (__bridge_transfer GLMSurfaceBox *)s->box;
        NSView *view = box->view;
        if (view) objc_setAssociatedObject(view, kSurfaceKey, nil, OBJC_ASSOCIATION_RETAIN);
        if (box->frame_observer) [[NSNotificationCenter defaultCenter] removeObserver:box->frame_observer];
        [box->layer removeFromSuperlayer];
        [box->host removeFromSuperview];
        free(s);
    });
}

void *glm_surface_view(struct glm_surface *s) { return s ? (__bridge void *)box_of(s)->view : NULL; }
void *glm_surface_layer(struct glm_surface *s) { return s ? (__bridge void *)box_of(s)->layer : NULL; }

void glm_surface_size(struct glm_surface *s, GLsizei *width, GLsizei *height)
{
    *width = s ? __atomic_load_n(&s->width, __ATOMIC_ACQUIRE) : 0;
    *height = s ? __atomic_load_n(&s->height, __ATOMIC_ACQUIRE) : 0;
}

void glm_surface_configure(struct glm_surface *s, const GLint *backing_size, GLint opacity)
{
    if (!s) return;
    GLint width = backing_size ? backing_size[0] : 0, height = backing_size ? backing_size[1] : 0;
    void (^apply)(void) = ^{
        NSView *view = box_of(s)->view;
        s->backing_size_enabled = backing_size != NULL;
        s->backing_size[0] = width;
        s->backing_size[1] = height;
        s->best_resolution = view && view.wantsBestResolutionOpenGLSurface;
        s->opacity = opacity;
        attach_host(s);
    };
    /* Never block a render thread on the main thread: a late layout only
       delays the resize. The first configuration runs synchronously so a
       fresh surface has a size. */
    if ([NSThread isMainThread] || !__atomic_load_n(&s->width, __ATOMIC_ACQUIRE)) {
        on_main(apply);
        return;
    }
    /* The surface must outlive the queued block. */
    glm_surface_retain(s);
    dispatch_async(dispatch_get_main_queue(), ^{
        apply();
        glm_surface_release(s);
    });
}

void glm_surface_set_swap_interval(struct glm_surface *s, int interval)
{
    if (!s || s->swap_interval == interval) return;
    s->swap_interval = interval;
    CAMetalLayer *layer = box_of(s)->layer;
    dispatch_async(dispatch_get_main_queue(), ^{ layer.displaySyncEnabled = interval != 0; });
}
