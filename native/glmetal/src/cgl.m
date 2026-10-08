/* CGL: pixel formats, renderer info, contexts and parameters, answering the
 * way Apple's CGL does on Apple silicon (data/apple-cgl-renderer-reference.txt
 * in lp32gl). */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "glm_internal.h"
#include "surface.h"
#include "thread.h"

GLM_HIDDEN void glm_shadow_refresh(struct glm_context *ctx);

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PIXEL_FORMAT_MAGIC 0x474d5046u /* GMPF */
#define CONTEXT_MAGIC 0x474d4358u      /* GMCX */
#define RENDERER_INFO_MAGIC 0x474d5249u
#define RENDERER_ID 0x1027f00
#define SOFTWARE_RENDERER_ID 0x1020400

struct _CGLPixelFormatObject {
    uint32_t magic;
    volatile int32_t refcount;
    enum glm_profile profile;
    GLint color_size, alpha_size, depth_size, stencil_size, samples, display_mask;
    bool double_buffer, color_float, no_recovery, minimum_policy, maximum_policy, closest_policy, backing_store,
         allow_offline;
};

struct _CGLContextObject {
    uint32_t magic;
    volatile int32_t refcount;
    CGLPixelFormatObj pixel_format;
    struct glm_context *gl;
    pthread_mutex_t lock;
    GLint swap_interval, surface_opacity, surface_order, virtual_screen;
    GLint swap_rect[4], backing_size[2];
    bool swap_rect_enabled, backing_size_enabled, rasterization, state_validation, mp_engine, swap_limit,
         display_list_optimization, crash_on_removed_functions, private_enable_1313, private_enable_1314;
    struct glm_surface *surface; /* attached window surface, NULL for none */
    bool had_drawable;           /* viewport initialised from a drawable */
};

static __thread CGLContextObj tls_current;

static bool valid_pixel_format(CGLPixelFormatObj pix) { return pix && pix->magic == PIXEL_FORMAT_MAGIC; }
static bool valid_context(CGLContextObj ctx) { return ctx && ctx->magic == CONTEXT_MAGIC; }

GLM_EXPORT bool glmetal_initialize(void) { return glm_backend_initialize(); }

GLM_EXPORT const char *glmetal_last_error(void)
{
    return glm_backend_device() ? "" : "no Metal device";
}

/* Every gl*, CGL*, agl* and GLM* entry point this library exports. */
GLM_EXPORT void *glmetal_get_proc_address(const char *name)
{
    static void *self;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        Dl_info info;
        if (dladdr((const void *)glmetal_get_proc_address, &info)) self = dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
    });
    return self && name ? dlsym(self, name) : NULL;
}

/* ---- pixel formats ------------------------------------------------------- */

