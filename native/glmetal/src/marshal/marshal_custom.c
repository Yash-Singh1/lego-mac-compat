/* The hand-written part of the marshal layer (tools/gen_marshal.py makes
 * the rest): a client-side shadow of the binding state draws and common
 * queries depend on, draw calls that stay in the command stream when every
 * enabled array lives in a buffer object, and stream control.
 *
 * Compiled without glm_impl_rename.h: the gl* names here are the exported
 * entry points, glm_impl_* the implementation. */
#include "../glm_internal.h"
#define GLM_APPLE_TYPES_ONLY
#include "glm_apple_gets.h"
#include "../marshal_support.h"

#include <stdlib.h>
#include <string.h>

void glm_impl_glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glm_impl_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances);
void glm_impl_glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);
void glm_impl_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const GLvoid *indices);
void glm_impl_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices, GLsizei instances);
void glm_impl_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices, GLint base);
void glm_impl_glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                            const GLvoid *indices, GLint base);
void glm_impl_glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices,
                                                GLsizei instances, GLint base);
void glm_impl_glGetIntegerv(GLenum pname, GLint *params);
GLsync glm_impl_glFenceSync(GLenum condition, GLbitfield flags);
GLenum glm_impl_glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout);
void glm_impl_glGetSynciv(GLsync sync, GLenum pname, GLsizei size, GLsizei *length, GLint *values);
void glm_impl_glGetTexParameteriv(GLenum target, GLenum pname, GLint *params);
void glm_impl_glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params);
void glm_impl_glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params);
void glm_impl_glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat *params);
void glm_impl_glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params);
void glm_impl_glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage);
void glm_impl_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data);
void glm_impl_glPixelStorei(GLenum pname, GLint param);
void glm_impl_glFlush(void);
void glm_impl_glFinish(void);

/* ---- parameter counts ------------------------------------------------------ */

size_t glm_param_count(unsigned pname)
{
    switch (pname) {
    /* colours, positions, planes, directions */
    case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_POSITION: case GL_EMISSION:
    case GL_AMBIENT_AND_DIFFUSE: case GL_LIGHT_MODEL_AMBIENT: case GL_FOG_COLOR: case GL_TEXTURE_ENV_COLOR:
    case GL_OBJECT_PLANE: case GL_EYE_PLANE: case GL_TEXTURE_BORDER_COLOR: case GL_TEXTURE_SWIZZLE_RGBA:
    case GL_POINT_DISTANCE_ATTENUATION:
        return 4;
    case GL_SPOT_DIRECTION: case GL_COLOR_INDEXES:
        return 3;
    default:
        return 1;
    }
}

int glm_cheap_pname(unsigned pname)
{
    /* PixelStore does not bump the draw-state serial. Typed getters must
       still wait for pending stores before reading the implementation state. */
    if ((pname >= GL_UNPACK_SWAP_BYTES && pname <= GL_UNPACK_ALIGNMENT) ||
        (pname >= GL_PACK_SWAP_BYTES && pname <= GL_PACK_ALIGNMENT) ||
        pname == GL_PACK_IMAGE_HEIGHT || pname == GL_PACK_SKIP_IMAGES ||
        pname == GL_UNPACK_IMAGE_HEIGHT || pname == GL_UNPACK_SKIP_IMAGES ||
        pname == GL_PACK_ROW_BYTES_APPLE || pname == GL_PACK_IMAGE_BYTES_APPLE ||
        pname == GL_UNPACK_ROW_BYTES_APPLE || pname == GL_UNPACK_IMAGE_BYTES_APPLE ||
        pname == GL_UNPACK_CLIENT_STORAGE_APPLE) return 1;
    if (pname >= 0x0B00 && pname <= 0x0B0F) return 1;   /* GL_CURRENT_* */
    if (pname >= 0x0BA0 && pname <= 0x0BA8) return 1;   /* matrix mode, stack depths, matrices */
    if (pname >= 0x84E3 && pname <= 0x84E6) return 1;   /* transpose matrices */
    if (pname == 0x80B1 || pname == 0x80B2) return 1;   /* color matrix */
    if (pname >= 0x8074 && pname <= 0x809A) return 1;   /* client arrays */
    if (pname >= 0x8450 && pname <= 0x845E) return 1;   /* fog coord / secondary color arrays and current */
    if (pname >= 0x8889 && pname <= 0x889F) return 1;   /* buffer bindings */
    if (pname == 0x85B5 || pname == 0x84E1) return 1;   /* vertex array binding, client active texture */
    return 0;
}

/* ---- shadow ---------------------------------------------------------------- */

struct shadow_vao {
    GLuint name, element_buffer;
    uint32_t enabled;       /* attribute arrays enabled */
    GLuint buffer[GLM_MAX_ATTRIBS];
};

/* Where each query's glEndQuery sits in the command stream, so result
   queries wait for just that much of it (query_object below). */
struct shadow_query_end {
    struct glm_thread *thread;
    uint64_t mark;             /* commands recorded through the glEndQuery */
};

struct glm_shadow {
    GLuint active_queries[8];          /* by shadow_query_class */
    struct shadow_query_end *query_ends; /* by query name */
    size_t query_end_capacity;
    GLuint array_buffer, pixel_pack, pixel_unpack, program, read_framebuffer, draw_framebuffer, renderbuffer;
    GLenum active_texture, client_active_texture;
    GLuint textures[GLM_MAX_TEXTURE_UNITS][GLM_TEX_SLOTS];
    GLint viewport[4], scissor[4];
    bool viewport_known, scissor_known;
    struct shadow_vao default_vao, *vao;
    struct shadow_vao **vaos;
    size_t vao_count;
    GLuint uniform_buffer, copy_read, copy_write, texture_buffer;
    GLint pixel_store[16];            /* GL_UNPACK_* 0x0CF0.. and GL_PACK_* 0x0D00.. by low nibble */
    GLint unpack_image_height, unpack_skip_images, unpack_row_bytes, unpack_image_bytes;
    bool pixel_store_known;
    GLuint samplers[GLM_MAX_TEXTURE_UNITS];
    GLint polygon_mode[2];
    bool polygon_mode_known;
    /* Integer texture parameters by texture name, seeded from the
       implementation when it is current. */
    struct shadow_texture { GLuint name; bool known; GLint p[14]; struct shadow_levels *levels; } *textures_params;
    size_t texture_count, texture_capacity;
    uint32_t *texture_index;          /* open addressing: name -> textures_params slot + 1 */
    size_t texture_index_capacity;
    /* Buffer sizes and usages this context defined. */
    /* Capabilities glIsEnabled asks about most, and the colour mask
       (draw buffer 0), seeded when the shadow is refreshed. */
    uint32_t caps, caps_known;
    GLboolean color_mask[4];
    bool color_mask_known;
    /* GL_DRAW_BUFFER (draw buffer 0) per draw framebuffer. */
    struct shadow_draw_buffer { GLuint framebuffer; GLenum mode; } draw_buffer[16];
    int draw_buffer_count;
    struct shadow_buffer { GLuint name; GLsizeiptr size; GLenum usage; } *buffers;
    size_t buffer_count, buffer_capacity;
};

/* Parameters shadowed per texture, by slot in shadow_texture.p. */
static const GLenum texture_pnames[14] = {
    GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R,
    GL_TEXTURE_BASE_LEVEL, GL_TEXTURE_MAX_LEVEL, GL_TEXTURE_COMPARE_MODE, GL_TEXTURE_COMPARE_FUNC,
    GL_DEPTH_TEXTURE_MODE, GL_GENERATE_MIPMAP, GL_TEXTURE_SWIZZLE_R, GL_TEXTURE_SWIZZLE_G, GL_TEXTURE_SWIZZLE_B,
};

static int texture_pname_slot(GLenum pname)
{
    for (int i = 0; i < 14; ++i)
        if (texture_pnames[i] == pname) return i;
    return -1;
}

/* Level sizes of a texture this context defined completely (from level 0
   or glTexStorage on): glGetTexLevelParameteriv answers without draining
   the stream. */
struct shadow_levels {
    GLint width[6][16], height[6][16], depth[6][16], format[6][16];
};

static void texture_index_rebuild(struct glm_shadow *s)
{
    size_t capacity = 64;
    while (capacity < s->texture_count * 2 + 2) capacity *= 2;
    free(s->texture_index);
    s->texture_index = calloc(capacity, sizeof *s->texture_index);
    s->texture_index_capacity = capacity;
    for (size_t i = 0; i < s->texture_count; ++i) {
        size_t at = (s->textures_params[i].name * 2654435761u) & (capacity - 1);
        while (s->texture_index[at]) at = (at + 1) & (capacity - 1);
        s->texture_index[at] = (uint32_t)i + 1;
    }
}

static struct shadow_texture *shadow_texture(struct glm_shadow *s, GLuint name, bool create)
{
    if (s->texture_index_capacity) {
        size_t mask = s->texture_index_capacity - 1;
        for (size_t at = (name * 2654435761u) & mask; s->texture_index[at]; at = (at + 1) & mask)
            if (s->textures_params[s->texture_index[at] - 1].name == name) return &s->textures_params[s->texture_index[at] - 1];
    }
    if (!create) return NULL;
    if (s->texture_count == s->texture_capacity) {
        s->texture_capacity = s->texture_capacity ? s->texture_capacity * 2 : 64;
        s->textures_params = realloc(s->textures_params, s->texture_capacity * sizeof *s->textures_params);
    }
    struct shadow_texture *t = &s->textures_params[s->texture_count++];
    memset(t, 0, sizeof *t);
    t->name = name;
    if (s->texture_count * 2 + 2 > s->texture_index_capacity) {
        texture_index_rebuild(s);
    } else {
        size_t mask = s->texture_index_capacity - 1, at = (name * 2654435761u) & mask;
        while (s->texture_index[at]) at = (at + 1) & mask;
        s->texture_index[at] = (uint32_t)s->texture_count;
    }
    return t;
}

static struct shadow_buffer *shadow_buffer(struct glm_shadow *s, GLuint name, bool create)
{
    for (size_t i = 0; i < s->buffer_count; ++i)
        if (s->buffers[i].name == name) return &s->buffers[i];
    if (!create || !name) return NULL;
    if (s->buffer_count == s->buffer_capacity) {
        s->buffer_capacity = s->buffer_capacity ? s->buffer_capacity * 2 : 64;
        s->buffers = realloc(s->buffers, s->buffer_capacity * sizeof *s->buffers);
    }
    s->buffers[s->buffer_count] = (struct shadow_buffer){name, 0, GL_STATIC_DRAW};
    return &s->buffers[s->buffer_count++];
}

static struct glm_shadow *shadow(struct glm_context *ctx)
{
    if (!ctx->shadow) {
        struct glm_shadow *s = calloc(1, sizeof *s);
        s->active_texture = s->client_active_texture = GL_TEXTURE0;
        s->vao = &s->default_vao;
        ctx->shadow = s;
    }
    return ctx->shadow;
}

static struct shadow_vao *find_vao(struct glm_shadow *s, GLuint name, bool create)
{
    if (!name) return &s->default_vao;
    for (size_t i = 0; i < s->vao_count; ++i)
        if (s->vaos[i]->name == name) return s->vaos[i];
    if (!create) return NULL;
    s->vaos = realloc(s->vaos, (s->vao_count + 1) * sizeof *s->vaos);
    struct shadow_vao *v = calloc(1, sizeof *v);
    v->name = name;
    s->vaos[s->vao_count++] = v;
    return v;
}

/* Re-reads everything from the implementation: after calls that change
   shadowed state in ways the hooks do not follow. The stream is drained. */
static void shadow_set_draw_buffer(struct glm_shadow *s, GLuint framebuffer, GLenum mode);

static const GLenum shadow_caps[] = {GL_DEPTH_TEST, GL_STENCIL_TEST, GL_BLEND, GL_CULL_FACE, GL_SCISSOR_TEST,
                                     GL_FRAMEBUFFER_SRGB, GL_POLYGON_OFFSET_FILL, GL_SAMPLE_ALPHA_TO_COVERAGE,
                                     GL_RASTERIZER_DISCARD, GL_DITHER, GL_ALPHA_TEST, GL_DEPTH_CLAMP};
enum { SHADOW_CAPS = sizeof shadow_caps / sizeof shadow_caps[0] };

static int shadow_cap_slot(GLenum cap)
{
    for (int i = 0; i < SHADOW_CAPS; ++i)
        if (shadow_caps[i] == cap) return i;
    return -1;
}

/* The slot a query may answer from: none for capabilities the profile
   lacks (the implementation raises the error). */
static int shadow_query_slot(const struct glm_context *ctx, GLenum cap)
{
    if (cap == GL_ALPHA_TEST && ctx->profile == GLM_PROFILE_CORE) return -1;
    return shadow_cap_slot(cap);
}

static void shadow_set_cap(struct glm_context *ctx, GLenum cap, bool value)
{
    int slot = shadow_cap_slot(cap);
    if (slot < 0) return;
    struct glm_shadow *s = shadow(ctx);
    if (value) s->caps |= 1u << slot;
    else s->caps &= ~(1u << slot);
    s->caps_known |= 1u << slot;
}

void glm_shadow_glEnable(struct glm_context *ctx, GLenum cap) { shadow_set_cap(ctx, cap, true); }
void glm_shadow_glDisable(struct glm_context *ctx, GLenum cap) { shadow_set_cap(ctx, cap, false); }
/* Index 0 is what glIsEnabled reports. */
void glm_shadow_glEnablei(struct glm_context *ctx, GLenum cap, GLuint index) { if (!index) shadow_set_cap(ctx, cap, true); }
void glm_shadow_glDisablei(struct glm_context *ctx, GLenum cap, GLuint index) { if (!index) shadow_set_cap(ctx, cap, false); }
void glm_shadow_glColorMask(struct glm_context *ctx, GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    struct glm_shadow *s = shadow(ctx);
    s->color_mask[0] = r;
    s->color_mask[1] = g;
    s->color_mask[2] = b;
    s->color_mask[3] = a;
    s->color_mask_known = true;
}
void glm_shadow_glColorMaski(struct glm_context *ctx, GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    if (!index) glm_shadow_glColorMask(ctx, r, g, b, a);
}

GLboolean glm_impl_glIsEnabled(GLenum cap);
void glm_impl_glGetBooleanv(GLenum pname, GLboolean *params);

