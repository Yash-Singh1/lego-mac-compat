/* Mesa bring-up: one kopper (Zink) DRI screen per process, contexts created
 * through driCreateContextAttribs, and window drawables whose Vulkan surface
 * is a VkMetalSurfaceCreateInfoEXT for the owner's CAMetalLayer. */
#include "lp32gl_dri.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_metal.h>
#include "mesa_interface.h"
#include "kopper_interface.h"

/* From src/gallium/frontends/dri/dri_util.h at the pinned revision. */
enum dri_screen_type {
    DRI_SCREEN_DRI3,
    DRI_SCREEN_KOPPER,
    DRI_SCREEN_SWRAST,
    DRI_SCREEN_KMS_SWRAST,
};
struct dri_screen;
struct dri_context;
struct dri_drawable;
struct dri_config;

struct dri_screen *driCreateNewScreen3(int scrn, int fd, const __DRIextension **loader_extensions,
                                       enum dri_screen_type type,
                                       const struct dri_config ***driver_configs,
                                       bool driver_name_is_inferred, bool has_multibuffer,
                                       void *data);
struct dri_context *driCreateContextAttribs(struct dri_screen *screen, int api,
                                            const struct dri_config *config,
                                            struct dri_context *shared, unsigned num_attribs,
                                            const uint32_t *attribs, unsigned *error,
                                            void *data, bool thread_safe);
void driDestroyContext(struct dri_context *context);
struct dri_drawable *dri_create_drawable(struct dri_screen *screen, const struct dri_config *config,
                                         bool is_pixmap, void *loader_private);
void driDestroyDrawable(struct dri_drawable *drawable);
int driBindContext(struct dri_context *context, struct dri_drawable *draw, struct dri_drawable *read);
int driUnbindContext(struct dri_context *context);
int driGetConfigAttrib(const struct dri_config *config, unsigned attrib, unsigned *value);
int64_t kopperSwapBuffers(struct dri_drawable *drawable, uint32_t flush_flags);
void kopperSetSwapInterval(struct dri_drawable *drawable, int interval);
void dri_flush(struct dri_context *context, struct dri_drawable *drawable, unsigned flags,
               enum __DRI2throttleReason reason);
void dri_invalidate_drawable(struct dri_drawable *drawable);
int dri_query_renderer_string(struct dri_screen *screen, int param, const char **value);
void *_mesa_glapi_get_proc_address(const char *name);

static struct dri_screen *screen;
static const struct dri_config **configs;
static unsigned config_count;
static char renderer[256];

static void set_surface_create_info(void *loader_private, struct kopper_loader_info *out)
{
    struct lp32_dri_surface *surface = loader_private;
    VkMetalSurfaceCreateInfoEXT *metal = (VkMetalSurfaceCreateInfoEXT *)&out->bos;
    _Static_assert(sizeof(VkMetalSurfaceCreateInfoEXT) <= sizeof(out->bos),
                   "kopper surface storage too small for VkMetalSurfaceCreateInfoEXT");
    if (!surface || !surface->metal_layer) {
        /* A pbuffer: no swapchain, Mesa allocates plain textures. */
        memset(&out->bos, 0, sizeof(out->bos));
        return;
    }
    memset(metal, 0, sizeof(*metal));
    metal->sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    metal->pLayer = surface->metal_layer;
    out->initial_swap_interval = surface->swap_interval;
    out->present_opaque = true;
}

static void get_drawable_info(struct dri_drawable *drawable, int *width, int *height,
                              void *closure)
{
    (void)drawable;
    struct lp32_dri_surface *surface = closure;
    *width = surface ? surface->width : 0;
    *height = surface ? surface->height : 0;
}

static const __DRIkopperLoaderExtension kopper_loader = {
    .base = {__DRI_KOPPER_LOADER, 1},
    .SetSurfaceCreateInfo = set_surface_create_info,
    .GetDrawableInfo = get_drawable_info,
};
static const __DRIextension *loader_extensions[] = {&kopper_loader.base, NULL};

static char init_error[256];
static bool init_ok;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

static void initialize(void)
{
    /* Mesa reads these once at screen creation.  The Apple profile limits are
       enforced by LP32GL itself, so Mesa is asked for everything it has. */
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 0);
    screen = driCreateNewScreen3(0, -1, loader_extensions, DRI_SCREEN_KOPPER, &configs,
                                 false, true, NULL);
    if (!screen) {
        snprintf(init_error, sizeof init_error,
                 "Zink could not create a screen on the Vulkan driver");
        return;
    }
    while (configs && configs[config_count]) ++config_count;
    const char *value = NULL;
    /* __DRI2_RENDERER_DEVICE_ID-style queries are optional; fall back quietly. */
    if (dri_query_renderer_string(screen, 0x0001 /* __DRI2_RENDERER_QUERY_RENDERER */, &value) == 0 &&
        value)
        snprintf(renderer, sizeof renderer, "%s", value);
    init_ok = config_count > 0;
    if (!init_ok) snprintf(init_error, sizeof init_error, "Zink exposed no framebuffer configs");
}