GLM_EXPORT CGLError CGLChoosePixelFormat(const CGLPixelFormatAttribute *attribs, CGLPixelFormatObj *out, GLint *count)
{
    if (!out) return kCGLBadAddress;
    *out = NULL;
    if (count) *count = 0;
    if (!glm_backend_initialize()) return kCGLBadCodeModule;
    struct _CGLPixelFormatObject req = {0};
    GLint profile = kCGLOGLPVersion_Legacy, renderer = 0, sample_buffers = 0, accum = 0, aux = 0;
    bool stereo = false, multisample = false;
    req.display_mask = (GLint)0xffffffff;
    for (const CGLPixelFormatAttribute *a = attribs; a && *a; ++a) {
        switch ((int)*a) {
        case kCGLPFAAllRenderers: case kCGLPFAAuxDepthStencil: case kCGLPFASampleAlpha: case kCGLPFASingleRenderer:
        case kCGLPFARobust: case kCGLPFABackingVolatile: case kCGLPFAMPSafe: case kCGLPFAWindow: case kCGLPFAMultiScreen:
        case kCGLPFACompliant: case kCGLPFAPBuffer: case kCGLPFARemotePBuffer: case kCGLPFAAcceleratedCompute:
        case kCGLPFASupportsAutomaticGraphicsSwitching: case kCGLPFAOffScreen: case kCGLPFAFullScreen:
        case kCGLPFAAccelerated: case kCGLPFASupersample:
            break;
        case kCGLPFATripleBuffer: case kCGLPFADoubleBuffer: req.double_buffer = true; break;
        case kCGLPFAStereo: stereo = true; break;
        case kCGLPFAAuxBuffers: aux = *++a; break;
        case kCGLPFAColorSize: req.color_size = *++a; break;
        case kCGLPFAAlphaSize: req.alpha_size = *++a; break;
        case kCGLPFADepthSize: req.depth_size = *++a; break;
        case kCGLPFAStencilSize: req.stencil_size = *++a; break;
        case kCGLPFAAccumSize: accum = *++a; break;
        case kCGLPFAMinimumPolicy: req.minimum_policy = true; break;
        case kCGLPFAMaximumPolicy: req.maximum_policy = true; break;
        case kCGLPFAClosestPolicy: req.closest_policy = true; break;
        case kCGLPFASampleBuffers: sample_buffers = *++a; break;
        case kCGLPFASamples: req.samples = *++a; break;
        case kCGLPFAColorFloat: req.color_float = true; break;
        case kCGLPFAMultisample: multisample = true; break;
        case kCGLPFARendererID: renderer = *++a; break;
        case kCGLPFANoRecovery: req.no_recovery = true; break;
        case kCGLPFABackingStore: req.backing_store = true; break;
        case kCGLPFADisplayMask: req.display_mask = *++a; break;
        case kCGLPFAAllowOfflineRenderers: req.allow_offline = true; break;
        case kCGLPFAOpenGLProfile: profile = *++a; break;
        case kCGLPFAVirtualScreenCount: ++a; break;
        default:
            glm_log("CGLChoosePixelFormat: unknown attribute %d", (int)*a);
            return kCGLBadAttribute;
        }
    }
    switch (profile) {
    case kCGLOGLPVersion_Legacy: req.profile = GLM_PROFILE_LEGACY; break;
    case kCGLOGLPVersion_3_2_Core: case kCGLOGLPVersion_GL4_Core: req.profile = GLM_PROFILE_CORE; break;
    default: return kCGLBadAttribute;
    }
    /* Only the hardware renderer exists; other renderer IDs, stereo, aux
       buffers and deep accumulation buffers find no format, as on Apple.
       Renderer IDs compare under kCGLRendererIDMatchingMask like Apple's
       (Feral's ports pass 0x27f00, without the vendor byte). */
    if (renderer && (renderer & kCGLRendererIDMatchingMask) != (RENDERER_ID & kCGLRendererIDMatchingMask))
        return kCGLNoError;
    if (stereo || accum > 64 || aux > 0) return kCGLNoError;
    /* Apple silicon: RGBA8 (RGBA16F with color float), 32-bit depth, 8-bit
       stencil, and the request's sample count. */
    req.samples = (sample_buffers > 0 || multisample) ? (req.samples > 0 ? req.samples : 4) : 0;
    if (req.samples > 8) return kCGLNoError;
    struct _CGLPixelFormatObject *pix = calloc(1, sizeof *pix);
    *pix = req;
    pix->magic = PIXEL_FORMAT_MAGIC;
    pix->refcount = 1;
    pix->depth_size = req.depth_size > 0 ? 32 : 0;
    pix->stencil_size = req.stencil_size > 0 ? 8 : 0;
    *out = pix;
    if (count) *count = 1;
    return kCGLNoError;
}

GLM_EXPORT CGLPixelFormatObj CGLRetainPixelFormat(CGLPixelFormatObj pix)
{
    if (valid_pixel_format(pix)) __atomic_add_fetch(&pix->refcount, 1, __ATOMIC_RELAXED);
    return pix;
}

