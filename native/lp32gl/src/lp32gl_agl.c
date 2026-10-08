/* AGL on LP32GL's CGL.  AGL objects are the CGL objects themselves, the
 * way Apple's AGL wraps CGL (aglGetCGLContext hands back the same context).
 * AGL's attribute, renderer-property and parameter values are CGL's.
 *
 * Carbon drawables (aglSetDrawable/aglSetWindowRef/aglSetHIViewRef) have no
 * 64-bit Cocoa equivalent to resolve to; hosts that emulate Carbon attach a
 * view with LP32CGLSetView (see carbon_ui.m) and these calls fail with
 * AGL_BAD_DRAWABLE like Apple's AGL does for a window it cannot use. */
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <string.h>

#include "lp32gl_internal.h"

#define EXPORT LP32GL_EXPORT

typedef CGLPixelFormatObj AGLPixelFormat;
typedef CGLContextObj AGLContext;
typedef CGLRendererInfoObj AGLRendererInfo;
typedef void *AGLDevice;
typedef void *AGLDrawable;
typedef void *AGLPbuffer;

enum {
    AGL_NONE = 0, AGL_ALL_RENDERERS = 1, AGL_BUFFER_SIZE = 2, AGL_LEVEL = 3, AGL_RGBA = 4,
    AGL_DOUBLEBUFFER = 5, AGL_STEREO = 6, AGL_AUX_BUFFERS = 7, AGL_RED_SIZE = 8, AGL_GREEN_SIZE = 9,
    AGL_BLUE_SIZE = 10, AGL_ALPHA_SIZE = 11, AGL_DEPTH_SIZE = 12, AGL_STENCIL_SIZE = 13,
    AGL_ACCUM_RED_SIZE = 14, AGL_ACCUM_GREEN_SIZE = 15, AGL_ACCUM_BLUE_SIZE = 16,
    AGL_ACCUM_ALPHA_SIZE = 17, AGL_PIXEL_SIZE = 50, AGL_MINIMUM_POLICY = 51,
    AGL_MAXIMUM_POLICY = 52, AGL_OFFSCREEN = 53, AGL_FULLSCREEN = 54,
    AGL_SAMPLE_BUFFERS_ARB = 55, AGL_SAMPLES_ARB = 56, AGL_AUX_DEPTH_STENCIL = 57,
    AGL_COLOR_FLOAT = 58, AGL_MULTISAMPLE = 59, AGL_SUPERSAMPLE = 60, AGL_SAMPLE_ALPHA = 61,
    AGL_RENDERER_ID = 70, AGL_SINGLE_RENDERER = 71, AGL_NO_RECOVERY = 72, AGL_ACCELERATED = 73,
    AGL_CLOSEST_POLICY = 74, AGL_ROBUST = 75, AGL_BACKING_STORE = 76, AGL_MP_SAFE = 78,
    AGL_WINDOW = 80, AGL_MULTISCREEN = 81, AGL_VIRTUAL_SCREEN = 82, AGL_COMPLIANT = 83,
    AGL_DISPLAY_MASK = 84, AGL_PBUFFER = 90, AGL_REMOTE_PBUFFER = 91,
    AGL_ALLOW_OFFLINE_RENDERERS = 96,
};
enum {
    AGL_SWAP_RECT = 200, AGL_BUFFER_RECT = 202, AGL_SWAP_LIMIT = 203, AGL_COLORMAP_TRACKING = 210,
    AGL_COLORMAP_ENTRY = 212, AGL_RASTERIZATION = 220, AGL_SWAP_INTERVAL = 222,
    AGL_STATE_VALIDATION = 230, AGL_BUFFER_NAME = 231, AGL_ORDER_CONTEXT_TO_FRONT = 232,
    AGL_CONTEXT_SURFACE_ID = 233, AGL_CONTEXT_DISPLAY_ID = 234, AGL_SURFACE_ORDER = 235,
    AGL_SURFACE_OPACITY = 236, AGL_CLIP_REGION = 254, AGL_FS_CAPTURE_SINGLE = 255,
    AGL_SURFACE_BACKING_SIZE = 304, AGL_ENABLE_SURFACE_BACKING_SIZE = 305,
    AGL_SURFACE_VOLATILE = 306,
};
enum {
    AGL_NO_ERROR = 0, AGL_BAD_ATTRIBUTE = 10000, AGL_BAD_PROPERTY = 10001,
    AGL_BAD_PIXELFMT = 10002, AGL_BAD_RENDINFO = 10003, AGL_BAD_CONTEXT = 10004,
    AGL_BAD_DRAWABLE = 10005, AGL_BAD_GDEV = 10006, AGL_BAD_STATE = 10007, AGL_BAD_VALUE = 10008,
    AGL_BAD_MATCH = 10009, AGL_BAD_ENUM = 10010, AGL_BAD_OFFSCREEN = 10011,
    AGL_BAD_FULLSCREEN = 10012, AGL_BAD_WINDOW = 10013, AGL_BAD_POINTER = 10014,
    AGL_BAD_MODULE = 10015, AGL_BAD_ALLOC = 10016, AGL_BAD_CONNECTION = 10017,
};

