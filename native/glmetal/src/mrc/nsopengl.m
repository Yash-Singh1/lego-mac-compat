/* NSOpenGLContext and NSOpenGLPixelFormat on GLMetal (ported from LP32GL's
 * NSOpenGL layer).
 *
 * The replacements are subclasses, so isKindOfClass: checks, static types
 * and NSOpenGLView keep working, but every public method is reimplemented
 * on GLMetal's CGL and the AppKit superclass state is never initialized.
 * glmetal_install_appkit_overrides() redirects +allocWithZone: on the two
 * AppKit classes (not their other subclasses) to these, and routes the
 * class-level current-context calls through GLMetal.
 *
 * Built with manual reference counting: -dealloc must skip AppKit's. */
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <pthread.h>

#define GLM_EXPORT __attribute__((visibility("default")))

int GLMCGLSetView(CGLContextObj ctx, void *view);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#pragma clang diagnostic ignored "-Wobjc-missing-super-calls"
#pragma clang diagnostic ignored "-Wobjc-designated-initializers"

/* AppKit's own -dealloc would tear down state these objects never set up. */
static void dealloc_as_nsobject(id self)
{
    static IMP imp;
    if (!imp) imp = class_getMethodImplementation([NSObject class], @selector(dealloc));
    ((void (*)(id, SEL))imp)(self, @selector(dealloc));
}

@interface GLMOpenGLPixelFormat : NSOpenGLPixelFormat {
    CGLPixelFormatObj _pix;
}
- (instancetype)glm_initWithPixelFormat:(CGLPixelFormatObj)pix;
@end

@implementation GLMOpenGLPixelFormat

- (instancetype)init
{
    CGLPixelFormatAttribute attributes[] = {kCGLPFADoubleBuffer, kCGLPFAAccelerated, 0};
    CGLPixelFormatObj pix = NULL;
    GLint count = 0;
    CGLChoosePixelFormat(attributes, &pix, &count);
    return [self glm_initWithPixelFormat:pix];
}

- (instancetype)initWithAttributes:(const NSOpenGLPixelFormatAttribute *)attribs
{
    CGLPixelFormatAttribute converted[256];
    size_t used = 0;
    for (const NSOpenGLPixelFormatAttribute *a = attribs; a && *a && used + 2 < 256;) {
        CGLPixelFormatAttribute attribute = (CGLPixelFormatAttribute)*a++;
        converted[used++] = attribute;
        /* A zero value is valid. Only a zero in the attribute position ends
           the list. */
        switch (attribute) {
        case kCGLPFAAuxBuffers: case kCGLPFAColorSize:
        case kCGLPFAAlphaSize: case kCGLPFADepthSize:
        case kCGLPFAStencilSize: case kCGLPFAAccumSize:
        case kCGLPFASampleBuffers: case kCGLPFASamples:
        case kCGLPFARendererID: case kCGLPFADisplayMask:
        case kCGLPFAOpenGLProfile: case kCGLPFAVirtualScreenCount:
            converted[used++] = (CGLPixelFormatAttribute)*a++;
            break;
        default:
            break;
        }
    }
    converted[used] = 0;
    CGLPixelFormatObj pix = NULL;
    GLint count = 0;
    CGLChoosePixelFormat(converted, &pix, &count);
    return [self glm_initWithPixelFormat:pix];
}

- (instancetype)initWithCGLPixelFormatObj:(CGLPixelFormatObj)format
{
    return [self glm_initWithPixelFormat:CGLRetainPixelFormat(format)];
}

- (instancetype)initWithData:(NSData *)attribs
{
    (void)attribs;
    [self release];
    return nil;
}

- (instancetype)glm_initWithPixelFormat:(CGLPixelFormatObj)pix
{
    if (!pix) {
        [self release];
        return nil;
    }
    _pix = pix;
    return self;
}

- (void)dealloc
{
    CGLReleasePixelFormat(_pix);
    dealloc_as_nsobject(self);
}

- (void)getValues:(GLint *)vals forAttribute:(NSOpenGLPixelFormatAttribute)attrib
 forVirtualScreen:(GLint)screen
{
    CGLDescribePixelFormat(_pix, screen, (CGLPixelFormatAttribute)attrib, vals);
}

