/* Apple's CGL API on Mesa.  Semantics follow OpenGL.framework on Apple
 * silicon: one virtual screen, one accelerated renderer (plus the software
 * renderer in renderer queries), legacy and core profiles, share groups,
 * recursive context locks, and the context parameters games touch. */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lp32gl_internal.h"

#define EXPORT LP32GL_EXPORT

static bool trace_context(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("LP32GL_TRACE_CONTEXT") != NULL;
    return enabled;
}

static __thread CGLContextObj tls_current;
static __thread CGLContextObj tls_requested_context;
struct lp32gl_thread_clone {
    CGLContextObj requested;
    CGLContextObj clone;
    struct lp32gl_thread_clone *next;
};
static __thread struct lp32gl_thread_clone *tls_clones;
static pthread_key_t thread_clone_key;
static pthread_once_t thread_clone_key_once = PTHREAD_ONCE_INIT;
static void dispose_thread_clones(void *value)
{
    CGLSetCurrentContext(NULL);
    struct lp32gl_thread_clone *item = value;
    while (item) {
        struct lp32gl_thread_clone *next = item->next;
        CGLReleaseContext(item->clone);
        CGLReleaseContext(item->requested);
        free(item);
        item = next;
    }
    tls_clones = NULL;
}
static void create_thread_clone_key(void)
{
    pthread_key_create(&thread_clone_key, dispose_thread_clones);
}
/* Mesa cannot bind one DRI context on two threads at once. */
static pthread_mutex_t current_context_lock = PTHREAD_MUTEX_INITIALIZER;
static char last_error[512];
static pthread_mutex_t error_lock = PTHREAD_MUTEX_INITIALIZER;

void lp32gl_set_error(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    pthread_mutex_lock(&error_lock);
    vsnprintf(last_error, sizeof last_error, format, args);
    pthread_mutex_unlock(&error_lock);
    va_end(args);
    if (getenv("LP32GL_DEBUG")) fprintf(stderr, "lp32gl: %s\n", last_error);
}

EXPORT const char *lp32gl_last_error(void) { return last_error; }

static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static bool init_ok;

static void initialize(void)
{
    /* Mesa computes GL versions from the Vulkan driver's features.  Without
       transform feedback on KosmicKrisp it stops at 2.1, so LP32GL asks for
       the 4.1 Apple ships and reports each profile's version itself. */
    setenv("MESA_GL_VERSION_OVERRIDE", "4.1", 0);
    setenv("MESA_GLSL_VERSION_OVERRIDE", "410", 0);
    /* Zink expects custom border colors (GL_CLAMP_TO_BORDER with arbitrary
       colors); KosmicKrisp lists them as experimental. */
    setenv("MESA_KK_EXPERIMENTAL", "custom_border", 0);
    char error[256];
    init_ok = lp32_dri_initialize(error, sizeof error);
    if (!init_ok) lp32gl_set_error("%s", error);
    else lp32gl_exports_resolve();
}

EXPORT bool lp32gl_initialize(void)
{
    pthread_once(&init_once, initialize);
    return init_ok;
}

CGLContextObj lp32gl_current_context(void) { return tls_current; }

static bool valid_pixel_format(CGLPixelFormatObj pix)
{
    return pix && pix->magic == LP32GL_PIXEL_FORMAT_MAGIC;
}
static bool valid_context(CGLContextObj ctx)
{
    return ctx && ctx->magic == LP32GL_CONTEXT_MAGIC;
}

/* ---- pixel formats ---------------------------------------------------- */

/* Picks the Mesa config for a request.  Apple on Apple silicon always hands
   out RGBA8 (or RGBA16F for color-float) with D24S8/D32S8-class depth, so
   the choice is: satisfy every minimum, then prefer the smallest fit. */
static const void *choose_config(const struct _CGLPixelFormatObject *req)
{
    const void *best = NULL;
    long best_score = -1;
    for (unsigned i = 0; i < lp32_dri_config_count(); ++i) {
        const void *config = lp32_dri_config_at(i);
        struct lp32_dri_config_desc d;
        lp32_dri_describe_config(config, &d);
        if (!d.double_buffer) continue;
        if (d.float_color != req->color_float) continue;
        if (!req->color_float && (d.red != 8 || d.green != 8 || d.blue != 8)) continue;
        if (req->color_float && d.red != 16) continue;
        if (d.alpha < 8) continue; /* Apple's surfaces always carry alpha */
        if ((GLint)d.depth < req->depth_size) continue;
        if ((GLint)d.stencil < req->stencil_size) continue;
        GLint want_samples = (req->sample_buffers > 0 || req->multisample) ?
            (req->samples > 0 ? req->samples : 4) : 0;
        if (want_samples > 0 && (GLint)d.samples < want_samples) continue;
        if (want_samples == 0 && d.samples) continue;
        long score = 0;
        /* Apple silicon only has 32-bit depth; match what Apple hands out. */
        if (req->depth_size > 0) score += d.depth == 32 ? 0 : 512;
        score += (long)(d.stencil - (unsigned)req->stencil_size) * 16;
        score += (long)(d.samples - (unsigned)want_samples) * 64;
        if (req->depth_size == 0 && d.depth) score += 1024; /* keep it lean */
        if (req->stencil_size == 0 && d.stencil) score += 256;
        if (d.srgb_capable) score -= 1; /* GL_FRAMEBUFFER_SRGB works on it */
        if (best_score < 0 || score < best_score) {
            best = config;
            best_score = score;
        }
    }
    return best;
}