bool lp32_dri_initialize(char *error, size_t error_size)
{
    pthread_once(&init_once, initialize);
    if (!init_ok && error && error_size) snprintf(error, error_size, "%s", init_error);
    return init_ok;
}

unsigned lp32_dri_config_count(void) { return config_count; }
const void *lp32_dri_config_at(unsigned index)
{
    return index < config_count ? configs[index] : NULL;
}

static unsigned attrib(const void *config, unsigned name)
{
    unsigned value = 0;
    return driGetConfigAttrib(config, name, &value) ? value : 0;
}

void lp32_dri_describe_config(const void *config, struct lp32_dri_config_desc *out)
{
    memset(out, 0, sizeof *out);
    if (!config) return;
    out->red = attrib(config, __DRI_ATTRIB_RED_SIZE);
    out->green = attrib(config, __DRI_ATTRIB_GREEN_SIZE);
    out->blue = attrib(config, __DRI_ATTRIB_BLUE_SIZE);
    out->alpha = attrib(config, __DRI_ATTRIB_ALPHA_SIZE);
    out->depth = attrib(config, __DRI_ATTRIB_DEPTH_SIZE);
    out->stencil = attrib(config, __DRI_ATTRIB_STENCIL_SIZE);
    out->samples = attrib(config, __DRI_ATTRIB_SAMPLE_BUFFERS) ?
        attrib(config, __DRI_ATTRIB_SAMPLES) : 0;
    out->double_buffer = attrib(config, __DRI_ATTRIB_DOUBLE_BUFFER) != 0;
    out->srgb_capable = attrib(config, __DRI_ATTRIB_FRAMEBUFFER_SRGB_CAPABLE) != 0;
    out->float_color = (attrib(config, __DRI_ATTRIB_RENDER_TYPE) & __DRI_ATTRIB_FLOAT_BIT) != 0;
}

void *lp32_dri_create_context(const void *config, void *share, enum lp32_dri_api api,
                              int major, int minor, bool forward_compatible, bool debug,
                              unsigned *error)
{
    uint32_t attribs[8];
    unsigned count = 0;
    uint32_t flags = (forward_compatible ? __DRI_CTX_FLAG_FORWARD_COMPATIBLE : 0) |
                     (debug ? __DRI_CTX_FLAG_DEBUG : 0);
    attribs[count++] = __DRI_CTX_ATTRIB_MAJOR_VERSION;
    attribs[count++] = (uint32_t)major;
    attribs[count++] = __DRI_CTX_ATTRIB_MINOR_VERSION;
    attribs[count++] = (uint32_t)minor;
    attribs[count++] = __DRI_CTX_ATTRIB_FLAGS;
    attribs[count++] = flags;
    unsigned local_error = 0;
    struct dri_context *context =
        driCreateContextAttribs(screen, (int)api, config, share, count / 2, attribs,
                                &local_error, NULL, true);
    if (error) *error = local_error;
    return context;
}

void lp32_dri_destroy_context(void *context)
{
    if (context) driDestroyContext(context);
}

void *lp32_dri_create_window_drawable(const void *config, struct lp32_dri_surface *surface)
{
    return dri_create_drawable(screen, config, false, surface);
}

void *lp32_dri_create_pbuffer_drawable(const void *config, struct lp32_dri_surface *surface)
{
    return dri_create_drawable(screen, config, false, surface);
}

void lp32_dri_destroy_drawable(void *drawable)
{
    if (drawable) driDestroyDrawable(drawable);
}

void lp32_dri_invalidate_drawable(void *drawable)
{
    if (drawable) dri_invalidate_drawable(drawable);
}

bool lp32_dri_make_current(void *context, void *draw, void *read)
{
    return driBindContext(context, draw, read) != 0;
}

bool lp32_dri_release_current(void *context)
{
    return context ? driUnbindContext(context) != 0 : true;
}

void lp32_dri_swap_buffers(void *drawable)
{
    if (drawable) kopperSwapBuffers(drawable, 0);
}

void lp32_dri_set_swap_interval(void *drawable, int interval)
{
    if (drawable) kopperSetSwapInterval(drawable, interval);
}

void lp32_dri_flush(void *context, void *drawable)
{
    if (context) dri_flush(context, drawable, __DRI2_FLUSH_CONTEXT, __DRI2_THROTTLE_FLUSHFRONT);
}

void *lp32_dri_proc_address(const char *name)
{
    return _mesa_glapi_get_proc_address(name);
}

const char *lp32_dri_renderer_string(void)
{
    return renderer[0] ? renderer : NULL;
}
