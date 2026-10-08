/* Shared between LP32GL's CGL, AGL, NSOpenGL and GL layers. */
#ifndef LP32GL_INTERNAL_H
#define LP32GL_INTERNAL_H

#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/CGLTypes.h>
#include <OpenGL/gltypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "lp32gl.h"
#include "lp32gl_dri.h"

#define LP32GL_HIDDEN __attribute__((visibility("hidden")))

enum lp32gl_profile {
    LP32GL_PROFILE_LEGACY,  /* reports 2.1, backed by a 4.1 compatibility context */
    LP32GL_PROFILE_CORE,    /* reports 4.1 core */
};

struct _CGLPixelFormatObject {
    uint32_t magic;
    volatile int32_t refcount;
    const void *config;          /* Mesa dri_config */
    struct lp32_dri_config_desc desc;
    enum lp32gl_profile profile;
    /* Requested values, reported back by CGLDescribePixelFormat where Apple
       reports the request rather than the renderer's native format. */
    GLint color_size, alpha_size, depth_size, stencil_size, accum_size;
    GLint sample_buffers, samples, aux_buffers;
    bool double_buffer, stereo, color_float, accelerated, no_recovery;
    bool multisample, supersample, backing_store, allow_offline;
    bool window, offscreen, fullscreen, pbuffer, remote_pbuffer, accelerated_compute;
    bool mp_safe, multiscreen, compliant, closest_policy, minimum_policy, maximum_policy;
    GLint renderer_id, display_mask, profile_attribute;
};

struct lp32gl_share_group {
    volatile int32_t refcount;
    void *root_dri; /* first context; Mesa shares objects through it */
};
struct lp32gl_surface;

struct _CGLContextObject {
    uint32_t magic;
    volatile int32_t refcount;
    CGLPixelFormatObj pixel_format;
    struct lp32gl_share_group *share_group;
    void *dri;                   /* Mesa dri_context */
    CGLContextObj mirror_context; /* public context mirrored by a worker clone */
    enum lp32gl_profile profile;
    pthread_mutex_t lock;        /* CGLLockContext; recursive */
    pthread_mutex_t state;       /* guards surface/current_thread below */
    struct lp32gl_surface *surface;       /* attached drawable (NULL: none) */
    struct lp32gl_surface *bound_surface; /* what Mesa has bound */
    pthread_t current_thread;
    bool current;
    GLint swap_interval;
    GLint surface_opacity, surface_order, surface_volatile;
    GLint backing_size[2];
    bool backing_size_enabled;
    GLint swap_rect[4];
    bool swap_rect_enabled, rasterization, state_validation, mp_engine;
    GLint virtual_screen;
    void *gl_state;              /* lp32gl_gl.c per-context emulation state */
    void *view;                  /* NSView *, weak; see LP32CGLSetView */
};

/* lp32gl_cgl.m */
LP32GL_HIDDEN CGLContextObj lp32gl_current_context(void);
LP32GL_HIDDEN void lp32gl_set_error(const char *format, ...);
LP32GL_HIDDEN void lp32gl_context_attach_surface(CGLContextObj context, struct lp32gl_surface *surface);
LP32GL_HIDDEN void lp32gl_context_update(CGLContextObj context);
LP32GL_HIDDEN CGLError lp32gl_choose(const CGLPixelFormatAttribute *attributes,
                                     CGLPixelFormatObj *out, GLint *count);

/* lp32gl_surface.m */
LP32GL_HIDDEN struct lp32gl_surface *lp32gl_surface_for_view(void *view, const void *config);
LP32GL_HIDDEN void lp32gl_surface_retain(struct lp32gl_surface *surface);
LP32GL_HIDDEN void lp32gl_surface_release(struct lp32gl_surface *surface);
LP32GL_HIDDEN void *lp32gl_surface_drawable(struct lp32gl_surface *surface);
LP32GL_HIDDEN void lp32gl_surface_configure(struct lp32gl_surface *surface, const GLint *backing_size,
                                            bool best_resolution, GLint opacity, GLint order);
LP32GL_HIDDEN void lp32gl_surface_set_swap_interval(struct lp32gl_surface *surface, int interval);
LP32GL_HIDDEN void *lp32gl_surface_view(struct lp32gl_surface *surface);

/* lp32gl_gl.c */
LP32GL_HIDDEN void *lp32gl_gl_proc_address(const char *name);
LP32GL_HIDDEN void *lp32gl_gl_state_create(CGLContextObj context);
LP32GL_HIDDEN void lp32gl_gl_state_destroy(void *state);
LP32GL_HIDDEN void lp32gl_gl_context_made_current(CGLContextObj context);
LP32GL_HIDDEN void lp32gl_gl_context_presented(CGLContextObj context);

/* lp32gl_exports.c */
LP32GL_HIDDEN void lp32gl_exports_resolve(void);

/* lp32gl_agl.c */
LP32GL_HIDDEN void *lp32gl_agl_proc_address(const char *name);
/* lp32gl_cgl.m */
LP32GL_HIDDEN void *lp32gl_cgl_proc_address(const char *name);

#define LP32GL_PIXEL_FORMAT_MAGIC 0x4c504658u /* 'LPFX' */
#define LP32GL_CONTEXT_MAGIC 0x4c504358u      /* 'LPCX' */

/* Apple's values on Apple silicon (M-series), captured from OpenGL.framework
   on macOS 26; see data/apple-gl-reference.txt. */
#define LP32GL_RENDERER_ID 0x01027f00
#define LP32GL_SOFTWARE_RENDERER_ID 0x01020400

#endif