GLM_EXPORT GLboolean glIsEnabled(GLenum cap)
{
    struct glm_context *ctx = glm_current();
    if (ctx && ctx->thread) {
        struct glm_shadow *s = shadow(ctx);
        int slot = shadow_query_slot(ctx, cap);
        if (slot >= 0 && ((s->caps_known >> slot) & 1)) return (s->caps >> slot) & 1;
        if (!glm_thread_state_settled(ctx)) glm_thread_sync_named(ctx, "glIsEnabled");
    }
    return glm_impl_glIsEnabled(cap);
}

GLM_EXPORT void glGetBooleanv(GLenum pname, GLboolean *params)
{
    struct glm_context *ctx = glm_current();
    if (ctx && ctx->thread) {
        struct glm_shadow *s = shadow(ctx);
        if (pname == GL_COLOR_WRITEMASK && s->color_mask_known) {
            memcpy(params, s->color_mask, 4);
            return;
        }
        int slot = shadow_query_slot(ctx, pname);
        if (slot >= 0 && ((s->caps_known >> slot) & 1)) {
            params[0] = (s->caps >> slot) & 1;
            return;
        }
        if (!(glm_thread_state_settled(ctx) && !glm_cheap_pname(pname))) glm_thread_sync_named(ctx, "glGetBooleanv");
    }
    glm_impl_glGetBooleanv(pname, params);
}

void glm_shadow_refresh(struct glm_context *ctx)
{
    struct glm_shadow *s = shadow(ctx);
    const struct glm_state *st = &ctx->state;
    s->array_buffer = ctx->array_buffer;
    s->pixel_pack = ctx->pixel_pack_buffer;
    s->pixel_unpack = ctx->pixel_unpack_buffer;
    s->program = ctx->current_program;
    s->read_framebuffer = ctx->read_framebuffer;
    s->draw_framebuffer = ctx->draw_framebuffer;
    {
        const bool values[SHADOW_CAPS] = {st->depth_test, st->stencil_test, st->blend[0].enabled, st->cull_face,
                                          st->scissor_test, st->framebuffer_srgb, st->polygon_offset_fill,
                                          st->sample_alpha_to_coverage, st->rasterizer_discard, st->dither,
                                          st->alpha_test, st->depth_clamp};
        s->caps = 0;
        for (int i = 0; i < SHADOW_CAPS; ++i)
            if (values[i]) s->caps |= 1u << i;
        s->caps_known = (1u << SHADOW_CAPS) - 1;
        for (int i = 0; i < 4; ++i) s->color_mask[i] = st->color_mask[0][i];
        s->color_mask_known = true;
    }
    s->draw_buffer_count = 0;
    {
        struct glm_framebuffer *fb = glm_framebuffer_get(ctx, ctx->draw_framebuffer);
        if (fb) shadow_set_draw_buffer(s, ctx->draw_framebuffer, fb->draw_buffers[0]);
    }
    s->renderbuffer = ctx->renderbuffer_binding;
    s->active_texture = st->active_texture;
    s->client_active_texture = ctx->vao->client_active_texture;
    for (int u = 0; u < GLM_MAX_TEXTURE_UNITS; ++u)
        for (int t = 0; t < GLM_TEX_SLOTS; ++t) s->textures[u][t] = st->units[u].bound[t];
    memcpy(s->viewport, st->viewport, sizeof s->viewport);
    memcpy(s->scissor, st->scissor, sizeof s->scissor);
    s->viewport_known = s->scissor_known = true;
    s->polygon_mode[0] = (GLint)st->polygon_mode[0];
    s->polygon_mode[1] = (GLint)st->polygon_mode[1];
    s->polygon_mode_known = true;
    s->uniform_buffer = ctx->uniform_buffer;
    s->copy_read = ctx->copy_read_buffer;
    s->copy_write = ctx->copy_write_buffer;
    s->texture_buffer = ctx->texture_buffer;
    for (int u = 0; u < GLM_MAX_TEXTURE_UNITS; ++u) s->samplers[u] = st->units[u].sampler;
    /* From the state itself: the context need not be current here (the
       stream can start from another thread), so no getters. */
    s->pixel_store_known = true;
    s->unpack_image_height = st->unpack_image_height;
    s->unpack_skip_images = st->unpack_skip_images;
    s->unpack_row_bytes = st->unpack_row_bytes;
    s->unpack_image_bytes = st->unpack_image_bytes;
    const struct { GLenum pname; GLint value; } stores[] = {
        {GL_UNPACK_SWAP_BYTES, st->unpack_swap_bytes}, {GL_UNPACK_LSB_FIRST, st->unpack_lsb_first},
        {GL_UNPACK_ROW_LENGTH, st->unpack_row_length}, {GL_UNPACK_SKIP_ROWS, st->unpack_skip_rows},
        {GL_UNPACK_SKIP_PIXELS, st->unpack_skip_pixels}, {GL_UNPACK_ALIGNMENT, st->unpack_alignment},
        {GL_PACK_SWAP_BYTES, st->pack_swap_bytes}, {GL_PACK_LSB_FIRST, 0},
        {GL_PACK_ROW_LENGTH, st->pack_row_length}, {GL_PACK_SKIP_ROWS, st->pack_skip_rows},
        {GL_PACK_SKIP_PIXELS, st->pack_skip_pixels}, {GL_PACK_ALIGNMENT, st->pack_alignment}};
    for (size_t i = 0; i < sizeof stores / sizeof stores[0]; ++i)
        s->pixel_store[(stores[i].pname & 0xf) | (stores[i].pname >= GL_PACK_SWAP_BYTES ? 8 : 0)] = stores[i].value;
    struct shadow_vao *v = find_vao(s, ctx->vao->name, true);
    v->element_buffer = ctx->vao->element_buffer;
    v->enabled = 0;
    for (int i = 0; i < GLM_MAX_ATTRIBS; ++i) {
        if (ctx->vao->arrays[i].enabled) v->enabled |= 1u << i;
        v->buffer[i] = ctx->vao->arrays[i].buffer;
    }
    s->vao = v;
}

/* Every enabled array (and the indices, if `indexed`) in a buffer object:
   the draw reads no client memory and can be recorded. */
static bool draw_recordable(struct glm_context *ctx, bool indexed)
{
    struct glm_shadow *s = shadow(ctx);
    struct shadow_vao *v = s->vao;
    if (indexed && !v->element_buffer) return false;
    for (uint32_t bits = v->enabled; bits; bits &= bits - 1)
        if (!v->buffer[__builtin_ctz(bits)]) return false;
    return true;
}

static int legacy_array_slot(struct glm_shadow *s, GLenum array)
{
    switch (array) {
    case GL_VERTEX_ARRAY: return GLM_ATTR_POSITION;
    case GL_NORMAL_ARRAY: return GLM_ATTR_NORMAL;
    case GL_COLOR_ARRAY: return GLM_ATTR_COLOR;
    case GL_SECONDARY_COLOR_ARRAY: return GLM_ATTR_SECONDARY_COLOR;
    case GL_FOG_COORD_ARRAY: return GLM_ATTR_FOG;
    case GL_TEXTURE_COORD_ARRAY: return GLM_ATTR_TEX0 + (int)(s->client_active_texture - GL_TEXTURE0);
    default: return -1;
    }
}

static void set_enabled(struct glm_shadow *s, int slot, bool on)
{
    if (slot < 0 || slot >= GLM_MAX_ATTRIBS) return;
    if (on) s->vao->enabled |= 1u << slot;
    else s->vao->enabled &= ~(1u << slot);
}

static void set_pointer(struct glm_shadow *s, int slot)
{
    if (slot >= 0 && slot < GLM_MAX_ATTRIBS) s->vao->buffer[slot] = s->array_buffer;
}

static int texture_slot(GLenum target)
{
    switch (target) {
    case GL_TEXTURE_1D: return GLM_TEX_1D;
    case GL_TEXTURE_2D: return GLM_TEX_2D;
    case GL_TEXTURE_3D: return GLM_TEX_3D;
    case GL_TEXTURE_CUBE_MAP: return GLM_TEX_CUBE;
    case GL_TEXTURE_RECTANGLE_ARB: return GLM_TEX_RECT;
    case GL_TEXTURE_2D_ARRAY: return GLM_TEX_2D_ARRAY;
    default: return -1;
    }
}

void glm_shadow_glBindBuffer(struct glm_context *ctx, GLenum target, GLuint name)
{
    struct glm_shadow *s = shadow(ctx);
    if (ctx->profile == GLM_PROFILE_CORE && name && !shadow_buffer(s, name, false)) {
        /* Validate a newly seen name before changing the caller's bindings.
           Repeated bindings use the cache; deletion removes its entry. */
        glm_thread_sync(ctx);
        pthread_mutex_lock(&ctx->share->lock);
        struct glm_buffer *b = glm_buffer_get(ctx, name);
        if (b) {
            struct shadow_buffer *known = shadow_buffer(s, name, true);
            known->size = b->size;
            known->usage = b->usage;
        }
        pthread_mutex_unlock(&ctx->share->lock);
        if (!b) return;
    }
    switch (target) {
    case GL_ARRAY_BUFFER: s->array_buffer = name; break;
    case GL_ELEMENT_ARRAY_BUFFER: s->vao->element_buffer = name; break;
    case GL_PIXEL_PACK_BUFFER: s->pixel_pack = name; break;
    case GL_PIXEL_UNPACK_BUFFER: s->pixel_unpack = name; break;
    case GL_UNIFORM_BUFFER: s->uniform_buffer = name; break;
    case GL_COPY_READ_BUFFER: s->copy_read = name; break;
    case GL_COPY_WRITE_BUFFER: s->copy_write = name; break;
    case GL_TEXTURE_BUFFER: s->texture_buffer = name; break;
    }
}

/* The buffer the application has bound to `target`, or ~0 if unknown. */
static GLuint shadow_bound_buffer(struct glm_shadow *s, GLenum target)
{
    switch (target) {
    case GL_ARRAY_BUFFER: return s->array_buffer;
    case GL_ELEMENT_ARRAY_BUFFER: return s->vao->element_buffer;
    case GL_PIXEL_PACK_BUFFER: return s->pixel_pack;
    case GL_PIXEL_UNPACK_BUFFER: return s->pixel_unpack;
    case GL_UNIFORM_BUFFER: return s->uniform_buffer;
    case GL_COPY_READ_BUFFER: return s->copy_read;
    case GL_COPY_WRITE_BUFFER: return s->copy_write;
    case GL_TEXTURE_BUFFER: return s->texture_buffer;
    default: return ~0u;
    }
}
void glm_shadow_glBindBufferARB(struct glm_context *ctx, GLenum target, GLuint name) { glm_shadow_glBindBuffer(ctx, target, name); }

void glm_shadow_glDeleteBuffers(struct glm_context *ctx, GLsizei n, const GLuint *names)
{
    struct glm_shadow *s = shadow(ctx);
    for (GLsizei i = 0; i < n && names; ++i) {
        GLuint name = names[i];
        if (!name) continue;
        if (s->array_buffer == name) s->array_buffer = 0;
        if (s->pixel_pack == name) s->pixel_pack = 0;
        if (s->pixel_unpack == name) s->pixel_unpack = 0;
        if (s->uniform_buffer == name) s->uniform_buffer = 0;
        if (s->copy_read == name) s->copy_read = 0;
        if (s->copy_write == name) s->copy_write = 0;
        if (s->texture_buffer == name) s->texture_buffer = 0;
        if (s->vao->element_buffer == name) s->vao->element_buffer = 0;
        for (int a = 0; a < GLM_MAX_ATTRIBS; ++a)
            if (s->vao->buffer[a] == name) s->vao->buffer[a] = 0;
        struct shadow_buffer *b = shadow_buffer(s, name, false);
        if (b) *b = s->buffers[--s->buffer_count];
    }
}
void glm_shadow_glDeleteBuffersARB(struct glm_context *ctx, GLsizei n, const GLuint *names) { glm_shadow_glDeleteBuffers(ctx, n, names); }

void glm_shadow_glBindVertexArray(struct glm_context *ctx, GLuint name)
{
    struct glm_shadow *s = shadow(ctx);
    s->vao = find_vao(s, name, true);
}
void glm_shadow_glBindVertexArrayAPPLE(struct glm_context *ctx, GLuint name) { glm_shadow_glBindVertexArray(ctx, name); }

void glm_shadow_glDeleteVertexArrays(struct glm_context *ctx, GLsizei n, const GLuint *names)
{
    struct glm_shadow *s = shadow(ctx);
    for (GLsizei i = 0; i < n && names; ++i) {
        for (size_t k = 0; k < s->vao_count; ++k) {
            if (s->vaos[k]->name != names[i]) continue;
            if (s->vao == s->vaos[k]) s->vao = &s->default_vao;
            free(s->vaos[k]);
            s->vaos[k] = s->vaos[--s->vao_count];
            break;
        }
    }
}
void glm_shadow_glDeleteVertexArraysAPPLE(struct glm_context *ctx, GLsizei n, const GLuint *names) { glm_shadow_glDeleteVertexArrays(ctx, n, names); }

void glm_shadow_glEnableVertexAttribArray(struct glm_context *ctx, GLuint index) { set_enabled(shadow(ctx), (int)index, true); }
void glm_shadow_glEnableVertexAttribArrayARB(struct glm_context *ctx, GLuint index) { set_enabled(shadow(ctx), (int)index, true); }
void glm_shadow_glDisableVertexAttribArray(struct glm_context *ctx, GLuint index) { set_enabled(shadow(ctx), (int)index, false); }
void glm_shadow_glDisableVertexAttribArrayARB(struct glm_context *ctx, GLuint index) { set_enabled(shadow(ctx), (int)index, false); }
void glm_shadow_glEnableClientState(struct glm_context *ctx, GLenum array)
{
    struct glm_shadow *s = shadow(ctx);
    set_enabled(s, legacy_array_slot(s, array), true);
}
void glm_shadow_glDisableClientState(struct glm_context *ctx, GLenum array)
{
    struct glm_shadow *s = shadow(ctx);
    set_enabled(s, legacy_array_slot(s, array), false);
}
void glm_shadow_glClientActiveTexture(struct glm_context *ctx, GLenum unit) { shadow(ctx)->client_active_texture = unit; }
void glm_shadow_glClientActiveTextureARB(struct glm_context *ctx, GLenum unit) { shadow(ctx)->client_active_texture = unit; }