static __thread GLenum agl_error;

static GLboolean result(CGLError error)
{
    agl_error = (GLenum)error;
    return error == kCGLNoError;
}

EXPORT AGLPixelFormat aglCreatePixelFormat(const GLint *attribs)
{
    /* AGL color is per channel; CGL's is the sum.  Everything else shares
       CGL's numbering; AGL_RGBA and AGL_LEVEL have no CGL counterpart. */
    CGLPixelFormatAttribute cgl[128];
    size_t used = 0;
    GLint red = 0, green = 0, blue = 0, buffer = 0;
    for (const GLint *a = attribs; a && *a && used + 3 < 128; ++a) {
        switch (*a) {
        case AGL_RGBA: case AGL_ALL_RENDERERS: case AGL_FULLSCREEN: case AGL_WINDOW:
        case AGL_SINGLE_RENDERER: case AGL_OFFSCREEN:
            break;
        case AGL_LEVEL: ++a; break;
        case AGL_RED_SIZE: red = *++a; break;
        case AGL_GREEN_SIZE: green = *++a; break;
        case AGL_BLUE_SIZE: blue = *++a; break;
        case AGL_BUFFER_SIZE: case AGL_PIXEL_SIZE: buffer = *++a; break;
        case AGL_ACCUM_RED_SIZE: case AGL_ACCUM_GREEN_SIZE: case AGL_ACCUM_BLUE_SIZE:
        case AGL_ACCUM_ALPHA_SIZE:
            ++a;
            break;
        case AGL_AUX_BUFFERS: case AGL_ALPHA_SIZE: case AGL_DEPTH_SIZE: case AGL_STENCIL_SIZE:
        case AGL_SAMPLE_BUFFERS_ARB: case AGL_SAMPLES_ARB: case AGL_RENDERER_ID:
        case AGL_VIRTUAL_SCREEN: case AGL_DISPLAY_MASK:
            cgl[used++] = (CGLPixelFormatAttribute)a[0];
            cgl[used++] = (CGLPixelFormatAttribute)a[1];
            ++a;
            break;
        default:
            cgl[used++] = (CGLPixelFormatAttribute)*a;
            break;
        }
    }
    GLint color = red + green + blue;
    if (buffer > color) color = buffer;
    if (color) {
        cgl[used++] = kCGLPFAColorSize;
        cgl[used++] = (CGLPixelFormatAttribute)color;
    }
    cgl[used] = 0;
    CGLPixelFormatObj pix = NULL;
    GLint count = 0;
    CGLError error = lp32gl_choose(cgl, &pix, &count);
    agl_error = (GLenum)error;
    return pix;
}

EXPORT AGLPixelFormat aglChoosePixelFormat(const AGLDevice *gdevs, GLint ndev, const GLint *attribs)
{
    (void)gdevs; (void)ndev;
    return aglCreatePixelFormat(attribs);
}

EXPORT void aglDestroyPixelFormat(AGLPixelFormat pix) { result(CGLDestroyPixelFormat(pix)); }
EXPORT AGLPixelFormat aglNextPixelFormat(AGLPixelFormat pix) { (void)pix; return NULL; }

