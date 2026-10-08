/* The raster position and the pixel rectangle operations drawn at it
 * (GL 2.1 2.13, 3.6-3.7, 4.3.3): glRasterPos, glWindowPos, glDrawPixels,
 * glBitmap, glCopyPixels, glPixelZoom and glPixelTransfer. Images become
 * window-space rectangles drawn by the backend with the current
 * per-fragment state. */
#include "glm_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

GLM_HIDDEN void glm_mat4_multiply(float *out, const float *a, const float *b);

/* ---- raster position ------------------------------------------------------- */

static void raster_pos(float x, float y, float z, float w)
{
    GLM_CONTEXT(ctx);
    struct glm_state *s = &ctx->state;
    const float *mv = s->modelview.m[s->modelview.depth], *p = s->projection.m[s->projection.depth];
    float mvp[16], clip[4];
    glm_mat4_multiply(mvp, p, mv);
    for (int r = 0; r < 4; ++r) clip[r] = mvp[r] * x + mvp[4 + r] * y + mvp[8 + r] * z + mvp[12 + r] * w;
    /* Outside the clip volume the raster position is invalid. */
    bool inside = clip[3] > 0;
    for (int i = 0; i < 3 && inside; ++i) inside = clip[i] >= -clip[3] && clip[i] <= clip[3];
    s->raster_valid = inside;
    if (!inside) return;
    float ndc[3] = {clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3]};
    s->raster_pos[0] = (float)s->viewport[0] + (ndc[0] + 1) * 0.5f * (float)s->viewport[2];
    s->raster_pos[1] = (float)s->viewport[1] + (ndc[1] + 1) * 0.5f * (float)s->viewport[3];
    s->raster_pos[2] = (float)(s->depth_range[0] + (ndc[2] + 1) * 0.5 * (s->depth_range[1] - s->depth_range[0]));
    s->raster_pos[3] = clip[3];
    memcpy(s->raster_color, s->current[GLM_ATTR_COLOR], sizeof s->raster_color);
    /* Texture coordinates through each unit's texture matrix. */
    for (int u = 0; u < 8; ++u) {
        const float *t = s->texture[u].m[s->texture[u].depth], *c = s->current[GLM_ATTR_TEX0 + u];
        for (int r = 0; r < 4; ++r) s->raster_texcoord[u][r] = t[r] * c[0] + t[4 + r] * c[1] + t[8 + r] * c[2] + t[12 + r] * c[3];
    }
}

#define RASTER_POS(suffix, T)                                                                              \
    GLM_EXPORT void glRasterPos2##suffix(T x, T y) { raster_pos((float)x, (float)y, 0, 1); }                \
    GLM_EXPORT void glRasterPos3##suffix(T x, T y, T z) { raster_pos((float)x, (float)y, (float)z, 1); }    \
    GLM_EXPORT void glRasterPos4##suffix(T x, T y, T z, T w) { raster_pos((float)x, (float)y, (float)z, (float)w); } \
    GLM_EXPORT void glRasterPos2##suffix##v(const T *v) { raster_pos((float)v[0], (float)v[1], 0, 1); }      \
    GLM_EXPORT void glRasterPos3##suffix##v(const T *v) { raster_pos((float)v[0], (float)v[1], (float)v[2], 1); } \
    GLM_EXPORT void glRasterPos4##suffix##v(const T *v)                                                     \
    {                                                                                                       \
        raster_pos((float)v[0], (float)v[1], (float)v[2], (float)v[3]);                                     \
    }
RASTER_POS(s, GLshort)
RASTER_POS(i, GLint)
RASTER_POS(f, GLfloat)
RASTER_POS(d, GLdouble)

/* Window coordinates directly (GL 2.1 2.13). */
static void window_pos(float x, float y, float z)
{
    GLM_CONTEXT(ctx);
    struct glm_state *s = &ctx->state;
    s->raster_pos[0] = x;
    s->raster_pos[1] = y;
    float depth = z < 0 ? 0 : z > 1 ? 1 : z;
    s->raster_pos[2] = (float)(s->depth_range[0] + depth * (s->depth_range[1] - s->depth_range[0]));
    s->raster_pos[3] = 1;
    s->raster_valid = true;
    memcpy(s->raster_color, s->current[GLM_ATTR_COLOR], sizeof s->raster_color);
    for (int u = 0; u < 8; ++u) memcpy(s->raster_texcoord[u], s->current[GLM_ATTR_TEX0 + u], sizeof s->raster_texcoord[u]);
}