CGLError lp32gl_choose(const CGLPixelFormatAttribute *attribs, CGLPixelFormatObj *out, GLint *count)
{
    if (!out) return kCGLBadAddress;
    *out = NULL;
    if (count) *count = 0;
    if (!lp32gl_initialize()) return kCGLBadCodeModule;
    struct _CGLPixelFormatObject req;
    memset(&req, 0, sizeof req);
    req.profile_attribute = kCGLOGLPVersion_Legacy;
    req.color_size = 0;
    req.display_mask = 0xffffffff;
    for (const CGLPixelFormatAttribute *a = attribs; a && *a; ++a) {
        switch ((int)*a) {
        case kCGLPFAAllRenderers: break;
        case kCGLPFATripleBuffer: case kCGLPFADoubleBuffer: req.double_buffer = true; break;
        case kCGLPFAStereo: req.stereo = true; break;
        case kCGLPFAAuxBuffers: req.aux_buffers = *++a; break;
        case kCGLPFAColorSize: req.color_size = *++a; break;
        case kCGLPFAAlphaSize: req.alpha_size = *++a; break;
        case kCGLPFADepthSize: req.depth_size = *++a; break;
        case kCGLPFAStencilSize: req.stencil_size = *++a; break;
        case kCGLPFAAccumSize: req.accum_size = *++a; break;
        case kCGLPFAMinimumPolicy: req.minimum_policy = true; break;
        case kCGLPFAMaximumPolicy: req.maximum_policy = true; break;
        case kCGLPFAOffScreen: req.offscreen = true; break;
        case kCGLPFAFullScreen: req.fullscreen = true; break;
        case kCGLPFASampleBuffers: req.sample_buffers = *++a; break;
        case kCGLPFASamples: req.samples = *++a; break;
        case kCGLPFAAuxDepthStencil: break;
        case kCGLPFAColorFloat: req.color_float = true; break;
        case kCGLPFAMultisample: req.multisample = true; break;
        case kCGLPFASupersample: req.supersample = true; break;
        case kCGLPFASampleAlpha: break;
        case kCGLPFARendererID: req.renderer_id = *++a; break;
        case kCGLPFASingleRenderer: break;
        case kCGLPFANoRecovery: req.no_recovery = true; break;
        case kCGLPFAAccelerated: req.accelerated = true; break;
        case kCGLPFAClosestPolicy: req.closest_policy = true; break;
        case kCGLPFARobust: break;
        case kCGLPFABackingStore: req.backing_store = true; break;
        case kCGLPFABackingVolatile: break;
        case kCGLPFAMPSafe: req.mp_safe = true; break;
        case kCGLPFAWindow: req.window = true; break;
        case kCGLPFAMultiScreen: req.multiscreen = true; break;
        case kCGLPFACompliant: req.compliant = true; break;
        case kCGLPFADisplayMask: req.display_mask = *++a; break;
        case kCGLPFAPBuffer: req.pbuffer = true; break;
        case kCGLPFARemotePBuffer: req.remote_pbuffer = true; break;
        case kCGLPFAAllowOfflineRenderers: req.allow_offline = true; break;
        case kCGLPFAAcceleratedCompute: req.accelerated_compute = true; break;
        case kCGLPFAOpenGLProfile: req.profile_attribute = *++a; break;
        case kCGLPFASupportsAutomaticGraphicsSwitching: break;
        case kCGLPFAVirtualScreenCount: ++a; break;
        default:
            lp32gl_set_error("CGLChoosePixelFormat: unknown attribute %d", (int)*a);
            return kCGLBadAttribute;
        }
    }
    switch (req.profile_attribute) {
    case kCGLOGLPVersion_Legacy: req.profile = LP32GL_PROFILE_LEGACY; break;
    case kCGLOGLPVersion_3_2_Core:
    case kCGLOGLPVersion_GL4_Core: req.profile = LP32GL_PROFILE_CORE; break;
    default: return kCGLBadAttribute;
    }
    /* Only the one hardware renderer exists; asking for the software one or
       a foreign renderer ID finds nothing, like Apple's with no match. */
    if (req.renderer_id && req.renderer_id != LP32GL_RENDERER_ID &&
        (req.renderer_id & 0xfffff000) != (LP32GL_RENDERER_ID & 0xfffff000))
        return kCGLNoError;
    if (req.stereo || req.accum_size > 64 || req.aux_buffers > 0) return kCGLNoError;
    const void *config = choose_config(&req);
    if (!config) {
        lp32gl_set_error("no pixel format for profile=%x color=%d depth=%d stencil=%d samples=%d",
                         req.profile_attribute, req.color_size, req.depth_size,
                         req.stencil_size, req.samples);
        return kCGLNoError;
    }
    struct _CGLPixelFormatObject *pix = calloc(1, sizeof *pix);
    *pix = req;
    pix->magic = LP32GL_PIXEL_FORMAT_MAGIC;
    pix->refcount = 1;
    pix->config = config;
    lp32_dri_describe_config(config, &pix->desc);
    *out = pix;
    if (count) *count = 1;
    return kCGLNoError;
}

EXPORT CGLError CGLChoosePixelFormat(const CGLPixelFormatAttribute *attribs, CGLPixelFormatObj *pix,
                                     GLint *npix)
{
    return lp32gl_choose(attribs, pix, npix);
}

EXPORT CGLPixelFormatObj CGLRetainPixelFormat(CGLPixelFormatObj pix)
{
    if (valid_pixel_format(pix)) __atomic_add_fetch(&pix->refcount, 1, __ATOMIC_RELAXED);
    return pix;
}