GLM_EXPORT void CGLReleasePixelFormat(CGLPixelFormatObj pix)
{
    if (!valid_pixel_format(pix)) return;
    if (__atomic_sub_fetch(&pix->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
        pix->magic = 0;
        free(pix);
    }
}

GLM_EXPORT CGLError CGLDestroyPixelFormat(CGLPixelFormatObj pix)
{
    if (!pix) return kCGLNoError;
    if (!valid_pixel_format(pix)) return kCGLBadPixelFormat;
    CGLReleasePixelFormat(pix);
    return kCGLNoError;
}

GLM_EXPORT GLuint CGLGetPixelFormatRetainCount(CGLPixelFormatObj pix) { return valid_pixel_format(pix) ? (GLuint)pix->refcount : 0; }

GLM_EXPORT CGLError CGLDescribePixelFormat(CGLPixelFormatObj pix, GLint pix_num, CGLPixelFormatAttribute attrib, GLint *value)
{
    if (!valid_pixel_format(pix)) return kCGLBadPixelFormat;
    if (!value) return kCGLBadAddress;
    if (pix_num != 0) return kCGLBadValue;
    switch ((int)attrib) {
    case kCGLPFADoubleBuffer: *value = pix->double_buffer; break;
    case kCGLPFAColorSize: *value = pix->color_float ? 64 : 32; break;
    case kCGLPFAAlphaSize: *value = pix->color_float ? 16 : 8; break;
    case kCGLPFADepthSize: *value = pix->depth_size; break;
    case kCGLPFAStencilSize: *value = pix->stencil_size; break;
    case kCGLPFAMinimumPolicy: *value = pix->minimum_policy; break;
    case kCGLPFAMaximumPolicy: *value = pix->maximum_policy; break;
    case kCGLPFAOffScreen: *value = 1; break;
    case kCGLPFASampleBuffers: case kCGLPFAMultisample: *value = pix->samples ? 1 : 0; break;
    case kCGLPFASamples: *value = pix->samples; break;
    case kCGLPFAColorFloat: *value = pix->color_float; break;
    case kCGLPFARendererID: *value = RENDERER_ID; break;
    case kCGLPFANoRecovery: *value = pix->no_recovery; break;
    case kCGLPFAAccelerated: case kCGLPFABackingVolatile: case kCGLPFAMPSafe: case kCGLPFAWindow: case kCGLPFAMultiScreen:
    case kCGLPFACompliant: case kCGLPFAAcceleratedCompute: case kCGLPFAVirtualScreenCount:
        *value = 1; break;
    case kCGLPFAClosestPolicy: *value = pix->closest_policy; break;
    case kCGLPFABackingStore: *value = pix->backing_store; break;
    case kCGLPFADisplayMask: *value = pix->display_mask; break;
    case kCGLPFAAllowOfflineRenderers: *value = pix->allow_offline; break;
    case kCGLPFAOpenGLProfile: *value = pix->profile == GLM_PROFILE_CORE ? kCGLOGLPVersion_GL4_Core : kCGLOGLPVersion_Legacy; break;
    case kCGLPFAAllRenderers: case kCGLPFATripleBuffer: case kCGLPFAStereo: case kCGLPFAAuxBuffers: case kCGLPFAAccumSize:
    case kCGLPFAFullScreen: case kCGLPFAAuxDepthStencil: case kCGLPFASupersample: case kCGLPFASampleAlpha:
    case kCGLPFASingleRenderer: case kCGLPFARobust: case kCGLPFAPBuffer: case kCGLPFARemotePBuffer:
    case kCGLPFASupportsAutomaticGraphicsSwitching:
        *value = 0; break;
    default: return kCGLBadAttribute;
    }
    return kCGLNoError;
}

/* ---- renderer info ------------------------------------------------------- */

struct _CGLRendererInfoObject {
    uint32_t magic;
    GLint display_mask;
};

GLM_EXPORT CGLError CGLQueryRendererInfo(GLuint display_mask, CGLRendererInfoObj *rend, GLint *nrend)
{
    if (!rend || !nrend) return kCGLBadAddress;
    struct _CGLRendererInfoObject *info = calloc(1, sizeof *info);
    info->magic = RENDERER_INFO_MAGIC;
    info->display_mask = (GLint)display_mask;
    *rend = info;
    *nrend = 2; /* the GPU and Apple's software renderer */
    return kCGLNoError;
}

GLM_EXPORT CGLError CGLDestroyRendererInfo(CGLRendererInfoObj rend)
{
    if (!rend || rend->magic != RENDERER_INFO_MAGIC) return kCGLBadRendererInfo;
    rend->magic = 0;
    free(rend);
    return kCGLNoError;
}

static GLint video_memory_megabytes(void)
{
    static GLint megabytes;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        id<MTLDevice> device = (__bridge id<MTLDevice>)glm_backend_device();
        unsigned long long bytes = device ? device.recommendedMaxWorkingSetSize : [NSProcessInfo processInfo].physicalMemory * 3 / 4;
        megabytes = (GLint)(bytes / (1024 * 1024));
    });
    return megabytes;
}