EXPORT GLboolean aglDescribePixelFormat(AGLPixelFormat pix, GLint attrib, GLint *value)
{
    if (!value) return result(kCGLBadAddress);
    switch (attrib) {
    case AGL_RGBA: *value = 1; return result(kCGLNoError);
    case AGL_LEVEL: *value = 0; return result(kCGLNoError);
    case AGL_RED_SIZE: case AGL_GREEN_SIZE: case AGL_BLUE_SIZE: {
        GLint alpha = 0, color = 0;
        CGLDescribePixelFormat(pix, 0, kCGLPFAAlphaSize, &alpha);
        CGLError error = CGLDescribePixelFormat(pix, 0, kCGLPFAColorSize, &color);
        *value = (color - alpha) / 3;
        return result(error);
    }
    case AGL_BUFFER_SIZE: case AGL_PIXEL_SIZE:
        return result(CGLDescribePixelFormat(pix, 0, kCGLPFAColorSize, value));
    case AGL_ACCUM_RED_SIZE: case AGL_ACCUM_GREEN_SIZE: case AGL_ACCUM_BLUE_SIZE:
    case AGL_ACCUM_ALPHA_SIZE:
        *value = 0;
        return result(kCGLNoError);
    default:
        return result(CGLDescribePixelFormat(pix, 0, (CGLPixelFormatAttribute)attrib, value));
    }
}

EXPORT AGLDevice *aglDevicesOfPixelFormat(AGLPixelFormat pix, GLint *ndevs)
{
    (void)pix;
    if (ndevs) *ndevs = 0;
    return NULL;
}

EXPORT GLboolean aglGetCGLPixelFormat(AGLPixelFormat pix, void **cgl_pix)
{
    if (!cgl_pix) return result(kCGLBadAddress);
    *cgl_pix = pix;
    return result(pix ? kCGLNoError : kCGLBadPixelFormat);
}

EXPORT AGLRendererInfo aglQueryRendererInfo(const AGLDevice *gdevs, GLint ndev)
{
    (void)gdevs; (void)ndev;
    CGLRendererInfoObj info = NULL;
    GLint count = 0;
    result(CGLQueryRendererInfo(0xffffffff, &info, &count));
    return info;
}

EXPORT AGLRendererInfo aglQueryRendererInfoForCGDirectDisplayIDs(const uint32_t *displays, GLint ndev)
{
    (void)displays; (void)ndev;
    return aglQueryRendererInfo(NULL, 0);
}

EXPORT void aglDestroyRendererInfo(AGLRendererInfo rend) { result(CGLDestroyRendererInfo(rend)); }
/* AGL walks renderers as a list; the first entry is the hardware one. */
EXPORT AGLRendererInfo aglNextRendererInfo(AGLRendererInfo rend) { (void)rend; return NULL; }

EXPORT GLboolean aglDescribeRenderer(AGLRendererInfo rend, GLint prop, GLint *value)
{
    return result(CGLDescribeRenderer(rend, 0, (CGLRendererProperty)prop, value));
}

EXPORT AGLContext aglCreateContext(AGLPixelFormat pix, AGLContext share)
{
    CGLContextObj ctx = NULL;
    result(CGLCreateContext(pix, share, &ctx));
    return ctx;
}

EXPORT GLboolean aglDestroyContext(AGLContext ctx)
{
    if (CGLGetCurrentContext() == ctx) CGLSetCurrentContext(NULL);
    return result(CGLDestroyContext(ctx));
}

EXPORT GLboolean aglCopyContext(AGLContext src, AGLContext dst, GLuint mask)
{
    return result(CGLCopyContext(src, dst, mask));
}

EXPORT GLboolean aglUpdateContext(AGLContext ctx) { return result(CGLUpdateContext(ctx)); }
EXPORT GLboolean aglSetCurrentContext(AGLContext ctx) { return result(CGLSetCurrentContext(ctx)); }
EXPORT AGLContext aglGetCurrentContext(void) { return CGLGetCurrentContext(); }

EXPORT GLboolean aglGetCGLContext(AGLContext ctx, void **cgl_ctx)
{
    if (!cgl_ctx) return result(kCGLBadAddress);
    *cgl_ctx = ctx;
    return result(ctx ? kCGLNoError : kCGLBadContext);
}