EXPORT void CGLReleasePixelFormat(CGLPixelFormatObj pix)
{
    if (!valid_pixel_format(pix)) return;
    if (__atomic_sub_fetch(&pix->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
        pix->magic = 0;
        free(pix);
    }
}

EXPORT CGLError CGLDestroyPixelFormat(CGLPixelFormatObj pix)
{
    if (!pix) return kCGLNoError;
    if (!valid_pixel_format(pix)) return kCGLBadPixelFormat;
    CGLReleasePixelFormat(pix);
    return kCGLNoError;
}

EXPORT GLuint CGLGetPixelFormatRetainCount(CGLPixelFormatObj pix)
{
    return valid_pixel_format(pix) ? (GLuint)pix->refcount : 0;
}

EXPORT CGLError CGLDescribePixelFormat(CGLPixelFormatObj pix, GLint pix_num,
                                       CGLPixelFormatAttribute attrib, GLint *value)
{
    if (!valid_pixel_format(pix)) return kCGLBadPixelFormat;
    if (!value) return kCGLBadAddress;
    if (pix_num != 0) return kCGLBadValue;
    const struct lp32_dri_config_desc *d = &pix->desc;
    switch ((int)attrib) {
    case kCGLPFAAllRenderers: *value = 0; break;
    case kCGLPFADoubleBuffer: *value = pix->double_buffer; break;
    case kCGLPFATripleBuffer: *value = 0; break;
    case kCGLPFAStereo: *value = 0; break;
    case kCGLPFAAuxBuffers: *value = 0; break;
    case kCGLPFAColorSize: *value = (GLint)((d->red + d->green + d->blue + d->alpha)); break;
    case kCGLPFAAlphaSize: *value = (GLint)d->alpha; break;
    case kCGLPFADepthSize: *value = (GLint)d->depth; break;
    case kCGLPFAStencilSize: *value = (GLint)d->stencil; break;
    case kCGLPFAAccumSize: *value = 0; break;
    case kCGLPFAMinimumPolicy: *value = pix->minimum_policy; break;
    case kCGLPFAMaximumPolicy: *value = pix->maximum_policy; break;
    case kCGLPFAOffScreen: *value = 1; break;
    case kCGLPFAFullScreen: *value = 0; break;
    case kCGLPFASampleBuffers: *value = d->samples ? 1 : 0; break;
    case kCGLPFASamples: *value = (GLint)d->samples; break;
    case kCGLPFAAuxDepthStencil: *value = 0; break;
    case kCGLPFAColorFloat: *value = d->float_color; break;
    case kCGLPFAMultisample: *value = d->samples ? 1 : 0; break;
    case kCGLPFASupersample: *value = 0; break;
    case kCGLPFASampleAlpha: *value = 0; break;
    case kCGLPFARendererID: *value = LP32GL_RENDERER_ID; break;
    case kCGLPFASingleRenderer: *value = 0; break;
    case kCGLPFANoRecovery: *value = pix->no_recovery; break;
    case kCGLPFAAccelerated: *value = 1; break;
    case kCGLPFAClosestPolicy: *value = pix->closest_policy; break;
    case kCGLPFARobust: *value = 0; break;
    case kCGLPFABackingStore: *value = pix->backing_store; break;
    case kCGLPFABackingVolatile: *value = 1; break;
    case kCGLPFAMPSafe: *value = 1; break;
    case kCGLPFAWindow: *value = 1; break;
    case kCGLPFAMultiScreen: *value = 1; break;
    case kCGLPFACompliant: *value = 1; break;
    case kCGLPFADisplayMask: *value = (GLint)pix->display_mask; break;
    case kCGLPFAPBuffer: *value = 0; break;
    case kCGLPFARemotePBuffer: *value = 0; break;
    case kCGLPFAAllowOfflineRenderers: *value = pix->allow_offline; break;
    case kCGLPFAAcceleratedCompute: *value = 1; break;
    case kCGLPFAOpenGLProfile:
        *value = pix->profile == LP32GL_PROFILE_CORE ? kCGLOGLPVersion_GL4_Core : kCGLOGLPVersion_Legacy;
        break;
    case kCGLPFAVirtualScreenCount: *value = 1; break;
    case kCGLPFASupportsAutomaticGraphicsSwitching: *value = 0; break;
    default: return kCGLBadAttribute;
    }
    return kCGLNoError;
}

/* ---- renderer info ---------------------------------------------------- */

struct _CGLRendererInfoObject {
    uint32_t magic;
    GLint display_mask;
};
#define LP32GL_RENDERER_INFO_MAGIC 0x4c505249u

EXPORT CGLError CGLQueryRendererInfo(GLuint display_mask, CGLRendererInfoObj *rend, GLint *nrend)
{
    if (!rend || !nrend) return kCGLBadAddress;
    struct _CGLRendererInfoObject *info = calloc(1, sizeof *info);
    info->magic = LP32GL_RENDERER_INFO_MAGIC;
    info->display_mask = (GLint)display_mask;
    *rend = info;
    *nrend = 2; /* accelerated + Apple's software renderer, as Apple reports */
    return kCGLNoError;
}

EXPORT CGLError CGLDestroyRendererInfo(CGLRendererInfoObj rend)
{
    if (!rend || rend->magic != LP32GL_RENDERER_INFO_MAGIC) return kCGLBadRendererInfo;
    rend->magic = 0;
    free(rend);
    return kCGLNoError;
}

static GLint video_memory_megabytes(void)
{
    /* Apple reports the GPU's recommended working set. */
    static GLint megabytes;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        unsigned long long bytes = device ? device.recommendedMaxWorkingSetSize
                                          : [[NSProcessInfo processInfo] physicalMemory] * 3 / 4;
        megabytes = (GLint)(bytes / (1024 * 1024));
        [device release];
    });
    return megabytes;
}

/* Apple's answers on Apple silicon for the hardware renderer (0) and the
   software renderer (1), from data/apple-cgl-renderer-reference.txt.  Games
   gate on several of these (COD4 wants kCGLRPFullScreen). */
EXPORT CGLError CGLDescribeRenderer(CGLRendererInfoObj rend, GLint rend_num, CGLRendererProperty prop,
                                    GLint *value)
{
    if (!rend || rend->magic != LP32GL_RENDERER_INFO_MAGIC) return kCGLBadRendererInfo;
    if (!value) return kCGLBadAddress;
    if (rend_num < 0 || rend_num > 1) return kCGLBadValue;
    bool hw = rend_num == 0;
    switch ((int)prop) {
    case kCGLRPOffScreen: *value = 0; break;
    case kCGLRPFullScreen: *value = hw; break;
    case kCGLRPRendererID: *value = hw ? LP32GL_RENDERER_ID : LP32GL_SOFTWARE_RENDERER_ID; break;
    case kCGLRPAccelerated: *value = hw; break;
    case kCGLRPRobust: *value = !hw; break;
    case kCGLRPBackingStore: *value = 1; break;
    case kCGLRPMPSafe: *value = 1; break;
    case kCGLRPWindow: *value = 1; break;
    case kCGLRPMultiScreen: *value = 1; break;
    case kCGLRPCompliant: *value = 1; break;
    case kCGLRPDisplayMask: *value = 0x1f; break;
    case kCGLRPBufferModes: *value = 0xd; break;
    case kCGLRPColorModes: *value = hw ? 0x0a848000 : 0x08008000; break;
    case kCGLRPAccumModes: *value = 0x08000000; break;
    case kCGLRPDepthModes: *value = 0x1001; break;   /* 0 or 32 bits */
    case kCGLRPStencilModes: *value = 0x81; break;   /* 0 or 8 bits */
    case kCGLRPMaxAuxBuffers: *value = hw ? 2 : 4; break;
    case kCGLRPMaxSampleBuffers: *value = 1; break;
    case kCGLRPMaxSamples: *value = hw ? 8 : 16; break;
    case kCGLRPSampleModes: *value = hw ? 3 : 1; break;
    case kCGLRPSampleAlpha: *value = 1; break;
    case kCGLRPVideoMemory: *value = hw ? 0x7fffffff : 0; break;
    case kCGLRPTextureMemory: *value = hw ? 0x7fffffff : 0; break;
    case kCGLRPGPUVertProcCapable: *value = hw; break;
    case kCGLRPGPUFragProcCapable: *value = hw; break;
    case kCGLRPRendererCount: *value = 2; break;
    case kCGLRPOnline: *value = 1; break;
    case kCGLRPAcceleratedCompute: *value = hw; break;
    case kCGLRPVideoMemoryMegabytes: *value = hw ? video_memory_megabytes() : 0; break;
    case kCGLRPTextureMemoryMegabytes: *value = hw ? video_memory_megabytes() : 0; break;
    case kCGLRPMajorGLVersion: *value = 4; break;
    case kCGLRPRegistryIDLow: *value = hw ? 0x663 : 0; break;
    case kCGLRPRegistryIDHigh: *value = hw ? 1 : 0; break;
    case kCGLRPRemovable: *value = 0; break;
    default: return kCGLBadProperty;
    }
    return kCGLNoError;
}