void glm_shadow_glVertexAttribPointer(struct glm_context *ctx, GLuint index, GLint size, GLenum type, GLboolean normalized,
                                      GLsizei stride, const GLvoid *pointer)
{
    set_pointer(shadow(ctx), (int)index);
}
void glm_shadow_glVertexAttribPointerARB(struct glm_context *ctx, GLuint index, GLint size, GLenum type,
                                         GLboolean normalized, GLsizei stride, const GLvoid *pointer)
{
    set_pointer(shadow(ctx), (int)index);
}
void glm_shadow_glVertexAttribIPointer(struct glm_context *ctx, GLuint index, GLint size, GLenum type, GLsizei stride,
                                       const GLvoid *pointer)
{
    set_pointer(shadow(ctx), (int)index);
}
void glm_shadow_glVertexAttribIPointerEXT(struct glm_context *ctx, GLuint index, GLint size, GLenum type, GLsizei stride,
                                          const GLvoid *pointer)
{
    set_pointer(shadow(ctx), (int)index);
}
void glm_shadow_glVertexPointer(struct glm_context *ctx, GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
    set_pointer(shadow(ctx), GLM_ATTR_POSITION);
}
void glm_shadow_glNormalPointer(struct glm_context *ctx, GLenum type, GLsizei stride, const GLvoid *pointer)
{
    set_pointer(shadow(ctx), GLM_ATTR_NORMAL);
}
void glm_shadow_glColorPointer(struct glm_context *ctx, GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
    set_pointer(shadow(ctx), GLM_ATTR_COLOR);
}
void glm_shadow_glSecondaryColorPointer(struct glm_context *ctx, GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
    set_pointer(shadow(ctx), GLM_ATTR_SECONDARY_COLOR);
}
void glm_shadow_glSecondaryColorPointerEXT(struct glm_context *ctx, GLint size, GLenum type, GLsizei stride,
                                           const GLvoid *pointer)
{
    set_pointer(shadow(ctx), GLM_ATTR_SECONDARY_COLOR);
}
void glm_shadow_glFogCoordPointer(struct glm_context *ctx, GLenum type, GLsizei stride, const GLvoid *pointer)
{
    set_pointer(shadow(ctx), GLM_ATTR_FOG);
}
void glm_shadow_glFogCoordPointerEXT(struct glm_context *ctx, GLenum type, GLsizei stride, const GLvoid *pointer)
{
    set_pointer(shadow(ctx), GLM_ATTR_FOG);
}
void glm_shadow_glTexCoordPointer(struct glm_context *ctx, GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
    struct glm_shadow *s = shadow(ctx);
    set_pointer(s, GLM_ATTR_TEX0 + (int)(s->client_active_texture - GL_TEXTURE0));
}
void glm_shadow_glVertexAttribDivisor(struct glm_context *ctx, GLuint index, GLuint divisor) {}
void glm_shadow_glVertexAttribDivisorARB(struct glm_context *ctx, GLuint index, GLuint divisor) {}

void glm_shadow_glUseProgram(struct glm_context *ctx, GLuint program) { shadow(ctx)->program = program; }
void glm_shadow_glUseProgramObjectARB(struct glm_context *ctx, GLhandleARB program)
{
    shadow(ctx)->program = (GLuint)(uintptr_t)program;
}
void glm_shadow_glActiveTexture(struct glm_context *ctx, GLenum unit) { shadow(ctx)->active_texture = unit; }
void glm_shadow_glActiveTextureARB(struct glm_context *ctx, GLenum unit) { shadow(ctx)->active_texture = unit; }
void glm_shadow_glBindTexture(struct glm_context *ctx, GLenum target, GLuint name)
{
    struct glm_shadow *s = shadow(ctx);
    int slot = texture_slot(target);
    unsigned unit = s->active_texture - GL_TEXTURE0;
    if (slot >= 0 && unit < GLM_MAX_TEXTURE_UNITS) s->textures[unit][slot] = name;
    /* Seed the parameter shadow while the implementation's object is current. */
    struct shadow_texture *t = name && slot >= 0 ? shadow_texture(s, name, true) : NULL;
    if (t && !t->known && ctx->thread && glm_thread_pending_zero(ctx, 1)) {
        glm_texture_query_override(ctx, target, name);
        for (int i = 0; i < 14; ++i)
            if (glm_texture_pname_in_profile(ctx, texture_pnames[i]))
                glm_impl_glGetTexParameteriv(target, texture_pnames[i], &t->p[i]);
        glm_texture_query_override_end();
        t->known = true;
    }
}

static void shadow_tex_parameter(struct glm_context *ctx, GLenum target, GLenum pname, GLint value)
{
    struct glm_shadow *s = shadow(ctx);
    GLuint name;
    int slot = texture_pname_slot(pname);
    int binding = texture_slot(target);
    unsigned unit = s->active_texture - GL_TEXTURE0;
    if (binding < 0 || unit >= GLM_MAX_TEXTURE_UNITS) return;
    name = s->textures[unit][binding];
    struct shadow_texture *t = name ? shadow_texture(s, name, false) : NULL;
    if (!t) return;
    if (slot >= 0) t->p[slot] = value;
}
void glm_shadow_glTexParameteri(struct glm_context *ctx, GLenum target, GLenum pname, GLint param) { shadow_tex_parameter(ctx, target, pname, param); }
void glm_shadow_glTexParameterf(struct glm_context *ctx, GLenum target, GLenum pname, GLfloat param) { shadow_tex_parameter(ctx, target, pname, (GLint)param); }
void glm_shadow_glTexParameteriv(struct glm_context *ctx, GLenum target, GLenum pname, const GLint *params)
{
    if (pname == GL_TEXTURE_SWIZZLE_RGBA) {
        for (int i = 0; i < 3; ++i) shadow_tex_parameter(ctx, target, GL_TEXTURE_SWIZZLE_R + (GLenum)i, params[i]);
        return;
    }
    if (params) shadow_tex_parameter(ctx, target, pname, params[0]);
}
void glm_shadow_glTexParameterfv(struct glm_context *ctx, GLenum target, GLenum pname, const GLfloat *params)
{
    if (params && pname != GL_TEXTURE_BORDER_COLOR) shadow_tex_parameter(ctx, target, pname, (GLint)params[0]);
}
void glm_shadow_glPolygonMode(struct glm_context *ctx, GLenum face, GLenum mode)
{
    if (mode != GL_POINT && mode != GL_LINE && mode != GL_FILL) return;
    struct glm_shadow *s = shadow(ctx);
    if (face == GL_FRONT || face == GL_FRONT_AND_BACK) s->polygon_mode[0] = (GLint)mode;
    if (face == GL_BACK || face == GL_FRONT_AND_BACK) s->polygon_mode[1] = (GLint)mode;
}
void glm_shadow_glBindTextureEXT(struct glm_context *ctx, GLenum target, GLuint name) { glm_shadow_glBindTexture(ctx, target, name); }
void glm_shadow_glBindFramebuffer(struct glm_context *ctx, GLenum target, GLuint name)
{
    struct glm_shadow *s = shadow(ctx);
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) s->draw_framebuffer = name;
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) s->read_framebuffer = name;
}
void glm_shadow_glBindFramebufferEXT(struct glm_context *ctx, GLenum target, GLuint name) { glm_shadow_glBindFramebuffer(ctx, target, name); }
void glm_shadow_glBindRenderbuffer(struct glm_context *ctx, GLenum target, GLuint name) { shadow(ctx)->renderbuffer = name; }
void glm_shadow_glBindRenderbufferEXT(struct glm_context *ctx, GLenum target, GLuint name) { shadow(ctx)->renderbuffer = name; }
void glm_shadow_glViewport(struct glm_context *ctx, GLint x, GLint y, GLsizei w, GLsizei h)
{
    struct glm_shadow *s = shadow(ctx);
    if (w < 0 || h < 0) return;
    s->viewport[0] = x;
    s->viewport[1] = y;
    s->viewport[2] = w;
    s->viewport[3] = h;
    s->viewport_known = true;
}
void glm_shadow_glScissor(struct glm_context *ctx, GLint x, GLint y, GLsizei w, GLsizei h)
{
    struct glm_shadow *s = shadow(ctx);
    if (w < 0 || h < 0) return;
    s->scissor[0] = x;
    s->scissor[1] = y;
    s->scissor[2] = w;
    s->scissor[3] = h;
    s->scissor_known = true;
}
void glm_shadow_glReadBuffer(struct glm_context *ctx, GLenum mode) {}
static void shadow_set_draw_buffer(struct glm_shadow *s, GLuint framebuffer, GLenum mode)
{
    for (int i = 0; i < s->draw_buffer_count; ++i)
        if (s->draw_buffer[i].framebuffer == framebuffer) {
            s->draw_buffer[i].mode = mode;
            return;
        }
    if (s->draw_buffer_count == 16) s->draw_buffer_count = 0; /* forget the oldest few */
    s->draw_buffer[s->draw_buffer_count++] = (struct shadow_draw_buffer){framebuffer, mode};
}
void glm_shadow_glDrawBuffer(struct glm_context *ctx, GLenum mode)
{
    struct glm_shadow *s = shadow(ctx);
    if (!glm_color_buffer_error(ctx, s->draw_framebuffer != 0, mode, GLM_CALL_DRAW_BUFFER))
        shadow_set_draw_buffer(s, s->draw_framebuffer, mode);
}
void glm_shadow_glDrawBuffers(struct glm_context *ctx, GLsizei n, const GLenum *modes)
{
    struct glm_shadow *s = shadow(ctx);
    if (n > 0 && modes && !glm_draw_buffers_error(ctx, s->draw_framebuffer != 0, n, modes))
        shadow_set_draw_buffer(s, s->draw_framebuffer, modes[0]);
}
void glm_shadow_glDrawBuffersARB(struct glm_context *ctx, GLsizei n, const GLenum *modes) { glm_shadow_glDrawBuffers(ctx, n, modes); }

/* ---- draws ------------------------------------------------------------------ */

/* Draws compiled into a display list keep their vertices (GL 2.1 5.4). */
static bool draw_arrays_list(struct glm_context *ctx, GLenum mode, GLint first, GLsizei count, GLsizei instances)
{
    if (ctx->thread) glm_thread_sync_named(ctx, "display list draw");
    glm_list_capture_draw(ctx, mode, first, count, 0, NULL, instances, 0);
    return !glm_list_executes(ctx);
}
static bool draw_elements_list(struct glm_context *ctx, GLenum mode, GLsizei count, GLenum type, const GLvoid *indices,
                               GLsizei instances, GLint base)
{
    if (ctx->thread) glm_thread_sync_named(ctx, "display list draw");
    glm_list_capture_draw(ctx, mode, 0, count, type, indices, instances, base);
    return !glm_list_executes(ctx);
}

#define RECORD(name, ...)                                                                                  \
    struct glm_context *ctx = glm_current();                                                               \
    if (ctx && ctx->listing && name##_list(ctx, __VA_ARGS__)) return;                                      \
    if (ctx && ctx->thread) {                                                                              \
        if (draw_recordable(ctx, INDEXED)) {                                                               \
            struct name##_cmd *c = glm_thread_alloc(ctx, sizeof *c, name##_exec);                          \
            *c = (struct name##_cmd){__VA_ARGS__};                                                         \
            return;                                                                                        \
        }                                                                                                  \
        glm_thread_sync_named(ctx, #name);                                                                 \
    }

#define INDEXED false
struct draw_arrays_cmd { GLenum mode; GLint first; GLsizei count, instances; };
static void draw_arrays_exec(const void *p)
{
    const struct draw_arrays_cmd *c = p;
    if (c->instances == 1) glm_impl_glDrawArrays(c->mode, c->first, c->count);
    else glm_impl_glDrawArraysInstanced(c->mode, c->first, c->count, c->instances);
}
GLM_EXPORT void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    RECORD(draw_arrays, mode, first, count, 1)
    glm_impl_glDrawArrays(mode, first, count);
}
GLM_EXPORT void glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances)
{
    RECORD(draw_arrays, mode, first, count, instances)
    glm_impl_glDrawArraysInstanced(mode, first, count, instances);
}
GLM_EXPORT void glDrawArraysInstancedARB(GLenum mode, GLint first, GLsizei count, GLsizei instances)
{
    glDrawArraysInstanced(mode, first, count, instances);
}
#undef INDEXED

#define INDEXED true
struct draw_elements_cmd { GLenum mode; GLsizei count; GLenum type; const GLvoid *indices; GLsizei instances; GLint base; };
static void draw_elements_exec(const void *p)
{
    const struct draw_elements_cmd *c = p;
    glm_impl_glDrawElementsInstancedBaseVertex(c->mode, c->count, c->type, c->indices, c->instances, c->base);
}
GLM_EXPORT void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
    RECORD(draw_elements, mode, count, type, indices, 1, 0)
    glm_impl_glDrawElements(mode, count, type, indices);
}
GLM_EXPORT void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const GLvoid *indices)
{
    RECORD(draw_elements, mode, count, type, indices, 1, 0)
    glm_impl_glDrawRangeElements(mode, start, end, count, type, indices);
}
GLM_EXPORT void glDrawRangeElementsEXT(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                       const GLvoid *indices)
{
    glDrawRangeElements(mode, start, end, count, type, indices);
}
GLM_EXPORT void glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices, GLsizei instances)
{
    RECORD(draw_elements, mode, count, type, indices, instances, 0)
    glm_impl_glDrawElementsInstanced(mode, count, type, indices, instances);
}
GLM_EXPORT void glDrawElementsInstancedARB(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices, GLsizei instances)
{
    glDrawElementsInstanced(mode, count, type, indices, instances);
}
GLM_EXPORT void glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices, GLint base)
{
    RECORD(draw_elements, mode, count, type, indices, 1, base)
    glm_impl_glDrawElementsBaseVertex(mode, count, type, indices, base);
}
GLM_EXPORT void glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                              const GLvoid *indices, GLint base)
{
    RECORD(draw_elements, mode, count, type, indices, 1, base)
    glm_impl_glDrawRangeElementsBaseVertex(mode, start, end, count, type, indices, base);
}
GLM_EXPORT void glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices,
                                                  GLsizei instances, GLint base)
{
    RECORD(draw_elements, mode, count, type, indices, instances, base)
    glm_impl_glDrawElementsInstancedBaseVertex(mode, count, type, indices, instances, base);
}
#undef INDEXED

/* ---- queries answered from the shadow -------------------------------------- */