EXPORT GLboolean aglSetDrawable(AGLContext ctx, AGLDrawable draw)
{
    if (!draw) return result(CGLClearDrawable(ctx));
    return result(kCGLBadDrawable);
}
EXPORT AGLDrawable aglGetDrawable(AGLContext ctx) { (void)ctx; return NULL; }
EXPORT GLboolean aglSetWindowRef(AGLContext ctx, void *window)
{
    if (!window) return result(CGLClearDrawable(ctx));
    return result(kCGLBadWindow);
}
EXPORT void *aglGetWindowRef(AGLContext ctx) { (void)ctx; return NULL; }
EXPORT GLboolean aglSetHIViewRef(AGLContext ctx, void *view)
{
    if (!view) return result(CGLClearDrawable(ctx));
    return result(kCGLBadDrawable);
}
EXPORT void *aglGetHIViewRef(AGLContext ctx) { (void)ctx; return NULL; }

EXPORT GLboolean aglSetOffScreen(AGLContext ctx, GLsizei width, GLsizei height, GLsizei rowbytes,
                                 void *baseaddr)
{
    return result(CGLSetOffScreen(ctx, width, height, rowbytes, baseaddr));
}
EXPORT GLboolean aglSetFullScreen(AGLContext ctx, GLsizei width, GLsizei height, GLsizei freq,
                                  GLint device)
{
    (void)width; (void)height; (void)freq; (void)device;
    return result(CGLSetFullScreen(ctx));
}

EXPORT GLboolean aglSetVirtualScreen(AGLContext ctx, GLint screen)
{
    return result(CGLSetVirtualScreen(ctx, screen));
}
EXPORT GLint aglGetVirtualScreen(AGLContext ctx)
{
    GLint screen = 0;
    result(CGLGetVirtualScreen(ctx, &screen));
    return screen;
}

EXPORT void aglGetVersion(GLint *major, GLint *minor)
{
    if (major) *major = 3;
    if (minor) *minor = 1;
}

EXPORT GLboolean aglConfigure(GLenum pname, GLuint param)
{
    (void)pname; (void)param;
    return result(kCGLNoError);
}
EXPORT GLboolean aglSetGlobalOption(GLenum pname, const GLint *params)
{
    return result(CGLSetGlobalOption((CGLGlobalOption)pname, params));
}
EXPORT GLboolean aglGetGlobalOption(GLenum pname, GLint *params)
{
    return result(CGLGetGlobalOption((CGLGlobalOption)pname, params));
}

EXPORT void aglSwapBuffers(AGLContext ctx) { result(CGLFlushDrawable(ctx)); }

EXPORT GLboolean aglEnable(AGLContext ctx, GLenum pname)
{
    switch (pname) {
    case AGL_BUFFER_RECT: case AGL_COLORMAP_TRACKING: case AGL_FS_CAPTURE_SINGLE:
    case AGL_CLIP_REGION:
        return result(ctx ? kCGLNoError : kCGLBadContext);
    default:
        return result(CGLEnable(ctx, (CGLContextEnable)pname));
    }
}
EXPORT GLboolean aglDisable(AGLContext ctx, GLenum pname)
{
    switch (pname) {
    case AGL_BUFFER_RECT: case AGL_COLORMAP_TRACKING: case AGL_FS_CAPTURE_SINGLE:
    case AGL_CLIP_REGION:
        return result(ctx ? kCGLNoError : kCGLBadContext);
    default:
        return result(CGLDisable(ctx, (CGLContextEnable)pname));
    }
}
EXPORT GLboolean aglIsEnabled(AGLContext ctx, GLenum pname)
{
    GLint enabled = 0;
    switch (pname) {
    case AGL_BUFFER_RECT: case AGL_COLORMAP_TRACKING: case AGL_FS_CAPTURE_SINGLE:
    case AGL_CLIP_REGION:
        return GL_FALSE;
    default:
        result(CGLIsEnabled(ctx, (CGLContextEnable)pname, &enabled));
        return enabled != 0;
    }
}