GLM_EXPORT CGLError CGLDescribeRenderer(CGLRendererInfoObj rend, GLint rend_num, CGLRendererProperty prop, GLint *value)
{
    if (!rend || rend->magic != RENDERER_INFO_MAGIC) return kCGLBadRendererInfo;
    if (!value) return kCGLBadAddress;
    if (rend_num < 0 || rend_num > 1) return kCGLBadValue;
    bool hw = rend_num == 0;
    switch ((int)prop) {
    case kCGLRPOffScreen: *value = 0; break;
    case kCGLRPFullScreen: *value = hw; break;
    case kCGLRPRendererID: *value = hw ? RENDERER_ID : SOFTWARE_RENDERER_ID; break;
    case kCGLRPAccelerated: *value = hw; break;
    case kCGLRPRobust: *value = !hw; break;
    case kCGLRPBackingStore: case kCGLRPMPSafe: case kCGLRPWindow: case kCGLRPMultiScreen: case kCGLRPCompliant:
    case kCGLRPOnline: case kCGLRPMaxSampleBuffers: case kCGLRPSampleAlpha:
        *value = 1; break;
    case kCGLRPDisplayMask: *value = 0x1f; break;
    case kCGLRPBufferModes: *value = 0xd; break;
    case kCGLRPColorModes: *value = hw ? 0x0a848000 : 0x08008000; break;
    case kCGLRPAccumModes: *value = 0x08000000; break;
    case kCGLRPDepthModes: *value = 0x1001; break;
    case kCGLRPStencilModes: *value = 0x81; break;
    case kCGLRPMaxAuxBuffers: *value = hw ? 2 : 4; break;
    case kCGLRPMaxSamples: *value = hw ? 8 : 16; break;
    case kCGLRPSampleModes: *value = hw ? 3 : 1; break;
    case kCGLRPVideoMemory: case kCGLRPTextureMemory: *value = hw ? 0x7fffffff : 0; break;
    case kCGLRPGPUVertProcCapable: case kCGLRPGPUFragProcCapable: case kCGLRPAcceleratedCompute: *value = hw; break;
    case kCGLRPRendererCount: *value = 2; break;
    case kCGLRPVideoMemoryMegabytes: case kCGLRPTextureMemoryMegabytes: *value = hw ? video_memory_megabytes() : 0; break;
    case kCGLRPMajorGLVersion: *value = 4; break;
    case kCGLRPRegistryIDLow: *value = hw ? 0x663 : 0; break;
    case kCGLRPRegistryIDHigh: *value = hw ? 1 : 0; break;
    case kCGLRPRemovable: *value = 0; break;
    default: return kCGLBadProperty;
    }
    return kCGLNoError;
}

/* ---- contexts ------------------------------------------------------------ */

GLM_EXPORT CGLError CGLCreateContext(CGLPixelFormatObj pix, CGLContextObj share, CGLContextObj *out)
{
    if (!out) return kCGLBadAddress;
    *out = NULL;
    if (!valid_pixel_format(pix)) return kCGLBadPixelFormat;
    if (share && !valid_context(share)) return kCGLBadContext;
    if (share && share->gl->profile != pix->profile) return kCGLBadMatch;
    struct glm_context *gl = glm_context_create(pix->profile, share ? share->gl : NULL);
    if (!gl) return kCGLBadAlloc;
    gl->default_depth_bits = (uint8_t)pix->depth_size;
    gl->default_stencil_bits = (uint8_t)pix->stencil_size;
    gl->default_bits_known = true;
    gl->default_single_buffered = !pix->double_buffer;
    struct _CGLContextObject *ctx = calloc(1, sizeof *ctx);
    ctx->magic = CONTEXT_MAGIC;
    ctx->refcount = 1;
    ctx->pixel_format = CGLRetainPixelFormat(pix);
    ctx->gl = gl;
    ctx->surface_opacity = 1;
    ctx->swap_interval = 1; /* Apple's default: presents wait for vertical sync */
    ctx->surface_order = 1;
    ctx->rasterization = true;
    ctx->display_list_optimization = true;
    ctx->private_enable_1313 = true;
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&ctx->lock, &attributes);
    pthread_mutexattr_destroy(&attributes);
    gl->cgl = ctx;
    *out = ctx;
    /* GLMETAL_THREAD_ALL=1: every context gets a command stream (testing
       the threaded path with surfaceless contexts). */
    const char *all = getenv("GLMETAL_THREAD_ALL");
    if (all && all[0] == '1') {
        glm_shadow_refresh(gl);
        glm_thread_start(gl);
    }
    return kCGLNoError;
}

/* CGL calls that read or change GL state wait for the command stream. */
static void drain(CGLContextObj ctx)
{
    if (ctx && ctx->gl->thread) glm_thread_sync(ctx->gl);
}

static void destroy_context(CGLContextObj ctx)
{
    glm_thread_stop(ctx->gl);
    if (tls_current == ctx) {
        tls_current = NULL;
        glm_set_current(NULL);
    }
    ctx->magic = 0;
    glm_surface_release(ctx->surface);
    ctx->surface = NULL;
    glm_context_destroy(ctx->gl);
    CGLReleasePixelFormat(ctx->pixel_format);
    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
}