/* ---- contexts --------------------------------------------------------- */

EXPORT CGLError CGLCreateContext(CGLPixelFormatObj pix, CGLContextObj share, CGLContextObj *ctx)
{
    if (!ctx) return kCGLBadAddress;
    *ctx = NULL;
    if (!valid_pixel_format(pix)) return kCGLBadPixelFormat;
    if (share && !valid_context(share)) return kCGLBadContext;
    if (share && share->profile != pix->profile) return kCGLBadMatch;
    if (!lp32gl_initialize()) return kCGLBadCodeModule;
    bool core = pix->profile == LP32GL_PROFILE_CORE;
    unsigned error = 0;
    void *dri = lp32_dri_create_context(pix->config, share ? share->dri : NULL,
                                        core ? LP32_DRI_API_CORE : LP32_DRI_API_COMPAT,
                                        core ? 4 : 2, core ? 1 : 1, core, false, &error);
    if (!dri) {
        lp32gl_set_error("Mesa refused the %s context (DRI error %u)", core ? "core" : "legacy", error);
        return error == 1 ? kCGLBadAlloc : kCGLBadMatch;
    }
    struct _CGLContextObject *c = calloc(1, sizeof *c);
    c->magic = LP32GL_CONTEXT_MAGIC;
    c->refcount = 1;
    c->pixel_format = CGLRetainPixelFormat(pix);
    c->dri = dri;
    c->profile = pix->profile;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&c->lock, &attr);
    pthread_mutexattr_destroy(&attr);
    pthread_mutex_init(&c->state, NULL);
    c->swap_interval = 0;
    c->surface_opacity = 1;
    c->surface_order = 1;
    c->rasterization = true;
    if (share) {
        c->share_group = share->share_group;
        __atomic_add_fetch(&c->share_group->refcount, 1, __ATOMIC_RELAXED);
    } else {
        c->share_group = calloc(1, sizeof *c->share_group);
        c->share_group->refcount = 1;
        c->share_group->root_dri = dri;
    }
    c->gl_state = lp32gl_gl_state_create(c);
    if (trace_context())
        fprintf(stderr, "LP32GL context created ctx=%p share=%p profile=%s\n",
                (void *)c, (void *)share, core ? "core" : "legacy");
    *ctx = c;
    return kCGLNoError;
}

static void destroy_context(CGLContextObj ctx)
{
    if (tls_current == ctx) CGLSetCurrentContext(NULL);
    lp32gl_context_attach_surface(ctx, NULL);
    lp32gl_gl_state_destroy(ctx->gl_state);
    lp32_dri_destroy_context(ctx->dri);
    if (__atomic_sub_fetch(&ctx->share_group->refcount, 1, __ATOMIC_ACQ_REL) == 0)
        free(ctx->share_group);
    CGLReleasePixelFormat(ctx->pixel_format);
    pthread_mutex_destroy(&ctx->lock);
    pthread_mutex_destroy(&ctx->state);
    ctx->magic = 0;
    free(ctx);
}

EXPORT CGLContextObj CGLRetainContext(CGLContextObj ctx)
{
    if (valid_context(ctx)) __atomic_add_fetch(&ctx->refcount, 1, __ATOMIC_RELAXED);
    return ctx;
}

EXPORT void CGLReleaseContext(CGLContextObj ctx)
{
    if (!valid_context(ctx)) return;
    if (__atomic_sub_fetch(&ctx->refcount, 1, __ATOMIC_ACQ_REL) == 0) destroy_context(ctx);
}

EXPORT GLuint CGLGetContextRetainCount(CGLContextObj ctx)
{
    return valid_context(ctx) ? (GLuint)ctx->refcount : 0;
}

EXPORT CGLError CGLDestroyContext(CGLContextObj ctx)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    CGLReleaseContext(ctx);
    return kCGLNoError;
}

EXPORT CGLPixelFormatObj CGLGetPixelFormat(CGLContextObj ctx)
{
    return valid_context(ctx) ? ctx->pixel_format : NULL;
}

EXPORT CGLShareGroupObj CGLGetShareGroup(CGLContextObj ctx)
{
    return valid_context(ctx) ? (CGLShareGroupObj)ctx->share_group : NULL;
}

EXPORT CGLError CGLCopyContext(CGLContextObj src, CGLContextObj dst, GLbitfield mask)
{
    (void)mask;
    if (!valid_context(src) || !valid_context(dst)) return kCGLBadContext;
    /* glCopyContext-style attribute copies are not supported by Mesa's
       gallium frontend; nothing ships that relies on it. */
    return kCGLBadState;
}

/* Binds `ctx` on the calling thread with whatever surface it has now. */
static bool bind_context(CGLContextObj ctx)
{
    pthread_mutex_lock(&ctx->state);
    struct lp32gl_surface *surface = ctx->surface;
    lp32gl_surface_retain(surface);
    pthread_mutex_unlock(&ctx->state);
    void *drawable = lp32gl_surface_drawable(surface);
    bool ok = lp32_dri_make_current(ctx->dri, drawable, drawable);
    if (!ok) {
        lp32gl_surface_release(surface);
        return false;
    }
    struct lp32gl_surface *old = ctx->bound_surface;
    ctx->bound_surface = surface;
    lp32gl_surface_release(old);
    if (surface) lp32gl_surface_set_swap_interval(surface, ctx->swap_interval);
    return ok;
}