#define WINDOW_POS(suffix, T)                                                                         \
    GLM_EXPORT void glWindowPos2##suffix(T x, T y) { window_pos((float)x, (float)y, 0); }              \
    GLM_EXPORT void glWindowPos3##suffix(T x, T y, T z) { window_pos((float)x, (float)y, (float)z); }  \
    GLM_EXPORT void glWindowPos2##suffix##v(const T *v) { window_pos((float)v[0], (float)v[1], 0); }   \
    GLM_EXPORT void glWindowPos3##suffix##v(const T *v) { window_pos((float)v[0], (float)v[1], (float)v[2]); }
WINDOW_POS(s, GLshort)
WINDOW_POS(i, GLint)
WINDOW_POS(f, GLfloat)
WINDOW_POS(d, GLdouble)
#define WINDOW_POS_ARB(suffix, T)                                                                         \
    GLM_EXPORT void glWindowPos2##suffix##ARB(T x, T y) { glWindowPos2##suffix(x, y); }                    \
    GLM_EXPORT void glWindowPos3##suffix##ARB(T x, T y, T z) { glWindowPos3##suffix(x, y, z); }            \
    GLM_EXPORT void glWindowPos2##suffix##vARB(const T *v) { glWindowPos2##suffix##v(v); }                 \
    GLM_EXPORT void glWindowPos3##suffix##vARB(const T *v) { glWindowPos3##suffix##v(v); }
WINDOW_POS_ARB(s, GLshort)
WINDOW_POS_ARB(i, GLint)
WINDOW_POS_ARB(f, GLfloat)
WINDOW_POS_ARB(d, GLdouble)

/* ---- pixel zoom and transfer ------------------------------------------------ */

GLM_EXPORT void glPixelZoom(GLfloat x, GLfloat y)
{
    GLM_CONTEXT(ctx);
    ctx->state.pixel_zoom[0] = x;
    ctx->state.pixel_zoom[1] = y;
}

GLM_EXPORT void glPixelTransferf(GLenum pname, GLfloat value)
{
    GLM_CONTEXT(ctx);
    struct glm_state *s = &ctx->state;
    switch (pname) {
    case GL_RED_SCALE: s->pixel_scale[0] = value; break;
    case GL_GREEN_SCALE: s->pixel_scale[1] = value; break;
    case GL_BLUE_SCALE: s->pixel_scale[2] = value; break;
    case GL_ALPHA_SCALE: s->pixel_scale[3] = value; break;
    case GL_RED_BIAS: s->pixel_bias[0] = value; break;
    case GL_GREEN_BIAS: s->pixel_bias[1] = value; break;
    case GL_BLUE_BIAS: s->pixel_bias[2] = value; break;
    case GL_ALPHA_BIAS: s->pixel_bias[3] = value; break;
    case GL_MAP_COLOR: s->map_color = value != 0; break;
    case GL_MAP_STENCIL: s->map_stencil = value != 0; break;
    case GL_INDEX_SHIFT: s->index_shift = (GLint)value; break;
    case GL_INDEX_OFFSET: s->index_offset = (GLint)value; break;
    case GL_DEPTH_SCALE: s->depth_scale = value; break;
    case GL_DEPTH_BIAS: s->depth_bias = value; break;
    case GL_POST_COLOR_MATRIX_RED_SCALE: break;
    default: glm_error(ctx, GL_INVALID_ENUM);
    }
}
GLM_EXPORT void glPixelTransferi(GLenum pname, GLint value) { glPixelTransferf(pname, (GLfloat)value); }

/* ---- pixel maps ------------------------------------------------------------ */

/* GL_PIXEL_MAP_I_TO_I .. GL_PIXEL_MAP_A_TO_A as 0..9; -1 for others. */
static int pixel_map_index(GLenum map)
{
    return map >= GL_PIXEL_MAP_I_TO_I && map <= GL_PIXEL_MAP_A_TO_A ? (int)(map - GL_PIXEL_MAP_I_TO_I) : -1;
}
/* Index maps hold indices; the others colour components. */
static bool index_map(int m) { return m <= 1; }