GLM_EXPORT CGLContextObj CGLRetainContext(CGLContextObj ctx)
{
    if (valid_context(ctx)) __atomic_add_fetch(&ctx->refcount, 1, __ATOMIC_RELAXED);
    return ctx;
}

GLM_EXPORT void CGLReleaseContext(CGLContextObj ctx)
{
    if (valid_context(ctx) && __atomic_sub_fetch(&ctx->refcount, 1, __ATOMIC_ACQ_REL) == 0) destroy_context(ctx);
}

GLM_EXPORT GLuint CGLGetContextRetainCount(CGLContextObj ctx) { return valid_context(ctx) ? (GLuint)ctx->refcount : 0; }

GLM_EXPORT CGLError CGLDestroyContext(CGLContextObj ctx)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    CGLReleaseContext(ctx);
    return kCGLNoError;
}

GLM_EXPORT CGLPixelFormatObj CGLGetPixelFormat(CGLContextObj ctx) { return valid_context(ctx) ? ctx->pixel_format : NULL; }
GLM_EXPORT CGLShareGroupObj CGLGetShareGroup(CGLContextObj ctx)
{
    return valid_context(ctx) ? (CGLShareGroupObj)(void *)ctx->gl->share : NULL;
}

GLM_EXPORT CGLError CGLSetCurrentContext(CGLContextObj ctx)
{
    if (ctx && !valid_context(ctx)) return kCGLBadContext;
    /* Switching contexts flushes the previous one (GL 2.1 5.7). */
    if (tls_current && tls_current != ctx) {
        if (tls_current->gl->thread) glm_thread_submit(tls_current->gl);
        else glm_backend_flush(tls_current->gl, false);
    }
    tls_current = ctx;
    glm_set_current(ctx ? ctx->gl : NULL);
    return kCGLNoError;
}

GLM_EXPORT CGLContextObj CGLGetCurrentContext(void) { return tls_current; }

GLM_EXPORT CGLError CGLLockContext(CGLContextObj ctx)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    pthread_mutex_lock(&ctx->lock);
    return kCGLNoError;
}

GLM_EXPORT CGLError CGLUnlockContext(CGLContextObj ctx)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    pthread_mutex_unlock(&ctx->lock);
    return kCGLNoError;
}

/* ---- drawables ----------------------------------------------------------- */

/* Sizes the default framebuffer to the surface. The first drawable also
   sets the viewport and scissor box (GL 2.1 2.11.1, 4.1.2). */
static void sync_drawable_size(CGLContextObj ctx)
{
    if (!ctx->surface) return;
    GLsizei width, height, current_width, current_height;
    glm_surface_size(ctx->surface, &width, &height);
    glm_backend_default_size(ctx->gl, &current_width, &current_height);
    if (width == current_width && height == current_height) return;
    glm_backend_default_framebuffer_resize(ctx->gl, width, height);
    ctx->gl->drawable_width = width;
    ctx->gl->drawable_height = height;
    if (!ctx->had_drawable && width > 0 && height > 0) {
        ctx->had_drawable = true;
        GLint box[4] = {0, 0, width, height};
        memcpy(ctx->gl->state.viewport, box, sizeof box);
        memcpy(ctx->gl->state.scissor, box, sizeof box);
        __atomic_add_fetch(&ctx->gl->state_serial, 1, __ATOMIC_RELAXED);
        if (ctx->gl->shadow) glm_shadow_refresh(ctx->gl);
    }
}

static void update_surface(CGLContextObj ctx)
{
    if (!ctx->surface) return;
    glm_surface_configure(ctx->surface, ctx->backing_size_enabled ? ctx->backing_size : NULL, ctx->surface_opacity);
    glm_surface_set_swap_interval(ctx->surface, ctx->swap_interval);
    sync_drawable_size(ctx);
}

static void attach_surface(CGLContextObj ctx, struct glm_surface *surface)
{
    if (ctx->surface == surface) {
        glm_surface_release(surface);
        update_surface(ctx);
        return;
    }
    glm_surface_release(ctx->surface);
    ctx->surface = surface;
    if (surface && !ctx->gl->presents) {
        /* The window-system buffers exist from the first drawable on. */
        struct glm_framebuffer *fb0 = &ctx->gl->default_framebuffer;
        GLenum buffer = ctx->gl->default_single_buffered ? GL_FRONT : GL_BACK;
        if (fb0->draw_buffers[0] == GL_NONE) fb0->draw_buffers[0] = buffer;
        if (fb0->read_buffer == GL_NONE) fb0->read_buffer = buffer;
        ctx->gl->presents = true;
    }
    update_surface(ctx);
    /* Contexts that present get a command stream (thread.h). */
    if (surface && !ctx->gl->thread) {
        glm_shadow_refresh(ctx->gl);
        glm_thread_start(ctx->gl);
    }
}