static bool shadow_query(struct glm_context *ctx, GLenum pname, GLint *out)
{
    struct glm_shadow *s = shadow(ctx);
    unsigned unit = s->active_texture - GL_TEXTURE0;
    switch (pname) {
    case GL_ARRAY_BUFFER_BINDING: *out = (GLint)s->array_buffer; return true;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: *out = (GLint)s->vao->element_buffer; return true;
    case GL_PIXEL_PACK_BUFFER_BINDING: *out = (GLint)s->pixel_pack; return true;
    case GL_PIXEL_UNPACK_BUFFER_BINDING: *out = (GLint)s->pixel_unpack; return true;
    case GL_VERTEX_ARRAY_BINDING: *out = (GLint)s->vao->name; return true;
    case GL_CURRENT_PROGRAM: *out = (GLint)s->program; return true;
    case GL_ACTIVE_TEXTURE: *out = (GLint)s->active_texture; return true;
    case GL_CLIENT_ACTIVE_TEXTURE: if (ctx->profile == GLM_PROFILE_CORE) return false; *out = (GLint)s->client_active_texture; return true;
    case GL_DRAW_FRAMEBUFFER_BINDING: *out = (GLint)s->draw_framebuffer; return true;
    case GL_READ_FRAMEBUFFER_BINDING: *out = (GLint)s->read_framebuffer; return true;
    case GL_RENDERBUFFER_BINDING: *out = (GLint)s->renderbuffer; return true;
    case GL_UNIFORM_BUFFER_BINDING: /* legacy answers GL_UNIFORM_BUFFER_BINDING_EXT only */
        if (ctx->profile != GLM_PROFILE_CORE) return false;
        *out = (GLint)s->uniform_buffer;
        return true;
    case GL_SAMPLER_BINDING:
        if (ctx->profile != GLM_PROFILE_CORE || unit >= GLM_MAX_TEXTURE_UNITS) return false;
        *out = (GLint)s->samplers[unit];
        return true;
    case GL_UNPACK_SWAP_BYTES: case GL_UNPACK_LSB_FIRST: case GL_UNPACK_ROW_LENGTH: case GL_UNPACK_SKIP_ROWS:
    case GL_UNPACK_SKIP_PIXELS: case GL_UNPACK_ALIGNMENT: case GL_PACK_SWAP_BYTES: case GL_PACK_LSB_FIRST:
    case GL_PACK_ROW_LENGTH: case GL_PACK_SKIP_ROWS: case GL_PACK_SKIP_PIXELS: case GL_PACK_ALIGNMENT:
        if (!s->pixel_store_known) return false;
        *out = s->pixel_store[(pname & 0xf) | (pname >= GL_PACK_SWAP_BYTES ? 8 : 0)];
        return true;
    case GL_TEXTURE_BINDING_1D: case GL_TEXTURE_BINDING_2D: case GL_TEXTURE_BINDING_3D: case GL_TEXTURE_BINDING_CUBE_MAP:
    case GL_TEXTURE_BINDING_RECTANGLE_ARB: case GL_TEXTURE_BINDING_2D_ARRAY: {
        if (unit >= GLM_MAX_TEXTURE_UNITS) return false;
        int slot = pname == GL_TEXTURE_BINDING_1D ? GLM_TEX_1D : pname == GL_TEXTURE_BINDING_2D ? GLM_TEX_2D
                 : pname == GL_TEXTURE_BINDING_3D ? GLM_TEX_3D : pname == GL_TEXTURE_BINDING_CUBE_MAP ? GLM_TEX_CUBE
                 : pname == GL_TEXTURE_BINDING_RECTANGLE_ARB ? GLM_TEX_RECT : GLM_TEX_2D_ARRAY;
        *out = (GLint)s->textures[unit][slot];
        return true;
    }
    default: return false;
    }
}

GLM_EXPORT void glGetIntegerv(GLenum pname, GLint *params)
{
    struct glm_context *ctx = glm_current();
    if (ctx && ctx->thread) {
        struct glm_shadow *s = shadow(ctx);
        if (pname == GL_VIEWPORT && s->viewport_known) {
            memcpy(params, s->viewport, sizeof s->viewport);
            return;
        }
        /* The window-system framebuffer before a surface: the implementation's
           answer (GL_NONE, as Apple's). */
        if (pname == GL_DRAW_BUFFER && (s->draw_framebuffer || ctx->presents)) {
            for (int i = 0; i < s->draw_buffer_count; ++i)
                if (s->draw_buffer[i].framebuffer == s->draw_framebuffer) {
                    params[0] = (GLint)s->draw_buffer[i].mode;
                    return;
                }
        }
        /* The implementation answers it in both profiles, as here. */
        if (pname == GL_POLYGON_MODE && s->polygon_mode_known) {
            params[0] = s->polygon_mode[0];
            params[1] = s->polygon_mode[1];
            return;
        }
        if (pname == GL_SCISSOR_BOX && s->scissor_known) {
            memcpy(params, s->scissor, sizeof s->scissor);
            return;
        }
        if (shadow_query(ctx, pname, params)) return;
        if (glm_thread_state_settled(ctx) && !glm_cheap_pname(pname)) {
            glm_impl_glGetIntegerv(pname, params);
            return;
        }
        static const char *names[] = {"glGetIntegerv"};
        glm_thread_sync_named(ctx, names[0]);
        {
            static int stats = -1;
            if (stats < 0) stats = getenv("GLMETAL_STREAM_STATS") != NULL;
            if (stats) glm_log("glGetIntegerv sync for 0x%04x", pname);
        }
    }
    glm_impl_glGetIntegerv(pname, params);
}

/* ---- stream control ---------------------------------------------------------- */

static void flush_exec(const void *p)
{
    (void)p;
    glm_impl_glFlush();
}

GLM_EXPORT void glFlush(void)
{
    struct glm_context *ctx = glm_current();
    if (ctx && ctx->thread) {
        glm_thread_alloc(ctx, 0, flush_exec);
        glm_thread_submit(ctx);
        return;
    }
    glm_impl_glFlush();
}

/* APPLE_element_array: element draws through the public glDrawElements,
   which copies client arrays for the stream. */
GLM_EXPORT void glElementPointerAPPLE(GLenum type, const GLvoid *pointer)
{
    struct glm_context *ctx = glm_current();
    if (glm_profile_rejected(ctx, 1)) return;
    if (!ctx) return;
    ctx->apple_element_type = type;
    ctx->apple_element_pointer = pointer;
}

static const char *apple_elements(struct glm_context *ctx, GLint first)
{
    size_t size = ctx->apple_element_type == GL_UNSIGNED_BYTE ? 1 : ctx->apple_element_type == GL_UNSIGNED_SHORT ? 2 : 4;
    return (const char *)ctx->apple_element_pointer + (size_t)first * size;
}

GLM_EXPORT void glDrawElementArrayAPPLE(GLenum mode, GLint first, GLsizei count)
{
    struct glm_context *ctx = glm_current();
    if (glm_profile_rejected(ctx, 1)) return;
    if (ctx) glDrawElements(mode, count, ctx->apple_element_type, apple_elements(ctx, first));
}
GLM_EXPORT void glDrawRangeElementArrayAPPLE(GLenum mode, GLuint start, GLuint end, GLint first, GLsizei count)
{
    struct glm_context *ctx = glm_current();
    if (glm_profile_rejected(ctx, 1)) return;
    if (ctx) glDrawRangeElements(mode, start, end, count, ctx->apple_element_type, apple_elements(ctx, first));
}
GLM_EXPORT void glMultiDrawElementArrayAPPLE(GLenum mode, const GLint *first, const GLsizei *count, GLsizei n)
{
    if (glm_profile_rejected(glm_current(), 1)) return;
    for (GLsizei i = 0; i < n; ++i) glDrawElementArrayAPPLE(mode, first[i], count[i]);
}
GLM_EXPORT void glMultiDrawRangeElementArrayAPPLE(GLenum mode, GLuint start, GLuint end, const GLint *first,
                                                  const GLsizei *count, GLsizei n)
{
    if (glm_profile_rejected(glm_current(), 1)) return;
    for (GLsizei i = 0; i < n; ++i) glDrawRangeElementArrayAPPLE(mode, start, end, first[i], count[i]);
}

/* Presents like CGLFlushDrawable, which records into the stream itself. */
GLM_EXPORT void glSwapAPPLE(void) { CGLFlushDrawable(CGLGetCurrentContext()); }

GLM_EXPORT void glFinish(void)
{
    struct glm_context *ctx = glm_current();
    if (ctx && ctx->thread) glm_thread_sync_named(ctx, "glFinish");
    glm_impl_glFinish();
}

/* ---- more shadowed state ------------------------------------------------------ */

void glm_shadow_glPixelStorei(struct glm_context *ctx, GLenum pname, GLint param)
{
    struct glm_shadow *s = shadow(ctx);
    /* Invalid values leave the state alone (glPixelStorei's errors). */
    if (param < 0 || ((pname == GL_UNPACK_ALIGNMENT || pname == GL_PACK_ALIGNMENT) && param != 1 && param != 2 &&
                      param != 4 && param != 8))
        return;
    if (pname == GL_UNPACK_ROW_BYTES_APPLE) s->unpack_row_bytes = param;
    if (pname == GL_UNPACK_IMAGE_BYTES_APPLE) s->unpack_image_bytes = param;
    if ((pname >= GL_UNPACK_SWAP_BYTES && pname <= GL_UNPACK_ALIGNMENT) || (pname >= GL_PACK_SWAP_BYTES && pname <= GL_PACK_ALIGNMENT))
        s->pixel_store[(pname & 0xf) | (pname >= GL_PACK_SWAP_BYTES ? 8 : 0)] = param;
    if (pname == GL_UNPACK_IMAGE_HEIGHT) s->unpack_image_height = param;
    if (pname == GL_UNPACK_SKIP_IMAGES) s->unpack_skip_images = param;
}

size_t glm_pixel_bytes(GLenum format, GLenum type);

/* Bytes a texture image upload reads from client memory under the
   unpack state (GL 2.1 3.6.4); 0 when a pixel unpack buffer is bound (the
   pointer is an offset), (size_t)-1 when unknown (the call syncs). */
size_t glm_marshal_image_size(struct glm_context *ctx, GLsizei w, GLsizei h, GLsizei d, unsigned format, unsigned type)
{
    struct glm_shadow *s = shadow(ctx);
    if (!s->pixel_store_known) return (size_t)-1;
    if (s->pixel_unpack) return 0;
    if (w <= 0 || h <= 0 || d <= 0) return 0;
    size_t bpp = glm_pixel_bytes(format, type);
    if (!bpp || type == GL_BITMAP) return (size_t)-1;
    size_t row_length = s->pixel_store[GL_UNPACK_ROW_LENGTH & 0xf] > 0 ? (size_t)s->pixel_store[GL_UNPACK_ROW_LENGTH & 0xf] : (size_t)w;
    size_t alignment = s->pixel_store[GL_UNPACK_ALIGNMENT & 0xf] > 0 ? (size_t)s->pixel_store[GL_UNPACK_ALIGNMENT & 0xf] : 4;
    size_t row_bytes = s->unpack_row_bytes > 0 ? (size_t)s->unpack_row_bytes
                                               : (row_length * bpp + alignment - 1) / alignment * alignment;
    size_t image_rows = s->unpack_image_height > 0 ? (size_t)s->unpack_image_height : (size_t)h;
    size_t image_bytes = s->unpack_image_bytes > 0 ? (size_t)s->unpack_image_bytes : row_bytes * image_rows;
    size_t skip_rows = (size_t)(s->pixel_store[GL_UNPACK_SKIP_ROWS & 0xf] > 0 ? s->pixel_store[GL_UNPACK_SKIP_ROWS & 0xf] : 0);
    size_t skip_pixels = (size_t)(s->pixel_store[GL_UNPACK_SKIP_PIXELS & 0xf] > 0 ? s->pixel_store[GL_UNPACK_SKIP_PIXELS & 0xf] : 0);
    size_t skip_images = (size_t)(s->unpack_skip_images > 0 ? s->unpack_skip_images : 0);
    return (skip_images + (size_t)d - 1) * image_bytes + (skip_rows + (size_t)h - 1) * row_bytes +
           (skip_pixels + (size_t)w) * bpp;
}
/* Indexed viewports and scissors: index 0 is what glGetIntegerv(GL_VIEWPORT /
   GL_SCISSOR_BOX) reports. Fractional viewports round in the
   implementation: those queries sync. */
static void shadow_viewport0(struct glm_context *ctx, const GLfloat *v)
{
    struct glm_shadow *s = shadow(ctx);
    bool integral = true;
    for (int i = 0; i < 4; ++i) {
        integral &= v[i] == (GLfloat)(GLint)v[i];
        s->viewport[i] = (GLint)v[i];
    }
    s->viewport_known = integral;
}
void glm_shadow_glViewportIndexedf(struct glm_context *ctx, GLuint index, GLfloat x, GLfloat y, GLfloat w, GLfloat h)
{
    const GLfloat v[4] = {x, y, w, h};
    if (index == 0) shadow_viewport0(ctx, v);
}
void glm_shadow_glViewportIndexedfv(struct glm_context *ctx, GLuint index, const GLfloat *v)
{
    if (index == 0 && v) shadow_viewport0(ctx, v);
}
void glm_shadow_glViewportArrayv(struct glm_context *ctx, GLuint first, GLsizei count, const GLfloat *v)
{
    if (first == 0 && count > 0 && v) shadow_viewport0(ctx, v);
}
void glm_shadow_glScissorIndexed(struct glm_context *ctx, GLuint index, GLint left, GLint bottom, GLsizei w, GLsizei h)
{
    if (index == 0) glm_shadow_glScissor(ctx, left, bottom, w, h);
}
void glm_shadow_glScissorIndexedv(struct glm_context *ctx, GLuint index, const GLint *v)
{
    if (index == 0 && v) glm_shadow_glScissor(ctx, v[0], v[1], v[2], v[3]);
}
void glm_shadow_glScissorArrayv(struct glm_context *ctx, GLuint first, GLsizei count, const GLint *v)
{
    if (first == 0 && count > 0 && v) glm_shadow_glScissor(ctx, v[0], v[1], v[2], v[3]);
}

void glm_shadow_glPixelStoref(struct glm_context *ctx, GLenum pname, GLfloat param) { glm_shadow_glPixelStorei(ctx, pname, (GLint)param); }
void glm_shadow_glBindSampler(struct glm_context *ctx, GLuint unit, GLuint sampler)
{
    if (unit < GLM_MAX_TEXTURE_UNITS) shadow(ctx)->samplers[unit] = sampler;
}

/* ---- sync objects -------------------------------------------------------------- */

struct fence_cmd { GLsync sync; };
static void fence_exec(const void *p) { glm_sync_arm(((const struct fence_cmd *)p)->sync); }