static void pixel_map(GLenum map, GLsizei size, const void *values, int kind /* 0 float, 1 uint, 2 ushort */)
{
    GLM_CONTEXT(ctx);
    int m = pixel_map_index(map);
    if (m < 0) return glm_error(ctx, GL_INVALID_ENUM);
    /* Index lookups (I_TO_*, S_TO_S) need power-of-two sizes. */
    bool from_index = m <= 5;
    if (size < 1 || size > 256 || (from_index && (size & (size - 1)))) return glm_error(ctx, GL_INVALID_VALUE);
    if (ctx->pixel_unpack_buffer) { /* `values` is an offset into the unpack buffer */
        struct glm_buffer *buffer = glm_buffer_get(ctx, ctx->pixel_unpack_buffer);
        const uint8_t *contents = buffer ? glm_backend_buffer_contents(ctx, buffer, false) : NULL;
        size_t bytes = (size_t)size * (kind == 2 ? 2 : 4);
        if (!contents || (uintptr_t)values + bytes > (size_t)buffer->size) return glm_error(ctx, GL_INVALID_OPERATION);
        values = contents + (uintptr_t)values;
    }
    struct glm_state *s = &ctx->state;
    for (GLsizei i = 0; i < size; ++i) {
        float v;
        if (kind == 0) v = ((const GLfloat *)values)[i];
        else if (kind == 1) {
            GLuint u = ((const GLuint *)values)[i];
            v = index_map(m) ? (float)u : (float)(u / 4294967295.0);
        } else {
            GLushort u = ((const GLushort *)values)[i];
            v = index_map(m) ? (float)u : (float)u / 65535.0f;
        }
        s->pixel_map[m][i] = v;
    }
    s->pixel_map_size[m] = size;
}
GLM_EXPORT void glPixelMapfv(GLenum map, GLsizei size, const GLfloat *values) { pixel_map(map, size, values, 0); }
GLM_EXPORT void glPixelMapuiv(GLenum map, GLsizei size, const GLuint *values) { pixel_map(map, size, values, 1); }
GLM_EXPORT void glPixelMapusv(GLenum map, GLsizei size, const GLushort *values) { pixel_map(map, size, values, 2); }

static void get_pixel_map(GLenum map, void *values, int kind)
{
    GLM_CONTEXT(ctx);
    int m = pixel_map_index(map);
    if (m < 0) return glm_error(ctx, GL_INVALID_ENUM);
    const struct glm_state *s = &ctx->state;
    if (ctx->pixel_pack_buffer) { /* `values` is an offset into the pack buffer */
        struct glm_buffer *buffer = glm_buffer_get(ctx, ctx->pixel_pack_buffer);
        uint8_t *contents = buffer ? glm_backend_buffer_contents(ctx, buffer, true) : NULL;
        size_t bytes = (size_t)s->pixel_map_size[m] * (kind == 2 ? 2 : 4);
        if (!contents || (uintptr_t)values + bytes > (size_t)buffer->size) return glm_error(ctx, GL_INVALID_OPERATION);
        values = contents + (uintptr_t)values;
    }
    for (GLint i = 0; i < s->pixel_map_size[m]; ++i) {
        float v = s->pixel_map[m][i];
        if (kind == 0) ((GLfloat *)values)[i] = v;
        else if (kind == 1)
            ((GLuint *)values)[i] = index_map(m) ? (GLuint)v : (GLuint)llround(fmin(fmax(v, 0), 1) * 4294967295.0);
        else ((GLushort *)values)[i] = index_map(m) ? (GLushort)v : (GLushort)lroundf(fminf(fmaxf(v, 0), 1) * 65535.0f);
    }
}
GLM_EXPORT void glGetPixelMapfv(GLenum map, GLfloat *values) { get_pixel_map(map, values, 0); }
GLM_EXPORT void glGetPixelMapuiv(GLenum map, GLuint *values) { get_pixel_map(map, values, 1); }
GLM_EXPORT void glGetPixelMapusv(GLenum map, GLushort *values) { get_pixel_map(map, values, 2); }

/* ---- drawing at the raster position ----------------------------------------- */

static bool image_to_rgba(struct glm_context *ctx, GLsizei width, GLsizei height, GLenum format, GLenum type,
                          const void *pixels, float **out)
{
    struct glm_format_info info;
    if (!glm_format_lookup(GL_RGBA32F, &info)) return false;
    const void *source = pixels;
    if (ctx->pixel_unpack_buffer) {
        struct glm_buffer *buffer = glm_buffer_get(ctx, ctx->pixel_unpack_buffer);
        const uint8_t *contents = buffer ? glm_backend_buffer_contents(ctx, buffer, false) : NULL;
        if (!contents) return false;
        source = contents + (uintptr_t)pixels;
    }
    size_t row_bytes, image_bytes;
    float *rgba = glm_convert_upload(ctx, &info, width, height, 1, format, type, source, &row_bytes, &image_bytes);
    if (!rgba) return false;
    /* (glm_convert_upload applies glPixelTransfer; fragments are clamped.) */
    for (size_t i = 0; i < (size_t)width * (size_t)height * 4; ++i) rgba[i] = rgba[i] < 0 ? 0 : rgba[i] > 1 ? 1 : rgba[i];
    *out = rgba;
    return true;
}