- (GLint)numberOfVirtualScreens { return 1; }
- (CGLPixelFormatObj)CGLPixelFormatObj { return _pix; }
- (NSData *)attributes { return nil; }
- (void)setAttributes:(NSData *)attribs { (void)attribs; }
- (id)copyWithZone:(NSZone *)zone { (void)zone; return [self retain]; }

@end

@interface GLMOpenGLContext : NSOpenGLContext {
    CGLContextObj _ctx;
    NSView *_view; /* not retained, like AppKit */
    GLMOpenGLPixelFormat *_format;
}
@end

static __thread GLMOpenGLContext *tls_current_context;
static NSMapTable *contexts_by_cgl; /* CGLContextObj -> GLMOpenGLContext, weak */
static pthread_mutex_t contexts_lock = PTHREAD_MUTEX_INITIALIZER;

@implementation GLMOpenGLContext

- (instancetype)glm_initWithCGLContext:(CGLContextObj)ctx
{
    if (!ctx) {
        [self release];
        return nil;
    }
    _ctx = ctx;
    pthread_mutex_lock(&contexts_lock);
    if (!contexts_by_cgl)
        contexts_by_cgl = [[NSMapTable alloc] initWithKeyOptions:NSPointerFunctionsOpaqueMemory |
                                                                 NSPointerFunctionsOpaquePersonality
                                                    valueOptions:NSPointerFunctionsOpaqueMemory |
                                                                 NSPointerFunctionsOpaquePersonality
                                                        capacity:8];
    [contexts_by_cgl setObject:self forKey:(id)ctx];
    pthread_mutex_unlock(&contexts_lock);
    return self;
}

- (instancetype)initWithFormat:(NSOpenGLPixelFormat *)format shareContext:(NSOpenGLContext *)share
{
    CGLContextObj ctx = NULL;
    CGLError error = CGLCreateContext([format CGLPixelFormatObj], share ? [share CGLContextObj] : NULL, &ctx);
    if (error && getenv("GLMETAL_DEBUG"))
        fprintf(stderr, "glmetal: -[NSOpenGLContext initWithFormat:shareContext:] failed: CGL error %d (format %s %p, share %s %p)\n",
                error, object_getClassName(format), (void *)[format CGLPixelFormatObj], share ? object_getClassName(share) : "nil",
                share ? (void *)[share CGLContextObj] : NULL);
    return [self glm_initWithCGLContext:ctx];
}

- (instancetype)initWithCGLContextObj:(CGLContextObj)context
{
    return [self glm_initWithCGLContext:CGLRetainContext(context)];
}

- (void)dealloc
{
    pthread_mutex_lock(&contexts_lock);
    if ([contexts_by_cgl objectForKey:(id)_ctx] == self) [contexts_by_cgl removeObjectForKey:(id)_ctx];
    pthread_mutex_unlock(&contexts_lock);
    if (tls_current_context == self) tls_current_context = nil;
    [_format release];
    CGLReleaseContext(_ctx);
    dealloc_as_nsobject(self);
}

- (CGLContextObj)CGLContextObj { return _ctx; }

- (NSOpenGLPixelFormat *)pixelFormat
{
    if (!_format)
        _format = [[GLMOpenGLPixelFormat alloc] initWithCGLPixelFormatObj:CGLGetPixelFormat(_ctx)];
    return _format;
}

- (void)setView:(NSView *)view
{
    _view = view;
    GLMCGLSetView(_ctx, view);
}

- (NSView *)view { return _view; }

- (void)clearDrawable
{
    _view = nil;
    CGLClearDrawable(_ctx);
}

- (void)update { CGLUpdateContext(_ctx); }
- (void)flushBuffer { CGLFlushDrawable(_ctx); }

- (void)makeCurrentContext
{
    if (CGLSetCurrentContext(_ctx) == kCGLNoError) tls_current_context = self;
}

+ (NSOpenGLContext *)currentContext
{
    CGLContextObj current = CGLGetCurrentContext();
    if (!current) return nil;
    if (tls_current_context && tls_current_context->_ctx == current) return tls_current_context;
    pthread_mutex_lock(&contexts_lock);
    GLMOpenGLContext *context = [contexts_by_cgl objectForKey:(id)current];
    pthread_mutex_unlock(&contexts_lock);
    return context;
}