/* dri_make_current requires the previous binding to be released first, even
   when only the drawable changed. */
static bool rebind_context(CGLContextObj ctx)
{
    if (!lp32_dri_release_current(ctx->dri)) return false;
    return bind_context(ctx);
}

/* Some SDL games use one NSOpenGLContext on several worker threads without
 * clearing it on the render thread. Mesa cannot bind the same DRI context
 * concurrently, but shared contexts expose the texture and shader objects
 * those workers upload. Keep one clone per worker and public context. */
static CGLContextObj clone_for_thread(CGLContextObj requested)
{
    for (struct lp32gl_thread_clone *item = tls_clones; item; item = item->next)
        if (item->requested == requested) return item->clone;
    CGLContextObj clone = NULL;
    if (CGLCreateContext(requested->pixel_format, requested, &clone) != kCGLNoError)
        return NULL;
    clone->mirror_context = requested;
    struct lp32gl_thread_clone *item = calloc(1, sizeof *item);
    if (!item) {
        CGLReleaseContext(clone);
        return NULL;
    }
    item->requested = CGLRetainContext(requested);
    item->clone = clone;
    item->next = tls_clones;
    tls_clones = item;
    pthread_once(&thread_clone_key_once, create_thread_clone_key);
    pthread_setspecific(thread_clone_key, tls_clones);
    if (trace_context())
        fprintf(stderr, "LP32GL context cloned requested=%p clone=%p thread=%p\n",
                (void *)requested, (void *)clone, (void *)pthread_self());
    return clone;
}

static void mirror_context_surface(CGLContextObj clone, CGLContextObj requested)
{
    pthread_mutex_lock(&requested->state);
    struct lp32gl_surface *surface = requested->surface;
    lp32gl_surface_retain(surface);
    pthread_mutex_unlock(&requested->state);
    pthread_mutex_lock(&clone->state);
    struct lp32gl_surface *old = clone->surface;
    clone->surface = surface;
    pthread_mutex_unlock(&clone->state);
    lp32gl_surface_release(old);
}

EXPORT CGLError CGLSetCurrentContext(CGLContextObj ctx)
{
    if (trace_context())
        fprintf(stderr, "LP32GL context set thread=%p old=%p new=%p owner=%p current=%d\n",
                (void *)pthread_self(), (void *)tls_current, (void *)ctx,
                ctx ? (void *)ctx->current_thread : NULL, ctx ? ctx->current : 0);
    if (ctx && !valid_context(ctx)) return kCGLBadContext;
    CGLContextObj requested = ctx;
    if (ctx && ((ctx->current && !pthread_equal(ctx->current_thread, pthread_self())) ||
                (tls_requested_context == ctx && tls_current != ctx))) {
        ctx = clone_for_thread(ctx);
        if (!ctx) return kCGLBadAlloc;
        mirror_context_surface(ctx, requested);
    }
    pthread_mutex_lock(&current_context_lock);
    CGLContextObj old = tls_current;
    if (!ctx) {
        if (old) {
            lp32_dri_release_current(old->dri);
            old->current = false;
            tls_current = NULL;
        }
        tls_requested_context = NULL;
        pthread_mutex_unlock(&current_context_lock);
        return kCGLNoError;
    }
    if (old == ctx && ctx->bound_surface == ctx->surface) {
        tls_requested_context = requested;
        if (ctx->mirror_context) lp32gl_gl_context_made_current(ctx);
        pthread_mutex_unlock(&current_context_lock);
        return kCGLNoError;
    }
    if (ctx->current && !pthread_equal(ctx->current_thread, pthread_self())) {
        if (trace_context())
            fprintf(stderr, "LP32GL context rejected cross-thread bind new=%p owner=%p caller=%p\n",
                    (void *)ctx, (void *)ctx->current_thread, (void *)pthread_self());
        lp32gl_set_error("context %p made current on a second thread", (void *)ctx);
        pthread_mutex_unlock(&current_context_lock);
        return kCGLBadState;
    }
    if (old) {
        lp32_dri_release_current(old->dri);
        old->current = false;
        tls_current = NULL;
        tls_requested_context = NULL;
    }
    if (!bind_context(ctx)) {
        ctx->current = false;
        pthread_mutex_unlock(&current_context_lock);
        return kCGLBadContext;
    }
    ctx->current = true;
    ctx->current_thread = pthread_self();
    tls_current = ctx;
    tls_requested_context = requested;
    lp32gl_gl_context_made_current(ctx);
    pthread_mutex_unlock(&current_context_lock);
    return kCGLNoError;
}

EXPORT CGLContextObj CGLGetCurrentContext(void) { return tls_requested_context; }

void lp32gl_context_attach_surface(CGLContextObj ctx, struct lp32gl_surface *surface)
{
    pthread_mutex_lock(&ctx->state);
    struct lp32gl_surface *old = ctx->surface;
    ctx->surface = surface;
    lp32gl_surface_retain(surface);
    pthread_mutex_unlock(&ctx->state);
    lp32gl_surface_release(old);
    if (surface) lp32gl_context_update(ctx);
    /* Rebind now when the caller owns the context; otherwise the owning
       thread picks the new surface up at its next flush or make-current. */
    if (tls_current == ctx) {
        pthread_mutex_lock(&current_context_lock);
        if (!rebind_context(ctx)) {
            ctx->current = false;
            tls_current = NULL;
            lp32gl_set_error("could not rebind context %p after changing its surface", (void *)ctx);
        }
        pthread_mutex_unlock(&current_context_lock);
    }
    if (!surface && ctx->bound_surface && !ctx->current) {
        struct lp32gl_surface *bound = ctx->bound_surface;
        ctx->bound_surface = NULL;
        lp32gl_surface_release(bound);
    }
}

void lp32gl_context_update(CGLContextObj ctx)
{
    pthread_mutex_lock(&ctx->state);
    struct lp32gl_surface *surface = ctx->surface;
    lp32gl_surface_retain(surface);
    pthread_mutex_unlock(&ctx->state);
    if (!surface) return;
    NSView *view = (__bridge NSView *)lp32gl_surface_view(surface);
    bool best = false;
    if (view && [view respondsToSelector:@selector(wantsBestResolutionOpenGLSurface)]) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        best = view.wantsBestResolutionOpenGLSurface;
#pragma clang diagnostic pop
    }
    lp32gl_surface_configure(surface, ctx->backing_size_enabled ? ctx->backing_size : NULL, best,
                             ctx->surface_opacity, ctx->surface_order);
    lp32gl_surface_release(surface);
}