static void draw_at_raster(struct glm_context *ctx, const float *rgba, GLsizei width, GLsizei height)
{
    const struct glm_state *s = &ctx->state;
    float x0 = s->raster_pos[0], y0 = s->raster_pos[1];
    float x1 = x0 + (float)width * s->pixel_zoom[0], y1 = y0 + (float)height * s->pixel_zoom[1];
    glm_backend_draw_image(ctx, rgba, width, height, x0, y0, x1, y1, s->raster_pos[2], false, NULL);
}

GLM_EXPORT void glDrawPixels(GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels)
{
    GLM_CONTEXT(ctx);
    if (width < 0 || height < 0) return glm_error(ctx, GL_INVALID_VALUE);
    if (ctx->immediate) return glm_error(ctx, GL_INVALID_OPERATION);
    /* Format and type as for glReadPixels (stencil indices included). */
    GLenum error = glm_read_error((GLenum)-1, false, false, format, type, ctx->profile == GLM_PROFILE_CORE, true);
    if (error) return glm_error(ctx, error);
    if (!ctx->state.raster_valid || !width || !height || ctx->conditional_discard) return;
    if (!pixels && !ctx->pixel_unpack_buffer) return;
    if (format == GL_DEPTH_COMPONENT || format == GL_STENCIL_INDEX || format == GL_DEPTH_STENCIL) {
        const void *source = pixels;
        if (ctx->pixel_unpack_buffer) {
            struct glm_buffer *buffer = glm_buffer_get(ctx, ctx->pixel_unpack_buffer);
            const uint8_t *contents = buffer ? glm_backend_buffer_contents(ctx, buffer, false) : NULL;
            if (!contents) return glm_error(ctx, GL_INVALID_OPERATION);
            source = contents + (uintptr_t)pixels;
        }
        size_t n = (size_t)width * (size_t)height;
        float *depth = format != GL_STENCIL_INDEX ? malloc(n * sizeof *depth) : NULL;
        uint32_t *stencil = format != GL_DEPTH_COMPONENT ? malloc(n * sizeof *stencil) : NULL;
        if (glm_unpack_depth_stencil(ctx, width, height, format, type, source, depth, stencil)) {
            const struct glm_state *s = &ctx->state;
            float x0 = s->raster_pos[0], y0 = s->raster_pos[1];
            glm_backend_draw_depth_stencil(ctx, depth, stencil, width, height, x0, y0, x0 + (float)width * s->pixel_zoom[0],
                                           y0 + (float)height * s->pixel_zoom[1]);
        }
        free(depth);
        free(stencil);
        return;
    }
    float *rgba;
    if (!image_to_rgba(ctx, width, height, format, type, pixels, &rgba)) return glm_error(ctx, GL_INVALID_ENUM);
    draw_at_raster(ctx, rgba, width, height);
    free(rgba);
}