+ (void)clearCurrentContext
{
    CGLSetCurrentContext(NULL);
    tls_current_context = nil;
}

- (void)setValues:(const GLint *)vals forParameter:(NSOpenGLContextParameter)param
{
    CGLSetParameter(_ctx, (CGLContextParameter)param, vals);
}

- (void)getValues:(GLint *)vals forParameter:(NSOpenGLContextParameter)param
{
    CGLGetParameter(_ctx, (CGLContextParameter)param, vals);
}

- (GLint)currentVirtualScreen { return 0; }
- (void)setCurrentVirtualScreen:(GLint)screen { CGLSetVirtualScreen(_ctx, screen); }

- (void)copyAttributesFromContext:(NSOpenGLContext *)context withMask:(GLbitfield)mask
{
    CGLCopyContext([context CGLContextObj], _ctx, mask);
}

- (void)setFullScreen {}
- (void)setOffScreen:(void *)baseaddr width:(GLsizei)width height:(GLsizei)height rowbytes:(GLint)rowbytes
{
    CGLSetOffScreen(_ctx, width, height, rowbytes, baseaddr);
}

- (void)setPixelBuffer:(NSOpenGLPixelBuffer *)pixelBuffer cubeMapFace:(GLenum)face
           mipMapLevel:(GLint)level currentVirtualScreen:(GLint)screen
{
    (void)pixelBuffer; (void)face; (void)level; (void)screen;
}
- (NSOpenGLPixelBuffer *)pixelBuffer { return nil; }
- (GLenum)pixelBufferCubeMapFace { return 0; }
- (GLint)pixelBufferMipMapLevel { return 0; }
- (void)setTextureImageToPixelBuffer:(NSOpenGLPixelBuffer *)pixelBuffer colorBuffer:(GLenum)source
{
    (void)pixelBuffer; (void)source;
}
- (void)createTexture:(GLenum)target fromView:(NSView *)view internalFormat:(GLenum)format
{
    (void)target; (void)view; (void)format;
}

@end

/* ---- redirection ------------------------------------------------------- */

static IMP original_context_alloc, original_format_alloc;
static IMP original_current_context, original_clear_current_context;

static id context_alloc(id self, SEL _cmd, NSZone *zone)
{
    if (self == [NSOpenGLContext class]) return [GLMOpenGLContext allocWithZone:zone];
    return ((id (*)(id, SEL, NSZone *))original_context_alloc)(self, _cmd, zone);
}

static id format_alloc(id self, SEL _cmd, NSZone *zone)
{
    if (self == [NSOpenGLPixelFormat class]) return [GLMOpenGLPixelFormat allocWithZone:zone];
    return ((id (*)(id, SEL, NSZone *))original_format_alloc)(self, _cmd, zone);
}

static id current_context(id self, SEL _cmd)
{
    NSOpenGLContext *context = [GLMOpenGLContext currentContext];
    if (context) return context;
    return ((id (*)(id, SEL))original_current_context)(self, _cmd);
}

static void clear_current_context(id self, SEL _cmd)
{
    [GLMOpenGLContext clearCurrentContext];
    ((void (*)(id, SEL))original_clear_current_context)(self, _cmd);
}

static IMP replace_class_method(Class cls, SEL selector, IMP replacement)
{
    Method method = class_getClassMethod(cls, selector);
    Class meta = object_getClass(cls);
    /* Add on the metaclass itself so inherited NSObject methods are not
       replaced for every class. */
    IMP original = method_getImplementation(method);
    if (!class_addMethod(meta, selector, replacement, method_getTypeEncoding(method)))
        original = method_setImplementation(class_getClassMethod(cls, selector), replacement);
    return original;
}

GLM_EXPORT void glmetal_install_appkit_overrides(void)
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        Class context = [NSOpenGLContext class];
        Class format = [NSOpenGLPixelFormat class];
        original_context_alloc = replace_class_method(context, @selector(allocWithZone:), (IMP)context_alloc);
        original_format_alloc = replace_class_method(format, @selector(allocWithZone:), (IMP)format_alloc);
        original_current_context =
            replace_class_method(context, @selector(currentContext), (IMP)current_context);
        original_clear_current_context =
            replace_class_method(context, @selector(clearCurrentContext), (IMP)clear_current_context);
    });
}

#pragma clang diagnostic pop