/* Attaches the context to an NSView (NSOpenGLContext -setView:). */
GLM_EXPORT int GLMCGLSetView(CGLContextObj ctx, void *view)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    drain(ctx);
    if (!view) {
        attach_surface(ctx, NULL);
        return kCGLNoError;
    }
    struct glm_surface *surface = glm_surface_for_view(view);
    if (!surface) return kCGLBadDrawable;
    attach_surface(ctx, surface);
    return kCGLNoError;
}

GLM_EXPORT void *GLMCGLGetView(CGLContextObj ctx) { return valid_context(ctx) ? glm_surface_view(ctx->surface) : NULL; }

static void present_now(CGLContextObj ctx)
{
    if (ctx->surface) {
        sync_drawable_size(ctx);
        if (glm_backend_present(ctx->gl, glm_surface_layer(ctx->surface), ctx->surface_opacity != 0)) return;
    }
    glm_backend_flush(ctx->gl, false);
}

struct present_cmd { CGLContextObj ctx; };
static void present_exec(const void *payload)
{
    CGLContextObj ctx = ((const struct present_cmd *)payload)->ctx;
    present_now(ctx);
    glm_thread_frame_executed(ctx->gl);
}

GLM_EXPORT CGLError CGLFlushDrawable(CGLContextObj ctx)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    if (ctx->gl->thread) {
        /* Recorded: the application's thread goes on with the next frame
           while the worker encodes and presents this one. */
        struct present_cmd *c = glm_thread_alloc(ctx->gl, sizeof *c, present_exec);
        c->ctx = ctx;
        glm_thread_frame_recorded(ctx->gl);
        return kCGLNoError;
    }
    if (ctx->surface) {
        sync_drawable_size(ctx);
        if (glm_backend_present(ctx->gl, glm_surface_layer(ctx->surface), ctx->surface_opacity != 0)) return kCGLNoError;
    }
    glm_backend_flush(ctx->gl, false);
    return kCGLNoError;
}

GLM_EXPORT CGLError CGLClearDrawable(CGLContextObj ctx)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    drain(ctx);
    attach_surface(ctx, NULL);
    return kCGLNoError;
}

GLM_EXPORT CGLError CGLUpdateContext(CGLContextObj ctx)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    drain(ctx);
    update_surface(ctx);
    return kCGLNoError;
}

/* Apple's CGLEnable/CGLDisable accept any value and keep state for the
   enables it knows, including private ones: Source's togl enables 1314
   (multithreaded GL) and falls back to kCGLCEMPEngine when that fails.
   Unknown values report kCGLBadEnumeration (and 0) from CGLIsEnabled only. */
static bool *enable_slot(CGLContextObj ctx, int pname)
{
    switch (pname) {
    case kCGLCESwapRectangle: return &ctx->swap_rect_enabled;
    case kCGLCERasterization: return &ctx->rasterization;
    case kCGLCEStateValidation: return &ctx->state_validation;
    case kCGLCESurfaceBackingSize: return &ctx->backing_size_enabled;
    case kCGLCEMPEngine: return &ctx->mp_engine;
    case kCGLCESwapLimit: return &ctx->swap_limit;
    case kCGLCEDisplayListOptimization: return &ctx->display_list_optimization;
    case kCGLCECrashOnRemovedFunctions: return &ctx->crash_on_removed_functions;
    case 1313: return &ctx->private_enable_1313;
    case 1314: return &ctx->private_enable_1314;
    default: return NULL;
    }
}

static CGLError set_enable(CGLContextObj ctx, CGLContextEnable pname, bool value)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    drain(ctx);
    bool *slot = enable_slot(ctx, (int)pname);
    if (!slot) return kCGLNoError;
    *slot = value;
    if ((int)pname == kCGLCESurfaceBackingSize) update_surface(ctx);
    return kCGLNoError;
}

GLM_EXPORT CGLError CGLEnable(CGLContextObj ctx, CGLContextEnable pname) { return set_enable(ctx, pname, true); }
GLM_EXPORT CGLError CGLDisable(CGLContextObj ctx, CGLContextEnable pname) { return set_enable(ctx, pname, false); }