GLM_EXPORT GLsync glFenceSync(GLenum condition, GLbitfield flags)
{
    struct glm_context *ctx = glm_current();
    if (ctx && ctx->thread && condition == GL_SYNC_GPU_COMMANDS_COMPLETE && !flags) {
        /* Created now, fenced when the worker reaches it. */
        GLsync sync = glm_sync_new_pending(ctx);
        struct fence_cmd *c = glm_thread_alloc(ctx, sizeof *c, fence_exec);
        c->sync = sync;
        glm_thread_submit(ctx);
        return sync;
    }
    if (ctx && ctx->thread) glm_thread_sync_named(ctx, "glFenceSync");
    return glm_impl_glFenceSync(condition, flags);
}

GLM_EXPORT GLenum glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout)
{
    struct glm_context *ctx = glm_current();
    if (glm_sync_is_pending(sync)) {
        /* Not reached by the worker yet: not signalled. */
        if (!timeout) return GL_TIMEOUT_EXPIRED;
        if (ctx && ctx->thread) glm_thread_sync_named(ctx, "glClientWaitSync");
    }
    return glm_impl_glClientWaitSync(sync, flags, timeout);
}

GLM_EXPORT void glGetSynciv(GLsync sync, GLenum pname, GLsizei size, GLsizei *length, GLint *values)
{
    if (glm_sync_is_pending(sync) && pname == GL_SYNC_STATUS && size > 0) {
        if (length) *length = 1;
        values[0] = GL_UNSIGNALED;
        return;
    }
    glm_impl_glGetSynciv(sync, pname, size, length, values);
}

/* ---- texture queries on the calling thread ---------------------------------------- */

/* The application's binding for `target`, with no texture-object change
   pending in the stream: the implementation's object is current. */
static bool texture_query_ready(struct glm_context *ctx, GLenum target, GLuint *name)
{
    if (!glm_thread_pending_zero(ctx, 1)) return false;
    struct glm_shadow *s = shadow(ctx);
    GLenum binding = target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? GL_TEXTURE_CUBE_MAP : target;
    int slot = texture_slot(binding);
    unsigned unit = s->active_texture - GL_TEXTURE0;
    if (slot < 0 || unit >= GLM_MAX_TEXTURE_UNITS) return false;
    *name = s->textures[unit][slot];
    return true;
}

static bool shadow_level_query(struct glm_context *ctx, GLenum target, GLint level, GLenum pname, GLint *out);

#define TEXTURE_QUERY(call, target)                                                                         \
    struct glm_context *ctx = glm_current();                                                                \
    if (ctx && ctx->thread) {                                                                               \
        GLuint name;                                                                                        \
        if (texture_query_ready(ctx, target, &name)) {                                                      \
            glm_texture_query_override(ctx, target, name);                                                  \
            call;                                                                                           \
            glm_texture_query_override_end();                                                               \
            return;                                                                                         \
        }                                                                                                   \
        glm_thread_sync_named(ctx, __func__);                                                               \
    }                                                                                                       \
    call;

/* Integer parameters from the shadow. */
static bool shadow_tex_query(struct glm_context *ctx, GLenum target, GLenum pname, GLint *out)
{
    if (!glm_texture_pname_in_profile(ctx, pname)) return false;
    struct glm_shadow *s = shadow(ctx);
    int slot = texture_pname_slot(pname);
    int binding = texture_slot(target);
    unsigned unit = s->active_texture - GL_TEXTURE0;
    if (slot < 0 || binding < 0 || unit >= GLM_MAX_TEXTURE_UNITS) return false;
    GLuint name = s->textures[unit][binding];
    struct shadow_texture *t = name ? shadow_texture(s, name, false) : NULL;
    if (!t || !t->known) return false;
    *out = t->p[slot];
    return true;
}

GLM_EXPORT void glGetTexParameteriv(GLenum target, GLenum pname, GLint *params)
{
    struct glm_context *early = glm_current();
    if (early && early->thread && shadow_tex_query(early, target, pname, params)) return;
    TEXTURE_QUERY(glm_impl_glGetTexParameteriv(target, pname, params), target)
}
GLM_EXPORT void glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params)
{
    TEXTURE_QUERY(glm_impl_glGetTexParameterfv(target, pname, params), target)
}
GLM_EXPORT void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params)
{
    struct glm_context *early = glm_current();
    if (early && early->thread && shadow_level_query(early, target, level, pname, params)) return;
    TEXTURE_QUERY(glm_impl_glGetTexLevelParameteriv(target, level, pname, params), target)
}
GLM_EXPORT void glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat *params)
{
    TEXTURE_QUERY(glm_impl_glGetTexLevelParameterfv(target, level, pname, params), target)
}

/* ---- buffers ----------------------------------------------------------------------- */

GLM_EXPORT void glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params)
{
    struct glm_context *ctx = glm_current();
    if (ctx && ctx->thread) {
        struct glm_shadow *s = shadow(ctx);
        GLuint name = shadow_bound_buffer(s, target);
        struct shadow_buffer *b = name != ~0u ? shadow_buffer(s, name, false) : NULL;
        if (b && pname == GL_BUFFER_SIZE) { *params = (GLint)b->size; return; }
        if (b && pname == GL_BUFFER_USAGE) { *params = (GLint)b->usage; return; }
        glm_thread_sync_named(ctx, "glGetBufferParameteriv");
    }
    glm_impl_glGetBufferParameteriv(target, pname, params);
}
GLM_EXPORT void glGetBufferParameterivARB(GLenum target, GLenum pname, GLint *params) { glGetBufferParameteriv(target, pname, params); }

struct buffer_data_cmd { GLenum target, usage; GLsizeiptr size; void *storage; };
static void buffer_data_exec(const void *p)
{
    const struct buffer_data_cmd *c = p;
    glm_buffer_data_with_storage(glm_current(), c->target, c->size, c->usage, c->storage);
}

GLM_EXPORT void glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage)
{
    struct glm_context *ctx = glm_current();
    bool valid = glm_buffer_usage_valid(usage);
    if (ctx) {
        struct glm_shadow *s = shadow(ctx);
        GLuint name = shadow_bound_buffer(s, target);
        struct shadow_buffer *b = name != ~0u && name ? shadow_buffer(s, name, true) : NULL;
        if (b && size >= 0 && valid) {
            b->size = size;
            b->usage = usage;
        }
    }
    if (ctx && ctx->thread && size >= 0) {
        /* The new storage is made and filled here; the worker installs it
           in order (earlier draws keep the storage they saw). */
        struct buffer_data_cmd *c = glm_thread_alloc(ctx, sizeof *c, buffer_data_exec);
        *c = (struct buffer_data_cmd){target, usage, size, valid ? glm_backend_new_buffer_storage((size_t)size, data) : NULL};
        return;
    }
    glm_impl_glBufferData(target, size, data, usage);
}
GLM_EXPORT void glBufferDataARB(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage) { glBufferData(target, size, data, usage); }

struct buffer_sub_data_cmd { GLenum target; GLintptr offset; GLsizeiptr size; void *staging; /* or inline data follows */ };
static void buffer_sub_data_exec(const void *p)
{
    const struct buffer_sub_data_cmd *c = p;
    if (c->staging) {
        glm_impl_glBufferSubData(c->target, c->offset, c->size, glm_backend_storage_contents(c->staging));
        glm_backend_release(c->staging);
    } else {
        glm_impl_glBufferSubData(c->target, c->offset, c->size, c + 1);
    }
}

GLM_EXPORT void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data)
{
    struct glm_context *ctx = glm_current();
    if (ctx && ctx->thread && size >= 0 && data) {
        bool inline_data = size <= GLM_MARSHAL_MAX;
        struct buffer_sub_data_cmd *c = glm_thread_alloc(ctx, sizeof *c + (inline_data ? (size_t)size : 0), buffer_sub_data_exec);
        *c = (struct buffer_sub_data_cmd){target, offset, size, NULL};
        if (inline_data) memcpy(c + 1, data, (size_t)size);
        else c->staging = glm_backend_new_buffer_storage((size_t)size, data);
        return;
    }
    glm_impl_glBufferSubData(target, offset, size, data);
}
GLM_EXPORT void glBufferSubDataARB(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data)
{
    glBufferSubData(target, offset, size, data);
}

void glm_shadow_glDeleteTextures(struct glm_context *ctx, GLsizei n, const GLuint *names)
{
    struct glm_shadow *s = shadow(ctx);
    bool removed = false;
    for (GLsizei i = 0; i < n && names; ++i) {
        struct shadow_texture *t = shadow_texture(s, names[i], false);
        if (!t) continue;
        free(t->levels);
        *t = s->textures_params[--s->texture_count];
        removed = true;
        if (s->texture_index_capacity) texture_index_rebuild(s); /* slots moved */
    }
    (void)removed;
}

/* ---- texture level sizes ----------------------------------------------------- */

static struct shadow_texture *bound_shadow_texture(struct glm_context *ctx, GLenum target, int *face)
{
    struct glm_shadow *s = shadow(ctx);
    *face = target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z
                ? (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
    GLenum binding = *face || target == GL_TEXTURE_CUBE_MAP_POSITIVE_X ? GL_TEXTURE_CUBE_MAP : target;
    int slot = texture_slot(binding);
    unsigned unit = s->active_texture - GL_TEXTURE0;
    if (slot < 0 || unit >= GLM_MAX_TEXTURE_UNITS) return NULL;
    GLuint name = s->textures[unit][slot];
    return name ? shadow_texture(s, name, true) : NULL;
}

static void shadow_define_level(struct glm_context *ctx, GLenum target, GLint level, GLenum internal, GLsizei w,
                                GLsizei h, GLsizei d)
{
    /* A rejected image definition must not overwrite cached level queries. */
    struct glm_format_info info;
    if (w < 0 || h < 0 || d < 0 ||
        !glm_texture_format_allowed(internal, ctx->profile == GLM_PROFILE_CORE) ||
        !glm_format_lookup(internal, &info) ||
        (target == GL_TEXTURE_3D && (info.depth || info.stencil))) return;
    int face;
    struct shadow_texture *t = bound_shadow_texture(ctx, target, &face);
    if (!t || level < 0 || level >= 16) return;
    /* Complete knowledge starts with the base level. */
    if (!t->levels) {
        if (level != 0) return;
        t->levels = calloc(1, sizeof *t->levels);
    }
    t->levels->width[face][level] = w;
    t->levels->height[face][level] = h;
    t->levels->depth[face][level] = d;
    t->levels->format[face][level] = (GLint)internal;
}

void glm_shadow_glTexImage1D(struct glm_context *ctx, GLenum target, GLint level, GLint internal, GLsizei w, GLint border,
                             GLenum format, GLenum type, const GLvoid *pixels)
{
    shadow_define_level(ctx, target, level, (GLenum)internal, w, 1, 1);
}
void glm_shadow_glTexImage2D(struct glm_context *ctx, GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
                             GLint border, GLenum format, GLenum type, const GLvoid *pixels)
{
    shadow_define_level(ctx, target, level, (GLenum)internal, w, h, 1);
}
void glm_shadow_glTexImage3D(struct glm_context *ctx, GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
                             GLsizei d, GLint border, GLenum format, GLenum type, const GLvoid *pixels)
{
    if (ctx->profile == GLM_PROFILE_CORE && target == GL_TEXTURE_3D) {
        switch (internal) {
        case GL_COMPRESSED_RED_RGTC1: internal = GL_R8; break;
        case GL_COMPRESSED_SIGNED_RED_RGTC1: internal = GL_R8_SNORM; break;
        case GL_COMPRESSED_RG_RGTC2: internal = GL_RG8; break;
        case GL_COMPRESSED_SIGNED_RG_RGTC2: internal = GL_RG8_SNORM; break;
        }
    }
    if (glm_tex_image_error((GLenum)internal, format, type, ctx->profile == GLM_PROFILE_CORE)) return;
    shadow_define_level(ctx, target, level, (GLenum)internal, w, h, d);
}
void glm_shadow_glCompressedTexImage2D(struct glm_context *ctx, GLenum target, GLint level, GLenum internal, GLsizei w,
                                       GLsizei h, GLint border, GLsizei size, const GLvoid *data)
{
    shadow_define_level(ctx, target, level, internal, w, h, 1);
}
void glm_shadow_glCompressedTexImage2DARB(struct glm_context *ctx, GLenum target, GLint level, GLenum internal,
                                          GLsizei w, GLsizei h, GLint border, GLsizei size, const GLvoid *data)
{
    shadow_define_level(ctx, target, level, internal, w, h, 1);
}
void glm_shadow_glCopyTexImage2D(struct glm_context *ctx, GLenum target, GLint level, GLenum internal, GLint x, GLint y,
                                 GLsizei w, GLsizei h, GLint border)
{
    /* The source framebuffer can reject the definition. Let subsequent
       level queries wait for the actual result instead of predicting it. */
    int face;
    struct shadow_texture *t = bound_shadow_texture(ctx, target, &face);
    if (t) {
        free(t->levels);
        t->levels = NULL;
    }
}
static void shadow_storage(struct glm_context *ctx, GLenum target, GLsizei levels, GLenum internal, GLsizei w, GLsizei h,
                           GLsizei d)
{
    int faces = target == GL_TEXTURE_CUBE_MAP ? 6 : 1;
    for (GLsizei level = 0; level < levels && level < 16; ++level)
        for (int f = 0; f < faces; ++f)
            shadow_define_level(ctx, faces == 6 ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)f : target, level, internal,
                                w >> level ? w >> level : 1,
                                target == GL_TEXTURE_1D_ARRAY ? h : (h >> level ? h >> level : 1),
                                target == GL_TEXTURE_3D ? (d >> level ? d >> level : 1) : d);
}
void glm_shadow_glTexStorage1D(struct glm_context *ctx, GLenum target, GLsizei levels, GLenum internal, GLsizei w)
{
    shadow_storage(ctx, target, levels, internal, w, 1, 1);
}
void glm_shadow_glTexStorage2D(struct glm_context *ctx, GLenum target, GLsizei levels, GLenum internal, GLsizei w, GLsizei h)
{
    shadow_storage(ctx, target, levels, internal, w, h, 1);
}
void glm_shadow_glTexStorage3D(struct glm_context *ctx, GLenum target, GLsizei levels, GLenum internal, GLsizei w,
                               GLsizei h, GLsizei d)
{
    shadow_storage(ctx, target, levels, internal, w, h, d);
}
/* Every level below the base, sized from it. */
void glm_shadow_glGenerateMipmap(struct glm_context *ctx, GLenum target)
{
    int face;
    struct shadow_texture *t = bound_shadow_texture(ctx, target, &face);
    if (!t || !t->levels) return;
    int faces = target == GL_TEXTURE_CUBE_MAP ? 6 : 1;
    int base = t->known ? t->p[5] : 0;
    if (base < 0 || base >= 16) return;
    for (int f = 0; f < faces; ++f) {
        GLint w = t->levels->width[f][base], h = t->levels->height[f][base], d = t->levels->depth[f][base];
        for (int level = base + 1; level < 16 && (w > 1 || h > 1 || (target == GL_TEXTURE_3D && d > 1)); ++level) {
            w = w > 1 ? w / 2 : 1;
            h = h > 1 && target != GL_TEXTURE_1D_ARRAY ? h / 2 : h;
            d = target == GL_TEXTURE_3D && d > 1 ? d / 2 : d;
            t->levels->width[f][level] = w;
            t->levels->height[f][level] = h;
            t->levels->depth[f][level] = d;
            t->levels->format[f][level] = t->levels->format[f][base];
        }
    }
}
void glm_shadow_glGenerateMipmapEXT(struct glm_context *ctx, GLenum target) { glm_shadow_glGenerateMipmap(ctx, target); }