EXPORT int LP32CGLSetView(LP32CGLContextObj ctx, void *view)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    if (view == ctx->view && (ctx->surface || !view)) {
        lp32gl_context_update(ctx);
        return kCGLNoError;
    }
    ctx->view = view;
    struct lp32gl_surface *surface = view ? lp32gl_surface_for_view(view, ctx->pixel_format->config) : NULL;
    if (view && !surface) return kCGLBadDrawable;
    lp32gl_context_attach_surface(ctx, surface);
    lp32gl_surface_release(surface); /* attach took its own reference */
    return kCGLNoError;
}

EXPORT void *LP32CGLGetView(LP32CGLContextObj ctx)
{
    return valid_context(ctx) ? ctx->view : NULL;
}

EXPORT CGLError CGLClearDrawable(CGLContextObj ctx)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    ctx->view = NULL;
    lp32gl_context_attach_surface(ctx, NULL);
    return kCGLNoError;
}

EXPORT CGLError CGLUpdateContext(CGLContextObj ctx)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    lp32gl_context_update(ctx);
    return kCGLNoError;
}

EXPORT CGLError CGLFlushDrawable(CGLContextObj ctx)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    if (tls_requested_context == ctx && tls_current && tls_current != ctx)
        ctx = tls_current;
    CGLContextObj previous = tls_current;
    bool borrowed = false;
    if (previous != ctx) {
        if (ctx->current) {
            /* Current on another thread; Apple would present anyway, but
               Mesa's swap has to run on the owning thread. */
            return kCGLNoError;
        }
        if (CGLSetCurrentContext(ctx) != kCGLNoError) return kCGLBadContext;
        borrowed = true;
    } else if (ctx->bound_surface != ctx->surface) {
        pthread_mutex_lock(&current_context_lock);
        bool rebound = rebind_context(ctx);
        if (!rebound) {
            ctx->current = false;
            tls_current = NULL;
        }
        pthread_mutex_unlock(&current_context_lock);
        if (!rebound) return kCGLBadContext;
    }
    if (ctx->bound_surface) {
        lp32gl_surface_set_swap_interval(ctx->bound_surface, ctx->swap_interval);
        lp32_dri_swap_buffers(lp32gl_surface_drawable(ctx->bound_surface));
        lp32gl_gl_context_presented(ctx);
    } else {
        lp32_dri_flush(ctx->dri, NULL);
    }
    if (borrowed) CGLSetCurrentContext(previous);
    return kCGLNoError;
}

EXPORT CGLError CGLEnable(CGLContextObj ctx, CGLContextEnable pname)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    switch ((int)pname) {
    case kCGLCESwapRectangle: ctx->swap_rect_enabled = true; break;
    case kCGLCERasterization: ctx->rasterization = true; break;
    case kCGLCEStateValidation: ctx->state_validation = true; break;
    case kCGLCESurfaceBackingSize:
        ctx->backing_size_enabled = true;
        lp32gl_context_update(ctx);
        break;
    case kCGLCEDisplayListOptimization: break;
    case kCGLCEMPEngine: ctx->mp_engine = true; break;
    case kCGLCECrashOnRemovedFunctions: break;
    default: return kCGLBadEnumeration;
    }
    return kCGLNoError;
}

EXPORT CGLError CGLDisable(CGLContextObj ctx, CGLContextEnable pname)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    switch ((int)pname) {
    case kCGLCESwapRectangle: ctx->swap_rect_enabled = false; break;
    case kCGLCERasterization: ctx->rasterization = false; break;
    case kCGLCEStateValidation: ctx->state_validation = false; break;
    case kCGLCESurfaceBackingSize:
        ctx->backing_size_enabled = false;
        lp32gl_context_update(ctx);
        break;
    case kCGLCEDisplayListOptimization: break;
    case kCGLCEMPEngine: ctx->mp_engine = false; break;
    case kCGLCECrashOnRemovedFunctions: break;
    default: return kCGLBadEnumeration;
    }
    return kCGLNoError;
}

EXPORT CGLError CGLIsEnabled(CGLContextObj ctx, CGLContextEnable pname, GLint *enable)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    if (!enable) return kCGLBadAddress;
    switch ((int)pname) {
    case kCGLCESwapRectangle: *enable = ctx->swap_rect_enabled; break;
    case kCGLCERasterization: *enable = ctx->rasterization; break;
    case kCGLCEStateValidation: *enable = ctx->state_validation; break;
    case kCGLCESurfaceBackingSize: *enable = ctx->backing_size_enabled; break;
    case kCGLCEDisplayListOptimization: *enable = 0; break;
    case kCGLCEMPEngine: *enable = ctx->mp_engine; break;
    case kCGLCECrashOnRemovedFunctions: *enable = 0; break;
    default: return kCGLBadEnumeration;
    }
    return kCGLNoError;
}

EXPORT CGLError CGLSetParameter(CGLContextObj ctx, CGLContextParameter pname, const GLint *params)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    if (!params) return kCGLBadAddress;
    switch ((int)pname) {
    case kCGLCPSwapRectangle: memcpy(ctx->swap_rect, params, sizeof ctx->swap_rect); break;
    case kCGLCPSwapInterval:
        ctx->swap_interval = params[0] ? 1 : 0;
        if (tls_current == ctx && ctx->bound_surface)
            lp32gl_surface_set_swap_interval(ctx->bound_surface, ctx->swap_interval);
        break;
    case kCGLCPDispatchTableSize: break;
    case kCGLCPClientStorage: break;
    case kCGLCPSurfaceTexture: return kCGLBadState;
    case kCGLCPSurfaceOrder: ctx->surface_order = params[0]; lp32gl_context_update(ctx); break;
    case kCGLCPSurfaceOpacity: ctx->surface_opacity = params[0]; lp32gl_context_update(ctx); break;
    case kCGLCPSurfaceBackingSize:
        if (params[0] < 0 || params[1] < 0 || params[0] > 16384 || params[1] > 16384) return kCGLBadValue;
        ctx->backing_size[0] = params[0];
        ctx->backing_size[1] = params[1];
        if (ctx->backing_size_enabled) lp32gl_context_update(ctx);
        break;
    case kCGLCPSurfaceSurfaceVolatile: ctx->surface_volatile = params[0]; break;
    case kCGLCPReclaimResources: break;
    case kCGLCPCurrentRendererID: return kCGLBadEnumeration;
    case kCGLCPGPUVertexProcessing: case kCGLCPGPUFragmentProcessing: return kCGLBadEnumeration;
    case kCGLCPHasDrawable: return kCGLBadEnumeration;
    case kCGLCPMPSwapsInFlight: break;
    case kCGLCPGPURestartStatus: return kCGLBadEnumeration;
    case kCGLCPAbortOnGPURestartStatusBlacklisted: break;
    case kCGLCPSupportSeparateAddressSpace: break;
    case kCGLCPContextPriorityRequest: break;
    default: return kCGLBadEnumeration;
    }
    return kCGLNoError;
}