GLM_EXPORT void glCopyPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum type)
{
    GLM_CONTEXT(ctx);
    if (width < 0 || height < 0) return glm_error(ctx, GL_INVALID_VALUE);
    if (type != GL_COLOR && type != GL_DEPTH && type != GL_STENCIL && type != GL_DEPTH_STENCIL)
        return glm_error(ctx, GL_INVALID_ENUM);
    if (!ctx->state.raster_valid || !width || !height || ctx->conditional_discard) return;
    if (type != GL_COLOR) {
        /* Read with the default pack state, then draw as glDrawPixels does. */
        struct glm_state *s = &ctx->state;
        struct glm_state saved = *s;
        GLuint pack_buffer = ctx->pixel_pack_buffer;
        s->pack_alignment = 4;
        s->pack_swap_bytes = false;
        s->pack_row_length = s->pack_skip_rows = s->pack_skip_pixels = s->pack_row_bytes = 0;
        ctx->pixel_pack_buffer = 0;
        size_t n = (size_t)width * (size_t)height;
        float *depth = type != GL_STENCIL ? calloc(n, sizeof *depth) : NULL;
        uint32_t *stencil = type != GL_DEPTH ? calloc(n, sizeof *stencil) : NULL;
        if (depth) glm_backend_read_pixels(ctx, x, y, width, height, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
        if (stencil) glm_backend_read_pixels(ctx, x, y, width, height, GL_STENCIL_INDEX, GL_UNSIGNED_INT, stencil);
        s->pack_alignment = saved.pack_alignment;
        s->pack_row_length = saved.pack_row_length;
        s->pack_skip_rows = saved.pack_skip_rows;
        s->pack_skip_pixels = saved.pack_skip_pixels;
        s->pack_row_bytes = saved.pack_row_bytes;
        s->pack_swap_bytes = saved.pack_swap_bytes;
        ctx->pixel_pack_buffer = pack_buffer;
        float x0 = s->raster_pos[0], y0 = s->raster_pos[1];
        glm_backend_draw_depth_stencil(ctx, depth, stencil, width, height, x0, y0, x0 + (float)width * s->pixel_zoom[0],
                                       y0 + (float)height * s->pixel_zoom[1]);
        free(depth);
        free(stencil);
        return;
    }
    float *rgba = calloc((size_t)width * (size_t)height, 16);
    if (!rgba) return glm_error(ctx, GL_OUT_OF_MEMORY);
    /* Read as floats with the default pack state, then draw. */
    struct glm_state *s = &ctx->state;
    GLint row_bytes = s->pack_row_bytes;
    s->pack_row_bytes = 0;
    GLint alignment = s->pack_alignment, row_length = s->pack_row_length, skip_rows = s->pack_skip_rows,
          skip_pixels = s->pack_skip_pixels;
    GLuint pack_buffer = ctx->pixel_pack_buffer;
    bool pack_swap = s->pack_swap_bytes;
    s->pack_alignment = 4;
    s->pack_swap_bytes = false;
    s->pack_row_length = s->pack_skip_rows = s->pack_skip_pixels = 0;
    ctx->pixel_pack_buffer = 0;
    glm_backend_read_pixels(ctx, x, y, width, height, GL_RGBA, GL_FLOAT, rgba);
    s->pack_alignment = alignment;
    s->pack_row_length = row_length;
    s->pack_row_bytes = row_bytes;
    s->pack_skip_rows = skip_rows;
    s->pack_skip_pixels = skip_pixels;
    s->pack_swap_bytes = pack_swap;
    ctx->pixel_pack_buffer = pack_buffer;
    draw_at_raster(ctx, rgba, width, height);
    free(rgba);
}

/* One bit per pixel, most significant first unless GL_UNPACK_LSB_FIRST,
   rows padded to the unpack alignment (GL 2.1 3.6.4). */
GLM_EXPORT void glBitmap(GLsizei width, GLsizei height, GLfloat xorig, GLfloat yorig, GLfloat xmove, GLfloat ymove,
                         const GLubyte *bitmap)
{
    GLM_CONTEXT(ctx);
    if (width < 0 || height < 0) return glm_error(ctx, GL_INVALID_VALUE);
    if (ctx->immediate) return glm_error(ctx, GL_INVALID_OPERATION);
    struct glm_state *s = &ctx->state;
    if (!s->raster_valid) return;
    const uint8_t *bits = bitmap;
    if (ctx->pixel_unpack_buffer) {
        struct glm_buffer *buffer = glm_buffer_get(ctx, ctx->pixel_unpack_buffer);
        const uint8_t *contents = buffer ? glm_backend_buffer_contents(ctx, buffer, false) : NULL;
        bits = contents ? contents + (uintptr_t)bitmap : NULL;
    }
    if (bits && width && height && !ctx->conditional_discard) {
        size_t row_pixels = s->unpack_row_length > 0 ? (size_t)s->unpack_row_length : (size_t)width;
        size_t alignment = (size_t)(s->unpack_alignment > 0 ? s->unpack_alignment : 4);
        size_t row_bytes = s->unpack_row_bytes > 0 ? (size_t)s->unpack_row_bytes
                                                   : ((row_pixels + 7) / 8 + alignment - 1) / alignment * alignment;
        float *mask = calloc((size_t)width * (size_t)height, 16);
        for (GLsizei y = 0; y < height; ++y)
            for (GLsizei x = 0; x < width; ++x) {
                size_t bit = (size_t)s->unpack_skip_pixels + (size_t)x;
                uint8_t byte = bits[((size_t)s->unpack_skip_rows + (size_t)y) * row_bytes + bit / 8];
                bool on = s->unpack_lsb_first ? (byte >> (bit % 8)) & 1 : (byte >> (7 - bit % 8)) & 1;
                mask[((size_t)y * (size_t)width + (size_t)x) * 4] = on ? 1.0f : 0.0f;
            }
        /* Bitmaps ignore the pixel zoom. */
        float x0 = floorf(s->raster_pos[0] - xorig), y0 = floorf(s->raster_pos[1] - yorig);
        glm_backend_draw_image(ctx, mask, width, height, x0, y0, x0 + (float)width, y0 + (float)height, s->raster_pos[2], true,
                               s->raster_color);
        free(mask);
    }
    s->raster_pos[0] += xmove;
    s->raster_pos[1] += ymove;
}
