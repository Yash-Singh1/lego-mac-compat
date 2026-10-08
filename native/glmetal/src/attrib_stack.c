/* glPushAttrib / glPopAttrib and the client attribute stack (GL 2.1 6.1.14).
 * A push saves the whole state; a pop restores only the groups in the
 * pushed mask. */
#include "glm_internal.h"

#include <stdlib.h>
#include <string.h>

struct saved_attrib {
    struct glm_state state;
    GLenum draw_buffers[GLM_MAX_DRAW_BUFFERS];
};

static void restore(struct glm_context *ctx, const struct saved_attrib *saved, GLbitfield mask)
{
    struct glm_state *s = &ctx->state;
    const struct glm_state *o = &saved->state;
#define COPY(field) memcpy(&s->field, &o->field, sizeof s->field)
    if (mask & GL_CURRENT_BIT) {
        COPY(current);
        COPY(raster_pos);
        COPY(raster_valid);
    }
    if (mask & GL_POINT_BIT) { COPY(point_size); COPY(point_sprite); }
    if (mask & GL_LINE_BIT) { COPY(line_width); COPY(line_smooth); }
    if (mask & GL_POLYGON_BIT) {
        COPY(cull_face); COPY(cull_mode); COPY(front_face); COPY(polygon_mode); COPY(polygon_smooth);
        COPY(polygon_offset_fill); COPY(polygon_offset_line); COPY(polygon_offset_point);
        COPY(polygon_offset_factor); COPY(polygon_offset_units);
    }
    if (mask & GL_LIGHTING_BIT) {
        COPY(lighting); COPY(lights); COPY(material); COPY(light_model_ambient); COPY(light_model_local_viewer);
        COPY(light_model_two_side); COPY(light_model_separate_specular); COPY(color_material);
        COPY(color_material_face); COPY(color_material_mode); COPY(shade_model);
    }
    if (mask & GL_FOG_BIT) {
        COPY(fog); COPY(fog_mode); COPY(fog_coord_source); COPY(fog_color); COPY(fog_density); COPY(fog_start); COPY(fog_end);
    }
    if (mask & GL_DEPTH_BUFFER_BIT) { COPY(depth_test); COPY(depth_func); COPY(depth_mask); COPY(clear_depth); }
    if (mask & GL_STENCIL_BUFFER_BIT) { COPY(stencil_test); COPY(stencil); COPY(clear_stencil); }
    if (mask & GL_COLOR_BUFFER_BIT) {
        COPY(alpha_test); COPY(alpha_func); COPY(alpha_ref); COPY(blend); COPY(blend_color); COPY(dither);
        COPY(color_logic_op); COPY(logic_op); COPY(color_mask); COPY(clear_color); COPY(framebuffer_srgb);
        struct glm_framebuffer *fb = glm_framebuffer_get(ctx, ctx->draw_framebuffer);
        if (fb) memcpy(fb->draw_buffers, saved->draw_buffers, sizeof fb->draw_buffers);
    }
    if (mask & GL_VIEWPORT_BIT) { COPY(viewport); COPY(depth_range); }
    if (mask & GL_SCISSOR_BIT) { COPY(scissor_test); COPY(scissor); }
    if (mask & GL_TRANSFORM_BIT) {
        COPY(matrix_mode); COPY(clip_plane_enabled); COPY(clip_planes); COPY(normalize); COPY(rescale_normal);
        COPY(depth_clamp);
    }
    if (mask & GL_ENABLE_BIT) {
        COPY(alpha_test); COPY(blend); COPY(clip_plane_enabled); COPY(color_material); COPY(cull_face);
        COPY(depth_test); COPY(dither); COPY(fog); COPY(lighting); COPY(line_smooth); COPY(color_logic_op);
        COPY(normalize); COPY(rescale_normal); COPY(point_sprite); COPY(polygon_offset_fill); COPY(polygon_offset_line);
        COPY(polygon_offset_point); COPY(polygon_smooth); COPY(scissor_test); COPY(stencil_test);
        COPY(multisample); COPY(sample_alpha_to_coverage); COPY(vertex_program); COPY(fragment_program); COPY(vertex_program_two_side);
        COPY(depth_clamp); COPY(primitive_restart); COPY(program_point_size); COPY(framebuffer_srgb);
        for (int i = 0; i < GLM_MAX_LIGHTS; ++i) s->lights[i].enabled = o->lights[i].enabled;
        for (int u = 0; u < GLM_MAX_TEXTURE_UNITS; ++u) {
            s->units[u].enabled_1d = o->units[u].enabled_1d;
            s->units[u].enabled_2d = o->units[u].enabled_2d;
            s->units[u].enabled_3d = o->units[u].enabled_3d;
            s->units[u].enabled_cube = o->units[u].enabled_cube;
            s->units[u].enabled_rect = o->units[u].enabled_rect;
            for (int c = 0; c < 4; ++c) s->units[u].gen[c].enabled = o->units[u].gen[c].enabled;
        }
    }
    if (mask & GL_TEXTURE_BIT) {
        COPY(active_texture);
        COPY(units);
    }
    if (mask & GL_PIXEL_MODE_BIT) COPY(pixel_zoom);
#undef COPY
}