GLM_EXPORT CGLError CGLIsEnabled(CGLContextObj ctx, CGLContextEnable pname, GLint *enable)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    if (!enable) return kCGLBadAddress;
    bool *slot = enable_slot(ctx, (int)pname);
    *enable = slot ? *slot : 0;
    return slot ? kCGLNoError : kCGLBadEnumeration;
}

GLM_EXPORT CGLError CGLSetParameter(CGLContextObj ctx, CGLContextParameter pname, const GLint *params)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    drain(ctx);
    if (!params) return kCGLBadAddress;
    switch ((int)pname) {
    case kCGLCPSwapRectangle: memcpy(ctx->swap_rect, params, sizeof ctx->swap_rect); break;
    case kCGLCPSwapInterval:
        ctx->swap_interval = params[0] ? 1 : 0;
        glm_surface_set_swap_interval(ctx->surface, ctx->swap_interval);
        break;
    case kCGLCPSurfaceOrder: ctx->surface_order = params[0]; update_surface(ctx); break;
    case kCGLCPSurfaceOpacity: ctx->surface_opacity = params[0]; update_surface(ctx); break;
    case kCGLCPSurfaceBackingSize:
        memcpy(ctx->backing_size, params, sizeof ctx->backing_size);
        if (ctx->backing_size_enabled) update_surface(ctx);
        break;
    case kCGLCPReclaimResources: case kCGLCPDispatchTableSize: case kCGLCPSurfaceSurfaceVolatile:
    case kCGLCPGPUVertexProcessing: case kCGLCPGPUFragmentProcessing: case kCGLCPHasDrawable: case kCGLCPMPSwapsInFlight:
        break;
    default: return kCGLBadEnumeration;
    }
    return kCGLNoError;
}

GLM_EXPORT CGLError CGLGetParameter(CGLContextObj ctx, CGLContextParameter pname, GLint *params)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    if (!params) return kCGLBadAddress;
    switch ((int)pname) {
    case kCGLCPSwapRectangle: memcpy(params, ctx->swap_rect, sizeof ctx->swap_rect); break;
    case kCGLCPSwapInterval: params[0] = ctx->swap_interval; break;
    case kCGLCPSurfaceOrder: params[0] = ctx->surface_order; break;
    case kCGLCPSurfaceOpacity: params[0] = ctx->surface_opacity; break;
    case kCGLCPSurfaceBackingSize: memcpy(params, ctx->backing_size, sizeof ctx->backing_size); break;
    case kCGLCPGPUVertexProcessing: case kCGLCPGPUFragmentProcessing: params[0] = 1; break;
    case kCGLCPHasDrawable: params[0] = ctx->surface != NULL; break;
    case kCGLCPMPSwapsInFlight: params[0] = 1; break;
    case kCGLCPCurrentRendererID: params[0] = RENDERER_ID; break;
    default: {
        /* The other parameters Apple's CGLGetParameter answers (many
           private) with the values it gives a new context. Source's
           launcher takes a successful query of 1314 to mean Apple's
           multithreaded GL engine is present. */
        static const struct { short pname, count; GLint value[2]; } apple[] = {
            {201, 1, {0}}, {203, 1, {0}}, {221, 1, {1}}, {224, 1, {7800}}, {226, 2, {0, 0}}, {228, 1, {0}},
            {301, 1, {0}}, {305, 1, {0}}, {306, 1, {0}}, {307, 1, {1}}, {313, 1, {0}}, {316, 1, {0}}, {317, 1, {0}},
            {318, 1, {1}}, {319, 1, {1}}, {350, 1, {0}}, {352, 1, {0}}, {354, 1, {0}}, {356, 1, {0}}, {358, 1, {0}},
            {360, 1, {0}}, {362, 1, {0}}, {364, 1, {0}}, {370, 1, {0}}, {372, 1, {0}}, {374, 1, {0}}, {376, 1, {0}},
            {380, 1, {0}}, {382, 1, {0}}, {384, 1, {0}}, {386, 1, {0}}, {388, 1, {0}}, {390, 1, {0}}, {392, 1, {0}},
            {394, 1, {0}}, {399, 1, {0}}, {608, 1, {1}}, {666, 1, {1}}, {667, 1, {1}}, {701, 1, {16}}, {982, 1, {0}},
            {986, 1, {0}}, {992, 1, {0}}, {1235, 1, {0}}, {1236, 2, {0, 0}}, {1310, 1, {1}}, {1311, 1, {1}},
            {1312, 2, {0, 0}}, {1313, 2, {0, 0}}, {1314, 1, {0}}, {1402, 1, {0}}, {1403, 2, {0, 0}}, {1405, 2, {0, 0}},
            {1460, 2, {0, 0}}, {1461, 2, {0, 0}}, {1462, 2, {0, 0}}};
        for (size_t i = 0; i < sizeof apple / sizeof apple[0]; ++i)
            if (apple[i].pname == (int)pname) {
                memcpy(params, apple[i].value, (size_t)apple[i].count * sizeof(GLint));
                return kCGLNoError;
            }
        return kCGLBadEnumeration;
    }
    }
    return kCGLNoError;
}