EXPORT GLboolean aglSetInteger(AGLContext ctx, GLenum pname, const GLint *params)
{
    switch (pname) {
    case AGL_BUFFER_RECT: case AGL_SWAP_LIMIT: case AGL_COLORMAP_ENTRY: case AGL_STATE_VALIDATION:
    case AGL_BUFFER_NAME: case AGL_ORDER_CONTEXT_TO_FRONT: case AGL_CLIP_REGION:
        return result(ctx ? kCGLNoError : kCGLBadContext);
    default:
        return result(CGLSetParameter(ctx, (CGLContextParameter)pname, params));
    }
}
EXPORT GLboolean aglGetInteger(AGLContext ctx, GLenum pname, GLint *params)
{
    switch (pname) {
    case AGL_BUFFER_RECT: case AGL_SWAP_LIMIT: case AGL_BUFFER_NAME:
    case AGL_CONTEXT_SURFACE_ID: case AGL_CONTEXT_DISPLAY_ID:
        if (params) *params = 0;
        return result(ctx ? kCGLNoError : kCGLBadContext);
    default:
        return result(CGLGetParameter(ctx, (CGLContextParameter)pname, params));
    }
}

EXPORT GLenum aglGetError(void)
{
    GLenum error = agl_error;
    agl_error = AGL_NO_ERROR;
    return error;
}
EXPORT const GLubyte *aglErrorString(GLenum code)
{
    return (const GLubyte *)CGLErrorString((CGLError)code);
}
EXPORT void aglResetLibrary(void) {}

EXPORT GLboolean aglCreatePBuffer(GLint width, GLint height, GLenum target, GLenum internalFormat,
                                  long max_level, AGLPbuffer *pbuffer)
{
    (void)width; (void)height; (void)target; (void)internalFormat; (void)max_level;
    if (pbuffer) *pbuffer = NULL;
    return result(kCGLBadAlloc);
}
EXPORT GLboolean aglDestroyPBuffer(AGLPbuffer pbuffer) { (void)pbuffer; return result(kCGLBadAddress); }

#define ENTRY(name) {#name, (void *)name}
static const struct { const char *name; void *function; } agl_entries[] = {
    ENTRY(aglCreatePixelFormat), ENTRY(aglChoosePixelFormat), ENTRY(aglDestroyPixelFormat),
    ENTRY(aglNextPixelFormat), ENTRY(aglDescribePixelFormat), ENTRY(aglDevicesOfPixelFormat),
    ENTRY(aglGetCGLPixelFormat), ENTRY(aglQueryRendererInfo),
    ENTRY(aglQueryRendererInfoForCGDirectDisplayIDs), ENTRY(aglDestroyRendererInfo),
    ENTRY(aglNextRendererInfo), ENTRY(aglDescribeRenderer), ENTRY(aglCreateContext),
    ENTRY(aglDestroyContext), ENTRY(aglCopyContext), ENTRY(aglUpdateContext),
    ENTRY(aglSetCurrentContext), ENTRY(aglGetCurrentContext), ENTRY(aglGetCGLContext),
    ENTRY(aglSetDrawable), ENTRY(aglGetDrawable), ENTRY(aglSetWindowRef), ENTRY(aglGetWindowRef),
    ENTRY(aglSetHIViewRef), ENTRY(aglGetHIViewRef), ENTRY(aglSetOffScreen), ENTRY(aglSetFullScreen),
    ENTRY(aglSetVirtualScreen), ENTRY(aglGetVirtualScreen), ENTRY(aglGetVersion),
    ENTRY(aglConfigure), ENTRY(aglSetGlobalOption), ENTRY(aglGetGlobalOption),
    ENTRY(aglSwapBuffers), ENTRY(aglEnable), ENTRY(aglDisable), ENTRY(aglIsEnabled),
    ENTRY(aglSetInteger), ENTRY(aglGetInteger), ENTRY(aglGetError), ENTRY(aglErrorString),
    ENTRY(aglResetLibrary), ENTRY(aglCreatePBuffer), ENTRY(aglDestroyPBuffer),
};

void *lp32gl_agl_proc_address(const char *name)
{
    for (size_t i = 0; i < sizeof agl_entries / sizeof agl_entries[0]; ++i)
        if (!strcmp(name, agl_entries[i].name)) return agl_entries[i].function;
    return NULL;
}