GLM_EXPORT void glPushAttrib(GLbitfield mask)
{
    GLM_CONTEXT(ctx);
    if (ctx->attrib_depth >= 16) return glm_error(ctx, GL_STACK_OVERFLOW);
    struct saved_attrib *saved = malloc(sizeof *saved);
    saved->state = ctx->state;
    struct glm_framebuffer *fb = glm_framebuffer_get(ctx, ctx->draw_framebuffer);
    if (fb) memcpy(saved->draw_buffers, fb->draw_buffers, sizeof saved->draw_buffers);
    ctx->attrib_stack[ctx->attrib_depth] = saved;
    ctx->attrib_masks[ctx->attrib_depth] = mask;
    ++ctx->attrib_depth;
}

GLM_EXPORT void glPopAttrib(void)
{
    GLM_CONTEXT(ctx);
    if (ctx->attrib_depth <= 0) return glm_error(ctx, GL_STACK_UNDERFLOW);
    --ctx->attrib_depth;
    struct saved_attrib *saved = ctx->attrib_stack[ctx->attrib_depth];
    restore(ctx, saved, ctx->attrib_masks[ctx->attrib_depth]);
    free(saved);
    glm_backend_framebuffer_changed(ctx);
}

/* ---- client attributes --------------------------------------------------- */

struct saved_client {
    GLbitfield mask;
    struct glm_vertex_array arrays;
    GLuint array_buffer;
    struct glm_state pixel;
};

static __thread struct saved_client *client_stack[16];
static __thread int client_depth;
int glm_client_attrib_depth(void) { return client_depth; }

GLM_EXPORT void glPushClientAttrib(GLbitfield mask)
{
    GLM_CONTEXT(ctx);
    if (client_depth >= 16) return glm_error(ctx, GL_STACK_OVERFLOW);
    struct saved_client *saved = malloc(sizeof *saved);
    saved->mask = mask;
    saved->arrays = *ctx->vao;
    saved->array_buffer = ctx->array_buffer;
    saved->pixel = ctx->state;
    client_stack[client_depth++] = saved;
}

GLM_EXPORT void glPopClientAttrib(void)
{
    GLM_CONTEXT(ctx);
    if (client_depth <= 0) return glm_error(ctx, GL_STACK_UNDERFLOW);
    struct saved_client *saved = client_stack[--client_depth];
    if (saved->mask & GL_CLIENT_VERTEX_ARRAY_BIT) {
        GLuint name = ctx->vao->name;
        *ctx->vao = saved->arrays;
        ctx->vao->name = name;
        glm_vao_changed(ctx->vao);
        ctx->array_buffer = saved->array_buffer;
    }
    if (saved->mask & GL_CLIENT_PIXEL_STORE_BIT) {
        struct glm_state *s = &ctx->state;
        const struct glm_state *o = &saved->pixel;
        s->unpack_alignment = o->unpack_alignment; s->unpack_row_length = o->unpack_row_length;
        s->unpack_skip_rows = o->unpack_skip_rows; s->unpack_skip_pixels = o->unpack_skip_pixels;
        s->unpack_image_height = o->unpack_image_height; s->unpack_skip_images = o->unpack_skip_images;
        s->unpack_swap_bytes = o->unpack_swap_bytes; s->unpack_lsb_first = o->unpack_lsb_first;
        s->pack_swap_bytes = o->pack_swap_bytes;
        s->unpack_client_storage = o->unpack_client_storage;
        s->pack_alignment = o->pack_alignment; s->pack_row_length = o->pack_row_length;
        s->pack_skip_rows = o->pack_skip_rows; s->pack_skip_pixels = o->pack_skip_pixels;
        s->pack_image_height = o->pack_image_height; s->pack_skip_images = o->pack_skip_images;
        s->unpack_row_bytes = o->unpack_row_bytes; s->unpack_image_bytes = o->unpack_image_bytes;
        s->pack_row_bytes = o->pack_row_bytes; s->pack_image_bytes = o->pack_image_bytes;
    }
    free(saved);
}