EXPORT CGLError CGLGetParameter(CGLContextObj ctx, CGLContextParameter pname, GLint *params)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    if (!params) return kCGLBadAddress;
    switch ((int)pname) {
    case kCGLCPSwapRectangle: memcpy(params, ctx->swap_rect, sizeof ctx->swap_rect); break;
    case kCGLCPSwapInterval: params[0] = ctx->swap_interval; break;
    case kCGLCPDispatchTableSize: params[0] = 0; break;
    case kCGLCPClientStorage: params[0] = 0; break;
    case kCGLCPSurfaceOrder: params[0] = ctx->surface_order; break;
    case kCGLCPSurfaceOpacity: params[0] = ctx->surface_opacity; break;
    case kCGLCPSurfaceBackingSize:
        params[0] = ctx->backing_size[0];
        params[1] = ctx->backing_size[1];
        break;
    case kCGLCPSurfaceSurfaceVolatile: params[0] = ctx->surface_volatile; break;
    case kCGLCPCurrentRendererID: params[0] = LP32GL_RENDERER_ID; break;
    case kCGLCPGPUVertexProcessing: case kCGLCPGPUFragmentProcessing: params[0] = 1; break;
    case kCGLCPHasDrawable: params[0] = ctx->surface != NULL; break;
    case kCGLCPMPSwapsInFlight: params[0] = 1; break;
    case kCGLCPGPURestartStatus: params[0] = kCGLCPGPURestartStatusNone; break;
    case kCGLCPAbortOnGPURestartStatusBlacklisted: params[0] = 0; break;
    case kCGLCPSupportSeparateAddressSpace: params[0] = 0; break;
    case kCGLCPContextPriorityRequest: params[0] = kCGLCPContextPriorityRequestNormal; break;
    default: return kCGLBadEnumeration;
    }
    return kCGLNoError;
}

EXPORT CGLError CGLSetVirtualScreen(CGLContextObj ctx, GLint screen)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    return screen == 0 ? kCGLNoError : kCGLBadValue;
}

EXPORT CGLError CGLGetVirtualScreen(CGLContextObj ctx, GLint *screen)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    if (!screen) return kCGLBadAddress;
    *screen = 0;
    return kCGLNoError;
}

EXPORT CGLError CGLLockContext(CGLContextObj ctx)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    pthread_mutex_lock(&ctx->lock);
    return kCGLNoError;
}

EXPORT CGLError CGLUnlockContext(CGLContextObj ctx)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    pthread_mutex_unlock(&ctx->lock);
    return kCGLNoError;
}

/* Full-screen and off-screen (CPU memory) drawables were removed from
   Apple's CGL on Apple silicon; these return what Apple returns there. */
EXPORT CGLError CGLSetFullScreen(CGLContextObj ctx)
{
    return valid_context(ctx) ? kCGLBadFullScreen : kCGLBadContext;
}
EXPORT CGLError CGLSetFullScreenOnDisplay(CGLContextObj ctx, GLuint display_mask)
{
    (void)display_mask;
    return valid_context(ctx) ? kCGLBadFullScreen : kCGLBadContext;
}
EXPORT CGLError CGLSetOffScreen(CGLContextObj ctx, GLsizei width, GLsizei height, GLint rowbytes,
                                void *baseaddr)
{
    (void)width; (void)height; (void)rowbytes; (void)baseaddr;
    return valid_context(ctx) ? kCGLBadOffScreen : kCGLBadContext;
}
EXPORT CGLError CGLGetOffScreen(CGLContextObj ctx, GLsizei *width, GLsizei *height, GLint *rowbytes,
                                void **baseaddr)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    if (width) *width = 0;
    if (height) *height = 0;
    if (rowbytes) *rowbytes = 0;
    if (baseaddr) *baseaddr = NULL;
    return kCGLNoError;
}

/* PBuffers: deprecated since 10.7 and unused by the titles LP32GL targets;
   creation fails cleanly so callers take their FBO paths. */
EXPORT CGLError CGLCreatePBuffer(GLsizei width, GLsizei height, GLenum target, GLenum internalFormat,
                                 GLint max_level, CGLPBufferObj *pbuffer)
{
    (void)width; (void)height; (void)target; (void)internalFormat; (void)max_level;
    if (pbuffer) *pbuffer = NULL;
    return kCGLBadAlloc;
}
EXPORT CGLError CGLDestroyPBuffer(CGLPBufferObj pbuffer) { (void)pbuffer; return kCGLBadAddress; }
EXPORT CGLError CGLDescribePBuffer(CGLPBufferObj obj, GLsizei *width, GLsizei *height, GLenum *target,
                                   GLenum *internalFormat, GLint *mipmap)
{
    (void)obj; (void)width; (void)height; (void)target; (void)internalFormat; (void)mipmap;
    return kCGLBadAddress;
}
EXPORT CGLError CGLTexImagePBuffer(CGLContextObj ctx, CGLPBufferObj pbuffer, GLenum source)
{
    (void)ctx; (void)pbuffer; (void)source;
    return kCGLBadAddress;
}
EXPORT CGLError CGLSetPBuffer(CGLContextObj ctx, CGLPBufferObj pbuffer, GLenum face, GLint level,
                              GLint screen)
{
    (void)ctx; (void)pbuffer; (void)face; (void)level; (void)screen;
    return kCGLBadAddress;
}
EXPORT CGLError CGLGetPBuffer(CGLContextObj ctx, CGLPBufferObj *pbuffer, GLenum *face, GLint *level,
                              GLint *screen)
{
    (void)ctx; (void)face; (void)level; (void)screen;
    if (pbuffer) *pbuffer = NULL;
    return kCGLNoError;
}