/* Sizes and formats of levels from the shadow; false when unknown. */
static bool shadow_level_query(struct glm_context *ctx, GLenum target, GLint level, GLenum pname, GLint *out)
{
    if (pname != GL_TEXTURE_WIDTH && pname != GL_TEXTURE_HEIGHT && pname != GL_TEXTURE_DEPTH &&
        pname != GL_TEXTURE_INTERNAL_FORMAT)
        return false;
    if (level < 0 || level >= 16) return false;
    struct glm_shadow *s = shadow(ctx);
    int face = target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z
                   ? (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
    GLenum binding = face || target == GL_TEXTURE_CUBE_MAP_POSITIVE_X ? GL_TEXTURE_CUBE_MAP : target;
    int slot = texture_slot(binding);
    unsigned unit = s->active_texture - GL_TEXTURE0;
    if (slot < 0 || unit >= GLM_MAX_TEXTURE_UNITS) return false;
    GLuint name = s->textures[unit][slot];
    struct shadow_texture *t = name ? shadow_texture(s, name, false) : NULL;
    if (!t || !t->levels) return false;
    bool defined = t->levels->width[face][level] != 0;
    switch (pname) {
    case GL_TEXTURE_WIDTH: *out = t->levels->width[face][level]; break;
    case GL_TEXTURE_HEIGHT: *out = t->levels->height[face][level]; break;
    case GL_TEXTURE_DEPTH: *out = t->levels->depth[face][level]; break;
    default: {
        /* As the level query reports it: Apple's sized format. */
        const struct glm_apple_texture *ref = defined ? glm_apple_texture_format(ctx, (GLenum)t->levels->format[face][level]) : NULL;
        *out = !defined ? 1 : ref ? ref->reported : t->levels->format[face][level];
        break;
    }
    }
    return true;
}
void glm_shadow_glDeleteTexturesEXT(struct glm_context *ctx, GLsizei n, const GLuint *names) { glm_shadow_glDeleteTextures(ctx, n, names); }

/* ---- display lists ------------------------------------------------------------ */

/* A compiled list: commands (header + payload, as in the command stream)
   in chunks that never move, so payload pointers into them stay valid.
   Lists are shared by contexts sharing objects; a glCallList recorded in
   the stream holds a reference, so recompiling or deleting the list does
   not change what it replays. */
struct list_chunk {
    struct list_chunk *next;
    size_t used, size;
    uint8_t data[];
};

struct glm_display_list {
    int refs;
    struct list_chunk *first, *last;
    bool shadowed; /* changes state the client-side shadow follows */
};

struct glm_list_build {
    GLuint name;
    GLenum mode;
    struct glm_display_list *list;
};

struct list_command {
    glm_exec_fn exec;
    uint32_t size;
    uint32_t pad;
};

static pthread_mutex_t list_lock = PTHREAD_MUTEX_INITIALIZER;

static struct glm_display_list *list_get(struct glm_context *ctx, GLuint name)
{
    return name ? glm_table_get(&ctx->share->lists, name) : NULL;
}

static void list_release(struct glm_display_list *l)
{
    if (!l) return;
    pthread_mutex_lock(&list_lock);
    bool last = --l->refs == 0;
    pthread_mutex_unlock(&list_lock);
    if (!last) return;
    for (struct list_chunk *c = l->first, *next; c; c = next) {
        next = c->next;
        free(c);
    }
    free(l);
}

static struct glm_display_list *list_acquire(struct glm_context *ctx, GLuint name)
{
    pthread_mutex_lock(&list_lock);
    struct glm_display_list *l = list_get(ctx, name);
    if (l) ++l->refs;
    pthread_mutex_unlock(&list_lock);
    return l;
}

void *glm_list_alloc(struct glm_context *ctx, size_t size, glm_exec_fn exec)
{
    struct glm_display_list *l = ctx->listing->list;
    size_t total = (sizeof(struct list_command) + size + 7) & ~(size_t)7;
    if (!l->last || l->last->used + total > l->last->size) {
        size_t chunk = total > 60000 ? total : 60000;
        struct list_chunk *c = malloc(sizeof *c + chunk);
        c->next = NULL;
        c->used = 0;
        c->size = chunk;
        if (l->last) l->last->next = c;
        else l->first = c;
        l->last = c;
    }
    struct list_command *h = (struct list_command *)(l->last->data + l->last->used);
    h->exec = exec;
    h->size = (uint32_t)total;
    l->last->used += total;
    return h + 1;
}

int glm_list_executes(struct glm_context *ctx) { return ctx->listing && ctx->listing->mode == GL_COMPILE_AND_EXECUTE; }
void glm_list_note_shadowed(struct glm_context *ctx) { ctx->listing->list->shadowed = true; }

void glm_list_unsupported(struct glm_context *ctx, const char *name)
{
    static const char *reported[32];
    for (int i = 0; i < 32; ++i) {
        if (reported[i] == name) return;
        if (!reported[i]) {
            reported[i] = name;
            break;
        }
    }
    (void)ctx;
    glm_log("%s inside a display list runs at compile time (not recorded)", name);
}

/* Replays a list on the executing thread (worker or caller). */
static __thread int replay_depth;
static void list_replay(struct glm_display_list *l)
{
    if (!l || replay_depth >= 64) return; /* GL_MAX_LIST_NESTING */
    ++replay_depth;
    for (struct list_chunk *c = l->first; c; c = c->next)
        for (size_t at = 0; at < c->used;) {
            const struct list_command *h = (const struct list_command *)(c->data + at);
            h->exec(h + 1);
            at += h->size;
        }
    --replay_depth;
}

GLM_EXPORT GLuint glGenLists(GLsizei range)
{
    struct glm_context *ctx = glm_current();
    if (glm_profile_rejected(ctx, 1)) return 0;
    if (!ctx) return 0;
    if (range < 0) {
        glm_thread_sync(ctx);
        glm_error(ctx, GL_INVALID_VALUE);
        return 0;
    }
    if (!range) return 0;
    pthread_mutex_lock(&list_lock);
    /* `range` consecutive unused names. */
    GLuint base = 1;
    for (GLuint n = 1; n - base < (GLuint)range; ++n)
        if (glm_table_get(&ctx->share->lists, n) || glm_table_get(&ctx->share->list_names, n)) base = n + 1;
    for (GLuint n = base; n < base + (GLuint)range; ++n) glm_table_set(&ctx->share->list_names, n, (void *)1);
    pthread_mutex_unlock(&list_lock);
    return base;
}

GLM_EXPORT GLboolean glIsList(GLuint name)
{
    struct glm_context *ctx = glm_current();
    if (glm_profile_rejected(ctx, 1)) return 0;
    if (!ctx || !name) return GL_FALSE;
    pthread_mutex_lock(&list_lock);
    /* glGenLists creates empty lists (GL 2.1 5.4). */
    bool exists = glm_table_get(&ctx->share->lists, name) || glm_table_get(&ctx->share->list_names, name);
    pthread_mutex_unlock(&list_lock);
    return exists;
}

GLM_EXPORT void glDeleteLists(GLuint first, GLsizei range)
{
    struct glm_context *ctx = glm_current();
    if (glm_profile_rejected(ctx, 1)) return;
    if (!ctx) return;
    if (range < 0) {
        glm_thread_sync(ctx);
        return glm_error(ctx, GL_INVALID_VALUE);
    }
    /* Only names that can hold a list (the range may be huge). */
    uint64_t end = (uint64_t)first + (uint64_t)range;
    pthread_mutex_lock(&list_lock);
    uint64_t used = ctx->share->lists.capacity > ctx->share->list_names.capacity ? ctx->share->lists.capacity : ctx->share->list_names.capacity;
    pthread_mutex_unlock(&list_lock);
    if (end > used) end = used;
    for (uint64_t n = first; n < end; ++n) {
        pthread_mutex_lock(&list_lock);
        struct glm_display_list *l = glm_table_get(&ctx->share->lists, n);
        glm_table_set(&ctx->share->lists, n, NULL);
        glm_table_set(&ctx->share->list_names, n, NULL);
        pthread_mutex_unlock(&list_lock);
        list_release(l);
    }
}

GLM_EXPORT void glNewList(GLuint name, GLenum mode)
{
    struct glm_context *ctx = glm_current();
    if (glm_profile_rejected(ctx, 1)) return;
    if (!ctx) return;
    if (!name || (mode != GL_COMPILE && mode != GL_COMPILE_AND_EXECUTE) || ctx->listing) {
        glm_thread_sync(ctx);
        return glm_error(ctx, !name ? GL_INVALID_VALUE : ctx->listing ? GL_INVALID_OPERATION : GL_INVALID_ENUM);
    }
    struct glm_list_build *b = calloc(1, sizeof *b);
    b->name = name;
    b->mode = mode;
    b->list = calloc(1, sizeof *b->list);
    b->list->refs = 1;
    ctx->listing = b;
}

GLM_EXPORT void glEndList(void)
{
    struct glm_context *ctx = glm_current();
    if (!ctx) return;
    struct glm_list_build *b = ctx->listing;
    if (!b) {
        glm_thread_sync(ctx);
        return glm_error(ctx, GL_INVALID_OPERATION);
    }
    ctx->listing = NULL;
    pthread_mutex_lock(&list_lock);
    struct glm_display_list *old = glm_table_get(&ctx->share->lists, b->name);
    glm_table_set(&ctx->share->lists, b->name, b->list);
    glm_table_set(&ctx->share->list_names, b->name, (void *)1);
    pthread_mutex_unlock(&list_lock);
    list_release(old);
    free(b);
}

GLM_EXPORT void glListBase(GLuint base)
{
    struct glm_context *ctx = glm_current();
    if (glm_profile_rejected(ctx, 1)) return;
    if (ctx) ctx->list_base = base;
}

/* The stream holds the lists by reference until they have run. */
struct call_lists_cmd {
    int count;
    struct glm_display_list *lists[];
};

static void call_lists_exec(const void *payload)
{
    const struct call_lists_cmd *c = payload;
    struct glm_context *ctx = glm_current();
    GLM_STATE_CHANGED(ctx);
    for (int i = 0; i < c->count; ++i) {
        list_replay(c->lists[i]);
        list_release(c->lists[i]);
    }
    glm_thread_state_executed(ctx);
}

/* Inside another list: resolved when that list runs. */
struct call_list_name_cmd {
    GLuint name;
};
static void call_list_name_exec(const void *payload)
{
    const struct call_list_name_cmd *c = payload;
    struct glm_context *ctx = glm_current();
    struct glm_display_list *l = ctx ? list_acquire(ctx, c->name) : NULL;
    list_replay(l);
    list_release(l);
}

static void call_lists(struct glm_context *ctx, const GLuint *names, int count)
{
    if (ctx->listing) {
        for (int i = 0; i < count; ++i) {
            struct call_list_name_cmd *c = glm_list_alloc(ctx, sizeof *c, call_list_name_exec);
            c->name = names[i];
        }
        ctx->listing->list->shadowed = true;
        if (!glm_list_executes(ctx)) return;
    }
    struct glm_display_list **lists = malloc(sizeof *lists * (size_t)(count ? count : 1));
    bool shadowed = false;
    int n = 0;
    for (int i = 0; i < count; ++i) {
        struct glm_display_list *l = list_acquire(ctx, names[i]);
        if (!l) continue;
        shadowed |= l->shadowed;
        lists[n++] = l;
    }
    if (n) {
        if (ctx->thread) {
            struct call_lists_cmd *c = glm_thread_alloc(ctx, sizeof *c + sizeof *lists * (size_t)n, call_lists_exec);
            c->count = n;
            memcpy(c->lists, lists, sizeof *lists * (size_t)n);
            glm_thread_state_recorded(ctx);
        } else {
            GLM_STATE_CHANGED(ctx);
            for (int i = 0; i < n; ++i) {
                list_replay(lists[i]);
                list_release(lists[i]);
            }
        }
    }
    free(lists);
    /* Lists that bind or change shadowed state leave the shadow stale. */
    if (shadowed) {
        if (ctx->thread) glm_thread_sync_named(ctx, "glCallList");
        glm_shadow_refresh(ctx);
    }
}

GLM_EXPORT void glCallList(GLuint name)
{
    struct glm_context *ctx = glm_current();
    if (glm_profile_rejected(ctx, 1)) return;
    if (ctx) call_lists(ctx, &name, 1);
}

GLM_EXPORT void glCallLists(GLsizei count, GLenum type, const GLvoid *lists)
{
    struct glm_context *ctx = glm_current();
    if (glm_profile_rejected(ctx, 1)) return;
    if (!ctx || count <= 0 || !lists) return;
    GLuint *names = malloc(sizeof *names * (size_t)count);
    const uint8_t *p = lists;
    for (GLsizei i = 0; i < count; ++i) {
        GLuint v = 0;
        switch (type) {
        case GL_BYTE: v = (GLuint)(GLint)((const GLbyte *)p)[i]; break;
        case GL_UNSIGNED_BYTE: v = p[i]; break;
        case GL_SHORT: v = (GLuint)(GLint)((const GLshort *)p)[i]; break;
        case GL_UNSIGNED_SHORT: v = ((const GLushort *)p)[i]; break;
        case GL_INT: case GL_UNSIGNED_INT: v = ((const GLuint *)p)[i]; break;
        case GL_FLOAT: v = (GLuint)((const GLfloat *)p)[i]; break;
        case GL_2_BYTES: v = (GLuint)p[i * 2] << 8 | p[i * 2 + 1]; break;
        case GL_3_BYTES: v = (GLuint)p[i * 3] << 16 | (GLuint)p[i * 3 + 1] << 8 | p[i * 3 + 2]; break;
        case GL_4_BYTES: v = (GLuint)p[i * 4] << 24 | (GLuint)p[i * 4 + 1] << 16 | (GLuint)p[i * 4 + 2] << 8 | p[i * 4 + 3]; break;
        default:
            free(names);
            if (ctx->thread) glm_thread_sync(ctx);
            return glm_error(ctx, GL_INVALID_ENUM);
        }
        names[i] = v + ctx->list_base;
    }
    call_lists(ctx, names, (int)count);
    free(names);
}


/* ---- shaders and programs on the application's thread ------------------------

   Games create, compile and link shaders in the middle of a frame and query
   them right away, including glGetShaderiv and glGetAttribLocation. Answered through the stream, each query waits for every
   queued command and then the whole compile. Here names are reserved on this
   thread, shaders are checked here, and a link runs the front end here and
   starts the Metal compiles in the background: queries answer from that
   shadow program, and the stream's own link finds the results cached. Objects
   this registry did not see (created before the stream started, or by an
   unthreaded context) take the stream. */

#include "../programs.h"
#include "../shader_compiler.h"

GLuint glm_impl_glCreateShader(GLenum type);
GLuint glm_impl_glCreateProgram(void);
void glm_impl_glShaderSource(GLuint name, GLsizei count, const GLchar *const *strings, const GLint *lengths);
void glm_impl_glCompileShader(GLuint name);
void glm_impl_glGetShaderiv(GLuint name, GLenum pname, GLint *params);
void glm_impl_glGetShaderInfoLog(GLuint name, GLsizei size, GLsizei *length, GLchar *log);
void glm_impl_glLinkProgram(GLuint name);
void glm_impl_glGetProgramiv(GLuint name, GLenum pname, GLint *params);
void glm_impl_glGetProgramInfoLog(GLuint name, GLsizei size, GLsizei *length, GLchar *log);
GLint glm_impl_glGetAttribLocation(GLuint name, const GLchar *attribute);
GLint glm_impl_glGetUniformLocation(GLuint name, const GLchar *uniform);

struct app_shader {
    GLenum type;
    char *source;
    bool checked, ok;
    char *log;
};

struct app_program {
    GLuint shaders[8];
    int shader_count;
    struct glm_name_location *attributes, *outputs;
    int attribute_count, output_count;
    bool untracked;              /* transform feedback, or a link this registry could not follow */
    struct glm_program *shadow;  /* the last link as seen from here */
};

struct app_objects {
    pthread_mutex_t lock;
    struct glm_object_table shaders, programs; /* struct app_shader / app_program by name */
};

static struct app_objects *app_objects(struct glm_context *ctx)
{
    struct glm_share_group *share = ctx->share;
    struct app_objects *o = __atomic_load_n((struct app_objects **)&share->app_objects, __ATOMIC_ACQUIRE);
    if (o) return o;
    pthread_mutex_lock(&share->lock);
    o = share->app_objects;
    if (!o) {
        o = calloc(1, sizeof *o);
        pthread_mutex_init(&o->lock, NULL);
        __atomic_store_n((struct app_objects **)&share->app_objects, o, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&share->lock);
    return o;
}

static enum glm_stage app_stage(GLenum type)
{
    switch (type) {
    case GL_FRAGMENT_SHADER: return GLM_STAGE_FRAGMENT;
    case GL_GEOMETRY_SHADER: return GLM_STAGE_GEOMETRY;
    case GL_TESS_CONTROL_SHADER: return GLM_STAGE_TESS_CONTROL;
    case GL_TESS_EVALUATION_SHADER: return GLM_STAGE_TESS_EVALUATION;
    default: return GLM_STAGE_VERTEX;
    }
}

static void app_free_program(struct app_program *p)
{
    if (!p) return;
    for (int i = 0; i < p->attribute_count; ++i) free((char *)p->attributes[i].name);
    for (int i = 0; i < p->output_count; ++i) free((char *)p->outputs[i].name);
    free(p->attributes);
    free(p->outputs);
    glm_program_shadow_free(p->shadow);
    free(p);
}

static void app_free_shader(struct app_shader *s)
{
    if (!s) return;
    free(s->source);
    free(s->log);
    free(s);
}

static void app_set_binding(struct glm_name_location **list, int *count, const char *name, int location)
{
    for (int i = 0; i < *count; ++i)
        if (!strcmp((*list)[i].name, name)) {
            (*list)[i].location = location;
            return;
        }
    *list = realloc(*list, (size_t)(*count + 1) * sizeof **list);
    (*list)[*count].name = strdup(name);
    (*list)[*count].location = location;
    ++*count;
}

/* Object creation under a name reserved here. */
struct create_named_cmd { GLenum type; GLuint name; };
static void create_shader_exec(const void *payload)
{
    const struct create_named_cmd *c = payload;
    struct glm_context *ctx = glm_current();
    GLM_STATE_CHANGED(ctx);
    glm_create_shader_named(ctx, c->type, c->name);
    glm_thread_state_executed(ctx);
}
static void create_program_exec(const void *payload)
{
    const struct create_named_cmd *c = payload;
    struct glm_context *ctx = glm_current();
    GLM_STATE_CHANGED(ctx);
    glm_create_program_named(ctx, c->name);
    glm_thread_state_executed(ctx);
}

static GLuint reserve_name(struct glm_context *ctx)
{
    pthread_mutex_lock(&ctx->share->lock);
    GLuint name = glm_table_reserve(&ctx->share->shaders);
    pthread_mutex_unlock(&ctx->share->lock);
    return name;
}

GLM_EXPORT GLuint glCreateShader(GLenum type)
{
    struct glm_context *ctx = glm_current();
    bool valid = type == GL_VERTEX_SHADER || type == GL_FRAGMENT_SHADER || type == GL_GEOMETRY_SHADER ||
                 type == GL_TESS_CONTROL_SHADER || type == GL_TESS_EVALUATION_SHADER;
    if (!ctx || !ctx->thread || !valid) {
        if (ctx && ctx->thread) glm_thread_sync_named(ctx, "glCreateShader");
        return glm_impl_glCreateShader(type);
    }
    GLuint name = reserve_name(ctx);
    struct app_objects *o = app_objects(ctx);
    struct app_shader *s = calloc(1, sizeof *s);
    s->type = type;
    pthread_mutex_lock(&o->lock);
    app_free_shader(glm_table_get(&o->shaders, name));
    glm_table_set(&o->shaders, name, s);
    pthread_mutex_unlock(&o->lock);
    glm_thread_state_recorded(ctx);
    struct create_named_cmd *c = glm_thread_alloc(ctx, sizeof *c, create_shader_exec);
    *c = (struct create_named_cmd){type, name};
    return name;
}

GLM_EXPORT GLuint glCreateProgram(void)
{
    struct glm_context *ctx = glm_current();
    if (!ctx || !ctx->thread) return glm_impl_glCreateProgram();
    GLuint name = reserve_name(ctx);
    struct app_objects *o = app_objects(ctx);
    pthread_mutex_lock(&o->lock);
    app_free_program(glm_table_get(&o->programs, name));
    glm_table_set(&o->programs, name, calloc(1, sizeof(struct app_program)));
    pthread_mutex_unlock(&o->lock);
    glm_thread_state_recorded(ctx);
    struct create_named_cmd *c = glm_thread_alloc(ctx, sizeof *c, create_program_exec);
    *c = (struct create_named_cmd){0, name};
    return name;
}

struct shader_source_cmd { GLuint name; char *source; };
static void shader_source_exec(const void *payload)
{
    const struct shader_source_cmd *c = payload;
    const char *text = c->source;
    glm_impl_glShaderSource(c->name, 1, &text, NULL);
    free(c->source);
}

GLM_EXPORT void glShaderSource(GLuint name, GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
    struct glm_context *ctx = glm_current();
    if (!ctx || !ctx->thread || count < 0 || (count && !strings)) {
        if (ctx && ctx->thread) glm_thread_sync_named(ctx, "glShaderSource");
        glm_impl_glShaderSource(name, count, strings, lengths);
        return;
    }
    size_t total = 0;
    for (GLsizei i = 0; i < count; ++i) total += lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
    char *source = malloc(total + 1);
    size_t at = 0;
    for (GLsizei i = 0; i < count; ++i) {
        size_t n = lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
        memcpy(source + at, strings[i], n);
        at += n;
    }
    source[at] = 0;
    struct app_objects *o = app_objects(ctx);
    pthread_mutex_lock(&o->lock);
    struct app_shader *s = glm_table_get(&o->shaders, name);
    if (s) {
        free(s->source);
        s->source = strdup(source);
        s->checked = false;
    }
    pthread_mutex_unlock(&o->lock);
    struct shader_source_cmd *c = glm_thread_alloc(ctx, sizeof *c, shader_source_exec);
    *c = (struct shader_source_cmd){name, source};
}

struct compiled_cmd { GLuint name; bool checked, ok; char *log; };
static void compiled_exec(const void *payload)
{
    const struct compiled_cmd *c = payload;
    if (c->checked) glm_shader_set_compiled(glm_current(), c->name, c->ok, c->log);
    else glm_impl_glCompileShader(c->name);
    free(c->log);
}

GLM_EXPORT void glCompileShader(GLuint name)
{
    struct glm_context *ctx = glm_current();
    if (!ctx || !ctx->thread) {
        glm_impl_glCompileShader(name);
        return;
    }
    struct app_objects *o = app_objects(ctx);
    pthread_mutex_lock(&o->lock);
    struct app_shader *s = glm_table_get(&o->shaders, name);
    char *source = s && s->source ? strdup(s->source) : NULL;
    GLenum type = s ? s->type : 0;
    pthread_mutex_unlock(&o->lock);
    struct compiled_cmd result = {name, false, false, NULL};
    if (source) {
        char *log = glm_shader_version_error(source, ctx->profile == GLM_PROFILE_CORE);
        bool ok = !log && glm_shader_check(app_stage(type), source, &log);
        pthread_mutex_lock(&o->lock);
        s = glm_table_get(&o->shaders, name);
        if (s) {
            free(s->log);
            s->log = log ? strdup(log) : NULL;
            s->ok = ok;
            s->checked = true;
        }
        pthread_mutex_unlock(&o->lock);
        result = (struct compiled_cmd){name, true, ok, log};
        free(source);
    }
    struct compiled_cmd *c = glm_thread_alloc(ctx, sizeof *c, compiled_exec);
    *c = result;
}

static void copy_out(const char *text, GLsizei size, GLsizei *length, GLchar *out)
{
    size_t n = text ? strlen(text) : 0;
    if (size <= 0 || !out) {
        if (length) *length = 0;
        return;
    }
    if (n > (size_t)size - 1) n = (size_t)size - 1;
    if (text) memcpy(out, text, n);
    out[n] = 0;
    if (length) *length = (GLsizei)n;
}

GLM_EXPORT void glGetShaderiv(GLuint name, GLenum pname, GLint *params)
{
    struct glm_context *ctx = glm_current();
    if (ctx && ctx->thread && params) {
        struct app_objects *o = app_objects(ctx);
        pthread_mutex_lock(&o->lock);
        struct app_shader *s = glm_table_get(&o->shaders, name);
        bool answered = false;
        if (s) {
            answered = true;
            switch (pname) {
            case GL_SHADER_TYPE: *params = (GLint)s->type; break;
            case GL_COMPILE_STATUS: if (s->checked) *params = s->ok; else answered = false; break;
            case GL_INFO_LOG_LENGTH:
                if (s->checked) *params = s->log && *s->log ? (GLint)strlen(s->log) + 1 : 0;
                else answered = false;
                break;
            case GL_SHADER_SOURCE_LENGTH: *params = s->source ? (GLint)strlen(s->source) + 1 : 0; break;
            default: answered = false;
            }
        }
        pthread_mutex_unlock(&o->lock);
        if (answered) return;
        glm_thread_sync_named(ctx, "glGetShaderiv");
    }
    glm_impl_glGetShaderiv(name, pname, params);
}

GLM_EXPORT void glGetShaderInfoLog(GLuint name, GLsizei size, GLsizei *length, GLchar *log)
{
    struct glm_context *ctx = glm_current();
    if (ctx && ctx->thread) {
        struct app_objects *o = app_objects(ctx);
        pthread_mutex_lock(&o->lock);
        struct app_shader *s = glm_table_get(&o->shaders, name);
        bool answered = s && s->checked;
        if (answered) copy_out(s->log, size, length, log);
        pthread_mutex_unlock(&o->lock);
        if (answered) return;
        glm_thread_sync_named(ctx, "glGetShaderInfoLog");
    }
    glm_impl_glGetShaderInfoLog(name, size, length, log);
}

/* Registry upkeep for the calls the stream still carries (marshal.c). */
void glm_shadow_glAttachShader(struct glm_context *ctx, GLuint program, GLuint shader)
{
    struct app_objects *o = app_objects(ctx);
    pthread_mutex_lock(&o->lock);
    struct app_program *p = glm_table_get(&o->programs, program);
    if (p) {
        bool present = false;
        for (int i = 0; i < p->shader_count; ++i) present |= p->shaders[i] == shader;
        if (!present && p->shader_count < 8) p->shaders[p->shader_count++] = shader;
    }
    pthread_mutex_unlock(&o->lock);
}

void glm_shadow_glDetachShader(struct glm_context *ctx, GLuint program, GLuint shader)
{
    struct app_objects *o = app_objects(ctx);
    pthread_mutex_lock(&o->lock);
    struct app_program *p = glm_table_get(&o->programs, program);
    for (int i = 0; p && i < p->shader_count; ++i)
        if (p->shaders[i] == shader) {
            p->shaders[i] = p->shaders[--p->shader_count];
            break;
        }
    pthread_mutex_unlock(&o->lock);
}

void glm_shadow_glBindAttribLocation(struct glm_context *ctx, GLuint program, GLuint index, const GLchar *name)
{
    if (!name || index >= GLM_MAX_ATTRIBS || !strncmp(name, "gl_", 3)) return;
    struct app_objects *o = app_objects(ctx);
    pthread_mutex_lock(&o->lock);
    struct app_program *p = glm_table_get(&o->programs, program);
    if (p) app_set_binding(&p->attributes, &p->attribute_count, name, (int)index);
    pthread_mutex_unlock(&o->lock);
}

static void shadow_output_binding(struct glm_context *ctx, GLuint program, int location, const GLchar *name)
{
    if (!name) return;
    struct app_objects *o = app_objects(ctx);
    pthread_mutex_lock(&o->lock);
    struct app_program *p = glm_table_get(&o->programs, program);
    if (p) app_set_binding(&p->outputs, &p->output_count, name, location);
    pthread_mutex_unlock(&o->lock);
}
void glm_shadow_glBindFragDataLocation(struct glm_context *ctx, GLuint program, GLuint color, const GLchar *name)
{
    shadow_output_binding(ctx, program, (int)color, name);
}
void glm_shadow_glBindFragDataLocationEXT(struct glm_context *ctx, GLuint program, GLuint color, const GLchar *name)
{
    shadow_output_binding(ctx, program, (int)color, name);
}
void glm_shadow_glBindFragDataLocationIndexed(struct glm_context *ctx, GLuint program, GLuint color, GLuint index,
                                              const GLchar *name)
{
    if (index > 1 || (index == 1 && color > 0) || color >= GLM_MAX_DRAW_BUFFERS || (name && !strncmp(name, "gl_", 3)))
        return;
    shadow_output_binding(ctx, program, (int)color | (index ? GLM_OUTPUT_INDEX1 : 0), name);
}

void glm_shadow_glTransformFeedbackVaryings(struct glm_context *ctx, GLuint program, GLsizei count,
                                            const GLchar *const *varyings, GLenum mode)
{
    (void)count, (void)varyings, (void)mode;
    struct app_objects *o = app_objects(ctx);
    pthread_mutex_lock(&o->lock);
    struct app_program *p = glm_table_get(&o->programs, program);
    if (p) {
        p->untracked = true;
        glm_program_shadow_free(p->shadow);
        p->shadow = NULL;
    }
    pthread_mutex_unlock(&o->lock);
}

void glm_shadow_glDeleteShader(struct glm_context *ctx, GLuint name)
{
    /* The shader lives on while attached, but only links read it, and
       those were set up when it was attached: forget it here. */
    struct app_objects *o = app_objects(ctx);
    pthread_mutex_lock(&o->lock);
    app_free_shader(glm_table_get(&o->shaders, name));
    glm_table_set(&o->shaders, name, NULL);
    pthread_mutex_unlock(&o->lock);
}

void glm_shadow_glDeleteProgram(struct glm_context *ctx, GLuint name)
{
    struct app_objects *o = app_objects(ctx);
    pthread_mutex_lock(&o->lock);
    app_free_program(glm_table_get(&o->programs, name));
    glm_table_set(&o->programs, name, NULL);
    pthread_mutex_unlock(&o->lock);
}

struct link_cmd { GLuint name; };
static void link_exec(const void *payload)
{
    const struct link_cmd *c = payload;
    struct glm_context *ctx = glm_current();
    GLM_STATE_CHANGED(ctx);
    glm_impl_glLinkProgram(c->name);
    glm_thread_state_executed(ctx);
}

GLM_EXPORT void glLinkProgram(GLuint name)
{
    struct glm_context *ctx = glm_current();
    if (!ctx || !ctx->thread) {
        glm_impl_glLinkProgram(name);
        return;
    }
    struct app_objects *o = app_objects(ctx);
    /* The request, copied out of the registry: every attached shader
       known with a source, one per stage, vertex and fragment only. */
    struct glm_compile_request request = {0};
    char *sources[GLM_STAGE_COUNT] = {0};
    struct glm_name_location *attributes = NULL, *outputs = NULL;
    int attribute_count = 0, output_count = 0;
    pthread_mutex_lock(&o->lock);
    struct app_program *p = glm_table_get(&o->programs, name);
    bool trackable = p && !p->untracked;
    for (int i = 0; trackable && i < p->shader_count; ++i) {
        struct app_shader *s = glm_table_get(&o->shaders, p->shaders[i]);
        enum glm_stage stage = s ? app_stage(s->type) : GLM_STAGE_VERTEX;
        if (!s || !s->source || sources[stage] || (stage != GLM_STAGE_VERTEX && stage != GLM_STAGE_FRAGMENT)) {
            trackable = false;
            break;
        }
        sources[stage] = strdup(s->source);
    }
    if (trackable) {
        attribute_count = p->attribute_count;
        output_count = p->output_count;
        attributes = calloc((size_t)attribute_count + 1, sizeof *attributes);
        outputs = calloc((size_t)output_count + 1, sizeof *outputs);
        for (int i = 0; i < attribute_count; ++i)
            attributes[i] = (struct glm_name_location){strdup(p->attributes[i].name), p->attributes[i].location};
        for (int i = 0; i < output_count; ++i)
            outputs[i] = (struct glm_name_location){strdup(p->outputs[i].name), p->outputs[i].location};
    } else if (p) {
        glm_program_shadow_free(p->shadow);
        p->shadow = NULL;
    }
    pthread_mutex_unlock(&o->lock);
    if (trackable) {
        for (int s = 0; s < GLM_STAGE_COUNT; ++s) request.sources[s] = sources[s];
        request.attributes = attributes;
        request.attribute_count = attribute_count;
        request.frag_outputs = outputs;
        request.frag_output_count = output_count;
        request.feedback_interleaved = true;
        /* Linked here, before the stream's link is recorded: that one
           finds the result (and the Metal functions) cached. */
        struct glm_program *shadow = glm_program_shadow_link(&request);
        pthread_mutex_lock(&o->lock);
        p = glm_table_get(&o->programs, name);
        if (p) {
            glm_program_shadow_free(p->shadow);
            p->shadow = shadow;
            shadow = NULL;
        }
        pthread_mutex_unlock(&o->lock);
        glm_program_shadow_free(shadow);
        for (int s = 0; s < GLM_STAGE_COUNT; ++s) free(sources[s]);
        for (int i = 0; i < attribute_count; ++i) free((char *)attributes[i].name);
        for (int i = 0; i < output_count; ++i) free((char *)outputs[i].name);
        free(attributes);
        free(outputs);
    }
    glm_thread_state_recorded(ctx);
    struct link_cmd *c = glm_thread_alloc(ctx, sizeof *c, link_exec);
    c->name = name;
}

/* The shadow of `name` when this thread can answer for it (locked). */
static struct glm_program *app_shadow(struct app_objects *o, GLuint name)
{
    struct app_program *p = glm_table_get(&o->programs, name);
    return p && !p->untracked ? p->shadow : NULL;
}

GLM_EXPORT void glGetProgramiv(GLuint name, GLenum pname, GLint *params)
{
    struct glm_context *ctx = glm_current();
    if (ctx && ctx->thread && params) {
        bool answerable = pname == GL_LINK_STATUS || pname == GL_INFO_LOG_LENGTH || pname == GL_ACTIVE_UNIFORMS ||
                          pname == GL_ACTIVE_UNIFORM_MAX_LENGTH || pname == GL_ACTIVE_ATTRIBUTES ||
                          pname == GL_ACTIVE_ATTRIBUTE_MAX_LENGTH || pname == GL_ACTIVE_UNIFORM_BLOCKS;
        struct app_objects *o = app_objects(ctx);
        pthread_mutex_lock(&o->lock);
        struct glm_program *shadow = answerable ? app_shadow(o, name) : NULL;
        bool answered = shadow && glm_program_query_iv(shadow, pname, params);
        pthread_mutex_unlock(&o->lock);
        if (answered) return;
        glm_thread_sync_named(ctx, "glGetProgramiv");
    }
    glm_impl_glGetProgramiv(name, pname, params);
}

GLM_EXPORT void glGetProgramInfoLog(GLuint name, GLsizei size, GLsizei *length, GLchar *log)
{
    struct glm_context *ctx = glm_current();
    if (ctx && ctx->thread) {
        struct app_objects *o = app_objects(ctx);
        pthread_mutex_lock(&o->lock);
        struct glm_program *shadow = app_shadow(o, name);
        if (shadow) copy_out(shadow->log, size, length, log);
        pthread_mutex_unlock(&o->lock);
        if (shadow) return;
        glm_thread_sync_named(ctx, "glGetProgramInfoLog");
    }
    glm_impl_glGetProgramInfoLog(name, size, length, log);
}

GLM_EXPORT GLint glGetAttribLocation(GLuint name, const GLchar *attribute)
{
    struct glm_context *ctx = glm_current();
    if (ctx && ctx->thread && attribute) {
        struct app_objects *o = app_objects(ctx);
        pthread_mutex_lock(&o->lock);
        struct glm_program *shadow = app_shadow(o, name);
        bool answered = shadow && shadow->linked;
        GLint location = answered ? glm_program_attrib_location(shadow, attribute) : -1;
        pthread_mutex_unlock(&o->lock);
        if (answered) return location;
        glm_thread_sync_named(ctx, "glGetAttribLocation");
    }
    return glm_impl_glGetAttribLocation(name, attribute);
}

GLM_EXPORT GLint glGetUniformLocation(GLuint name, const GLchar *uniform)
{
    struct glm_context *ctx = glm_current();
    if (ctx && ctx->thread && uniform) {
        struct app_objects *o = app_objects(ctx);
        pthread_mutex_lock(&o->lock);
        struct glm_program *shadow = app_shadow(o, name);
        bool answered = shadow && shadow->linked;
        GLint location = answered ? glm_program_uniform_location(shadow, uniform) : -1;
        pthread_mutex_unlock(&o->lock);
        if (answered) return location;
        glm_thread_sync_named(ctx, "glGetUniformLocation");
    }
    return glm_impl_glGetUniformLocation(name, uniform);
}

/* ---- queries --------------------------------------------------------------- */

static int shadow_query_class(GLenum target)
{
    switch (target) {
    case GL_SAMPLES_PASSED: case GL_ANY_SAMPLES_PASSED: case 0x8D6A /* GL_ANY_SAMPLES_PASSED_CONSERVATIVE */: return 0;
    case GL_PRIMITIVES_GENERATED: return 1;
    case GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN: return 2;
    case GL_TIME_ELAPSED: return 3;
    default: return -1;
    }
}

void glm_shadow_glBeginQuery(struct glm_context *ctx, GLenum target, GLuint name)
{
    int c = shadow_query_class(target);
    if (c >= 0) shadow(ctx)->active_queries[c] = name;
}
void glm_shadow_glBeginQueryARB(struct glm_context *ctx, GLenum target, GLuint name)
{
    glm_shadow_glBeginQuery(ctx, target, name);
}

void glm_shadow_glEndQuery(struct glm_context *ctx, GLenum target)
{
    struct glm_shadow *s = shadow(ctx);
    int c = shadow_query_class(target);
    if (c < 0 || !s->active_queries[c]) return;
    GLuint name = s->active_queries[c];
    s->active_queries[c] = 0;
    if (name >= 1u << 20) return;
    if (name >= s->query_end_capacity) {
        size_t capacity = s->query_end_capacity ? s->query_end_capacity : 64;
        while (capacity <= name) capacity *= 2;
        struct shadow_query_end *ends = realloc(s->query_ends, capacity * sizeof *ends);
        if (!ends) return;
        memset(ends + s->query_end_capacity, 0, (capacity - s->query_end_capacity) * sizeof *ends);
        s->query_ends = ends;
        s->query_end_capacity = capacity;
    }
    /* The hook runs before the glEndQuery is recorded. */
    s->query_ends[name] = (struct shadow_query_end){ctx->thread, ctx->thread ? glm_thread_recorded(ctx) + 1 : 0};
}
void glm_shadow_glEndQueryARB(struct glm_context *ctx, GLenum target) { glm_shadow_glEndQuery(ctx, target); }

void glm_impl_glGetQueryObjectiv(GLuint name, GLenum pname, GLint *params);
void glm_impl_glGetQueryObjectuiv(GLuint name, GLenum pname, GLuint *params);

struct query_get { GLuint name; GLenum pname; void *params; bool is_unsigned; };
static void query_get_exec(const void *payload)
{
    const struct query_get *q = payload;
    if (q->is_unsigned) glm_impl_glGetQueryObjectuiv(q->name, q->pname, q->params);
    else glm_impl_glGetQueryObjectiv(q->name, q->pname, q->params);
}

/* Result queries wait for the stream only up to the query's glEndQuery
   (Source polls a dozen a frame): before the worker gets there the result
   is simply not available yet. */
static void query_object(GLuint name, GLenum pname, void *params, bool is_unsigned, const char *entry)
{
    struct glm_context *ctx = glm_current();
    struct query_get q = {name, pname, params, is_unsigned};
    if (!ctx || !GLM_STREAM(ctx)) return query_get_exec(&q);
    static int sync_queries = -1; /* GLMETAL_SYNC_QUERIES=1: drain the stream instead (comparisons) */
    if (sync_queries < 0) sync_queries = getenv("GLMETAL_SYNC_QUERIES") != NULL;
    if (sync_queries) {
        glm_thread_sync_named(ctx, entry);
        return query_get_exec(&q);
    }
    struct glm_shadow *s = shadow(ctx);
    struct shadow_query_end *end = name < s->query_end_capacity ? &s->query_ends[name] : NULL;
    bool active = false;
    for (int c = 0; c < 8; ++c) active |= s->active_queries[c] == name && name;
    if (!end || !end->mark || end->thread != ctx->thread || active || !params ||
        (pname != GL_QUERY_RESULT && pname != GL_QUERY_RESULT_AVAILABLE)) {
        glm_thread_sync_named(ctx, entry);
        return query_get_exec(&q);
    }
    if (pname == GL_QUERY_RESULT_AVAILABLE && glm_thread_executed(ctx) < end->mark) {
        glm_thread_submit(ctx); /* so a polling loop gets there */
        *(GLint *)params = GL_FALSE;
        return;
    }
    glm_thread_request(ctx, end->mark, query_get_exec, &q);
}

GLM_EXPORT void glGetQueryObjectiv(GLuint name, GLenum pname, GLint *params)
{
    query_object(name, pname, params, false, "glGetQueryObjectiv");
}
GLM_EXPORT void glGetQueryObjectivARB(GLuint name, GLenum pname, GLint *params)
{
    query_object(name, pname, params, false, "glGetQueryObjectivARB");
}
GLM_EXPORT void glGetQueryObjectuiv(GLuint name, GLenum pname, GLuint *params)
{
    query_object(name, pname, params, true, "glGetQueryObjectuiv");
}
GLM_EXPORT void glGetQueryObjectuivARB(GLuint name, GLenum pname, GLuint *params)
{
    query_object(name, pname, params, true, "glGetQueryObjectuivARB");
}
