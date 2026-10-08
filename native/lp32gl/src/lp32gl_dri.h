/* The only part of LP32GL that talks to Mesa.  Mesa's DRI entry points are
 * internal to a Mesa build, so they are declared against the pinned revision
 * in native/lp32gl/mesa.rev.  Nothing here includes Apple's OpenGL headers,
 * and nothing outside lp32gl_dri.c includes Mesa's. */
#ifndef LP32GL_DRI_H
#define LP32GL_DRI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What a context is created for.  Apple exposes a legacy (2.1 compatibility)
 * profile and core profiles 3.2 and 4.1. */
enum lp32_dri_api {
    LP32_DRI_API_COMPAT = 0,
    LP32_DRI_API_CORE = 3,
};

struct lp32_dri_config_desc {
    unsigned red, green, blue, alpha;
    unsigned depth, stencil;
    unsigned samples;
    bool double_buffer;
    bool srgb_capable;
    bool float_color;
};

/* A window surface: the CAMetalLayer the swapchain presents to and the size
 * in pixels Mesa should allocate the back buffer at.  Owned by the CGL layer;
 * the DRI layer only reads it through the kopper loader callbacks. */
struct lp32_dri_surface {
    void *metal_layer;           /* CAMetalLayer *, retained by the owner */
    volatile int32_t width;
    volatile int32_t height;
    int swap_interval;
};

bool lp32_dri_initialize(char *error, size_t error_size);

/* Configs are Mesa's; the pointer stays valid for the process lifetime. */
unsigned lp32_dri_config_count(void);
const void *lp32_dri_config_at(unsigned index);
void lp32_dri_describe_config(const void *config, struct lp32_dri_config_desc *out);

void *lp32_dri_create_context(const void *config, void *share, enum lp32_dri_api api,
                              int major, int minor, bool forward_compatible,
                              bool debug, unsigned *error);
void lp32_dri_destroy_context(void *context);

void *lp32_dri_create_window_drawable(const void *config, struct lp32_dri_surface *surface);
void *lp32_dri_create_pbuffer_drawable(const void *config, struct lp32_dri_surface *surface);
void lp32_dri_destroy_drawable(void *drawable);
void lp32_dri_invalidate_drawable(void *drawable);

bool lp32_dri_make_current(void *context, void *draw, void *read);
bool lp32_dri_release_current(void *context);

void lp32_dri_swap_buffers(void *drawable);
void lp32_dri_set_swap_interval(void *drawable, int interval);
void lp32_dri_flush(void *context, void *drawable);

void *lp32_dri_proc_address(const char *name);
const char *lp32_dri_renderer_string(void);

#endif