EXPORT CGLError CGLSetGlobalOption(CGLGlobalOption pname, const GLint *params)
{
    (void)params;
    switch ((int)pname) {
    case kCGLGOFormatCacheSize: case kCGLGOClearFormatCache: case kCGLGORetainRenderers:
    case kCGLGOResetLibrary: case kCGLGOUseErrorHandler: case kCGLGOUseBuildCache:
        return kCGLNoError;
    default: return kCGLBadEnumeration;
    }
}
EXPORT CGLError CGLGetGlobalOption(CGLGlobalOption pname, GLint *params)
{
    if (!params) return kCGLBadAddress;
    switch ((int)pname) {
    case kCGLGOFormatCacheSize: *params = 5; return kCGLNoError;
    case kCGLGOClearFormatCache: case kCGLGORetainRenderers: case kCGLGOResetLibrary:
    case kCGLGOUseErrorHandler: case kCGLGOUseBuildCache:
        *params = 0;
        return kCGLNoError;
    default: return kCGLBadEnumeration;
    }
}
EXPORT CGLError CGLSetOption(CGLGlobalOption pname, GLint param)
{
    return CGLSetGlobalOption(pname, &param);
}
EXPORT CGLError CGLGetOption(CGLGlobalOption pname, GLint *param)
{
    return CGLGetGlobalOption(pname, param);
}

EXPORT void CGLGetVersion(GLint *majorvers, GLint *minorvers)
{
    if (majorvers) *majorvers = 1;
    if (minorvers) *minorvers = 2;
}

EXPORT const char *CGLErrorString(CGLError error)
{
    switch ((int)error) {
    case kCGLNoError: return "no error";
    case kCGLBadAttribute: return "invalid pixel format attribute";
    case kCGLBadProperty: return "invalid renderer property";
    case kCGLBadPixelFormat: return "invalid pixel format";
    case kCGLBadRendererInfo: return "invalid renderer info";
    case kCGLBadContext: return "invalid context";
    case kCGLBadDrawable: return "invalid drawable";
    case kCGLBadDisplay: return "invalid graphics device";
    case kCGLBadState: return "invalid context state";
    case kCGLBadValue: return "invalid numerical value";
    case kCGLBadMatch: return "invalid share context";
    case kCGLBadEnumeration: return "invalid enumerant";
    case kCGLBadOffScreen: return "invalid offscreen drawable";
    case kCGLBadFullScreen: return "invalid fullscreen drawable";
    case kCGLBadWindow: return "invalid window";
    case kCGLBadAddress: return "invalid pointer";
    case kCGLBadCodeModule: return "invalid code module";
    case kCGLBadAlloc: return "invalid memory allocation";
    case kCGLBadConnection: return "invalid CoreGraphics connection";
    default: return "unknown error";
    }
}

/* ---- name lookup ------------------------------------------------------ */

#define ENTRY(name) {#name, (void *)name}
static const struct { const char *name; void *function; } cgl_entries[] = {
    ENTRY(CGLChoosePixelFormat), ENTRY(CGLDestroyPixelFormat), ENTRY(CGLDescribePixelFormat),
    ENTRY(CGLReleasePixelFormat), ENTRY(CGLRetainPixelFormat), ENTRY(CGLGetPixelFormatRetainCount),
    ENTRY(CGLQueryRendererInfo), ENTRY(CGLDestroyRendererInfo), ENTRY(CGLDescribeRenderer),
    ENTRY(CGLCreateContext), ENTRY(CGLDestroyContext), ENTRY(CGLCopyContext),
    ENTRY(CGLRetainContext), ENTRY(CGLReleaseContext), ENTRY(CGLGetContextRetainCount),
    ENTRY(CGLGetPixelFormat), ENTRY(CGLCreatePBuffer), ENTRY(CGLDestroyPBuffer),
    ENTRY(CGLDescribePBuffer), ENTRY(CGLTexImagePBuffer), ENTRY(CGLSetOffScreen),
    ENTRY(CGLGetOffScreen), ENTRY(CGLSetFullScreen), ENTRY(CGLSetFullScreenOnDisplay),
    ENTRY(CGLSetPBuffer), ENTRY(CGLGetPBuffer), ENTRY(CGLClearDrawable), ENTRY(CGLFlushDrawable),
    ENTRY(CGLEnable), ENTRY(CGLDisable), ENTRY(CGLIsEnabled), ENTRY(CGLSetParameter),
    ENTRY(CGLGetParameter), ENTRY(CGLSetVirtualScreen), ENTRY(CGLGetVirtualScreen),
    ENTRY(CGLUpdateContext), ENTRY(CGLSetGlobalOption), ENTRY(CGLGetGlobalOption),
    ENTRY(CGLSetOption), ENTRY(CGLGetOption), ENTRY(CGLLockContext), ENTRY(CGLUnlockContext),
    ENTRY(CGLGetVersion), ENTRY(CGLErrorString), ENTRY(CGLSetCurrentContext),
    ENTRY(CGLGetCurrentContext), ENTRY(CGLGetShareGroup), ENTRY(LP32CGLSetView),
    ENTRY(LP32CGLGetView),
};

void *lp32gl_cgl_proc_address(const char *name)
{
    for (size_t i = 0; i < sizeof cgl_entries / sizeof cgl_entries[0]; ++i)
        if (!strcmp(name, cgl_entries[i].name)) return cgl_entries[i].function;
    return NULL;
}

const char *lp32gl_metal_device_name(void)
{
    static char name[128];
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        snprintf(name, sizeof name, "%s", device ? device.name.UTF8String : "");
        [device release];
    });
    return name;
}

EXPORT void *lp32gl_get_proc_address(const char *name)
{
    if (!name) return NULL;
    if (name[0] == '_') ++name;
    void *function;
    if (!strncmp(name, "CGL", 3) || !strncmp(name, "LP32CGL", 7))
        return lp32gl_cgl_proc_address(name);
    if (!strncmp(name, "agl", 3)) return lp32gl_agl_proc_address(name);
    if (!strncmp(name, "gl", 2)) {
        if (!lp32gl_initialize()) return NULL;
        function = lp32gl_gl_proc_address(name);
        return function;
    }
    return NULL;
}