GLM_EXPORT CGLError CGLSetVirtualScreen(CGLContextObj ctx, GLint screen)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    if (screen != 0) return kCGLBadValue;
    ctx->virtual_screen = screen;
    return kCGLNoError;
}

GLM_EXPORT CGLError CGLGetVirtualScreen(CGLContextObj ctx, GLint *screen)
{
    if (!valid_context(ctx)) return kCGLBadContext;
    if (!screen) return kCGLBadAddress;
    *screen = ctx->virtual_screen;
    return kCGLNoError;
}

GLM_EXPORT CGLError CGLCopyContext(CGLContextObj src, CGLContextObj dst, GLbitfield mask)
{
    if (!valid_context(src) || !valid_context(dst)) return kCGLBadContext;
    drain(src);
    drain(dst);
    (void)mask;
    dst->gl->state = src->gl->state;
    return kCGLNoError;
}

GLM_EXPORT CGLError CGLSetFullScreen(CGLContextObj ctx) { (void)ctx; return kCGLBadCodeModule; }
GLM_EXPORT CGLError CGLSetFullScreenOnDisplay(CGLContextObj ctx, GLuint mask) { (void)ctx; (void)mask; return kCGLBadCodeModule; }
GLM_EXPORT CGLError CGLSetOffScreen(CGLContextObj ctx, GLsizei w, GLsizei h, GLint rowbytes, void *baseaddr)
{ (void)ctx; (void)w; (void)h; (void)rowbytes; (void)baseaddr; return kCGLBadCodeModule; }
GLM_EXPORT CGLError CGLGetOffScreen(CGLContextObj ctx, GLsizei *w, GLsizei *h, GLint *rowbytes, void **baseaddr)
{
    (void)ctx;
    if (w) *w = 0;
    if (h) *h = 0;
    if (rowbytes) *rowbytes = 0;
    if (baseaddr) *baseaddr = NULL;
    return kCGLNoError;
}

static GLint global_options[8];

GLM_EXPORT CGLError CGLSetGlobalOption(CGLGlobalOption pname, const GLint *params)
{
    if (!params) return kCGLBadAddress;
    if ((int)pname >= 0 && (int)pname < 8) global_options[pname] = params[0];
    return kCGLNoError;
}

GLM_EXPORT CGLError CGLGetGlobalOption(CGLGlobalOption pname, GLint *params)
{
    if (!params) return kCGLBadAddress;
    params[0] = (int)pname >= 0 && (int)pname < 8 ? global_options[pname] : 0;
    return kCGLNoError;
}
GLM_EXPORT CGLError CGLSetOption(CGLGlobalOption pname, GLint param) { return CGLSetGlobalOption(pname, &param); }
GLM_EXPORT CGLError CGLGetOption(CGLGlobalOption pname, GLint *param) { return CGLGetGlobalOption(pname, param); }

GLM_EXPORT void CGLGetVersion(GLint *major, GLint *minor)
{
    if (major) *major = 1;
    if (minor) *minor = 2;
}

GLM_EXPORT const char *CGLErrorString(CGLError error)
{
    switch (error) {
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

/* Pbuffers are gone from Apple's modern GL; report no support. */
GLM_EXPORT CGLError CGLCreatePBuffer(GLsizei w, GLsizei h, GLenum target, GLenum format, GLint max_level, CGLPBufferObj *pbuffer)
{ (void)w; (void)h; (void)target; (void)format; (void)max_level; if (pbuffer) *pbuffer = NULL; return kCGLBadAlloc; }
GLM_EXPORT CGLError CGLDestroyPBuffer(CGLPBufferObj pbuffer) { (void)pbuffer; return kCGLBadAddress; }

GLM_HIDDEN struct glm_context *glm_cgl_gl(CGLContextObj ctx) { return valid_context(ctx) ? ctx->gl : NULL; }
