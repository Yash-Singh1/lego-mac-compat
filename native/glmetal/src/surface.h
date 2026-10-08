/* Window surfaces (surface.m) used by CGL. */
#ifndef GLM_SURFACE_H
#define GLM_SURFACE_H

#include "glm_internal.h"

struct glm_surface;

/* The surface for an NSView, shared by every context attached to it. */
GLM_HIDDEN struct glm_surface *glm_surface_for_view(void *view);
GLM_HIDDEN void glm_surface_retain(struct glm_surface *surface);
GLM_HIDDEN void glm_surface_release(struct glm_surface *surface);
GLM_HIDDEN void *glm_surface_view(struct glm_surface *surface);
GLM_HIDDEN void *glm_surface_layer(struct glm_surface *surface); /* CAMetalLayer */
/* Pixel size the default framebuffer should have. */
GLM_HIDDEN void glm_surface_size(struct glm_surface *surface, GLsizei *width, GLsizei *height);
/* `backing_size` NULL: follow the view. */
GLM_HIDDEN void glm_surface_configure(struct glm_surface *surface, const GLint *backing_size, GLint opacity);
GLM_HIDDEN void glm_surface_set_swap_interval(struct glm_surface *surface, int interval);

/* metal_backend.m */
GLM_HIDDEN bool glm_backend_present(struct glm_context *ctx, void *layer, bool opaque);
GLM_HIDDEN void glm_backend_default_size(struct glm_context *ctx, GLsizei *width, GLsizei *height);
GLM_HIDDEN void glm_backend_default_framebuffer_resize(struct glm_context *ctx, GLsizei width, GLsizei height);

#endif
