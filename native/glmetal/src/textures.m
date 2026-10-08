/* Texture objects: GL level bookkeeping, Metal storage, uploads, sampling
 * state and binding for draws.
 *
 * Storage: one MTLTexture per GL texture, sized from the base level with a
 * full mip chain, created lazily and rebuilt (copying compatible levels)
 * when a level's size or format stops matching. Uploads go through a staging
 * buffer and a blit in the command stream, so draws already recorded keep
 * sampling the old contents. 1D textures are stored as 2D textures one
 * texel high, because Metal's 1D textures cannot have mipmaps. */
#import <Metal/Metal.h>
#import <objc/message.h>

#include "ffgen.h"
#define GLM_APPLE_TYPES_ONLY
#ifndef GL_TEXTURE_LUMINANCE_TYPE
#define GL_TEXTURE_LUMINANCE_TYPE 0x8C14
#define GL_TEXTURE_INTENSITY_TYPE 0x8C15
#endif
#include "glm_apple_gets.h"
#include "glm_internal.h"

GLM_HIDDEN id<MTLBuffer> glm_new_shared_buffer(NSUInteger length, const void *bytes);
#include "programs.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

GLM_HIDDEN void *glm_backend_command_buffer(struct glm_context *ctx);
GLM_HIDDEN size_t glm_metal_format_bytes(uint32_t metal_format);

static id<MTLDevice> metal_device(void) { return (__bridge id<MTLDevice>)glm_backend_device(); }

GLM_HIDDEN void glm_encoder_texture(struct glm_context *ctx, unsigned stages, void *texture, void *sampler, NSUInteger slot);

/* ---- lookup -------------------------------------------------------------- */

int glm_texture_slot(GLenum target)
{
    switch (target) {
    case GL_TEXTURE_1D: return GLM_TEX_1D;
    case GL_TEXTURE_2D: return GLM_TEX_2D;
    case GL_TEXTURE_3D: return GLM_TEX_3D;
    case GL_TEXTURE_CUBE_MAP: return GLM_TEX_CUBE;
    case GL_TEXTURE_RECTANGLE_ARB: return GLM_TEX_RECT;
    case GL_TEXTURE_2D_ARRAY: return GLM_TEX_2D_ARRAY;
    case GL_TEXTURE_BUFFER: return GLM_TEX_BUFFER;
    case GL_TEXTURE_2D_MULTISAMPLE: return GLM_TEX_2D_MULTISAMPLE;
    case GL_TEXTURE_2D_MULTISAMPLE_ARRAY: return GLM_TEX_2D_MULTISAMPLE_ARRAY;
    case GL_TEXTURE_1D_ARRAY: return GLM_TEX_1D_ARRAY;
    case 0x9009 /* GL_TEXTURE_CUBE_MAP_ARRAY */: return GLM_TEX_CUBE_ARRAY;
    default: return -1;
    }
}

/* Level sizes: array targets keep their layer count at every level
   (1D arrays in height, the others in depth). */
static bool layers_in_height(GLenum target) { return target == GL_TEXTURE_1D_ARRAY; }
static bool layers_in_depth(GLenum target)
{
    return target == GL_TEXTURE_2D_ARRAY || target == GL_TEXTURE_CUBE_MAP_ARRAY || target == GL_TEXTURE_2D_MULTISAMPLE_ARRAY;
}
static bool multisampled(GLenum target) { return target == GL_TEXTURE_2D_MULTISAMPLE || target == GL_TEXTURE_2D_MULTISAMPLE_ARRAY; }
/* Metal slices a level upload goes to one per layer. */
static bool layered(GLenum target) { return layers_in_depth(target) || layers_in_height(target); }

static int face_index(GLenum target)
{
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
        return (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
    return 0;
}

static GLenum binding_target(GLenum target)
{
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) return GL_TEXTURE_CUBE_MAP;
    return target;
}

struct glm_texture *glm_texture_get(struct glm_context *ctx, GLuint name)
{
    return glm_table_get(&ctx->share->textures, name);
}

static struct glm_texture *new_texture(GLuint name)
{
    struct glm_texture *t = calloc(1, sizeof *t);
    t->name = name;
    t->sampler = (struct glm_sampler_state){GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR, GL_REPEAT, GL_REPEAT, GL_REPEAT,
                                            GL_NONE, GL_LEQUAL, {0, 0, 0, 0}, -1000, 1000, 0, 1};
    t->max_level = 1000;
    t->depth_texture_mode = GL_LUMINANCE;
    t->priority = 1;
    for (int c = 0; c < 4; ++c) t->swizzle[c] = (uint8_t)c;
    return t;
}

/* Default textures (name 0) exist per target and per context. */
static struct glm_texture *default_texture(struct glm_context *ctx, int slot)
{
    if (!ctx->default_textures[slot]) ctx->default_textures[slot] = new_texture(0);
    return ctx->default_textures[slot];
}

/* Set by the command stream's texture queries: the texture the application
   has bound, which the worker may not have bound yet. */
static __thread struct glm_texture *bound_override;

GLM_HIDDEN void glm_texture_query_override(struct glm_context *ctx, GLenum target, GLuint name)
{
    int slot = glm_texture_slot(binding_target(target));
    if (slot < 0) {
        bound_override = NULL;
        return;
    }
    bound_override = name ? glm_texture_get(ctx, name) : default_texture(ctx, slot);
}

GLM_HIDDEN void glm_texture_query_override_end(void) { bound_override = NULL; }

struct glm_texture *glm_texture_bound(struct glm_context *ctx, GLenum target)
{
    if (bound_override && glm_texture_slot(binding_target(target)) >= 0) return bound_override;
    int slot = glm_texture_slot(binding_target(target));
    if (slot < 0) return NULL;
    GLuint name = ctx->state.units[ctx->state.active_texture - GL_TEXTURE0].bound[slot];
    return name ? glm_texture_get(ctx, name) : default_texture(ctx, slot);
}

/* ---- objects ------------------------------------------------------------- */

GLM_EXPORT void glGenTextures(GLsizei n, GLuint *names)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    pthread_mutex_lock(&ctx->share->lock);
    for (GLsizei i = 0; i < n; ++i) {
        GLuint name = glm_table_reserve(&ctx->share->textures);
        glm_table_set(&ctx->share->textures, name, new_texture(name));
        names[i] = name;
    }
    pthread_mutex_unlock(&ctx->share->lock);
}

GLM_EXPORT void glBindTexture(GLenum target, GLuint name)
{
    GLM_CONTEXT(ctx);
    int slot = glm_texture_slot(target);
    if (slot < 0) return glm_error(ctx, GL_INVALID_ENUM);
    if (name) {
        struct glm_texture *t = glm_texture_get(ctx, name);
        if (!t) {
            t = new_texture(name);
            pthread_mutex_lock(&ctx->share->lock);
            glm_table_set(&ctx->share->textures, name, t);
            pthread_mutex_unlock(&ctx->share->lock);
        }
        if (t->target && t->target != target) return glm_error(ctx, GL_INVALID_OPERATION);
        if (!t->target) {
            t->target = target;
            if (target == GL_TEXTURE_RECTANGLE_ARB) {
                t->sampler.min_filter = GL_LINEAR;
                t->sampler.wrap_s = t->sampler.wrap_t = t->sampler.wrap_r = GL_CLAMP_TO_EDGE;
            }
        }
    }
    ctx->state.units[ctx->state.active_texture - GL_TEXTURE0].bound[slot] = name;
}
GLM_EXPORT void glBindTextureEXT(GLenum target, GLuint name) { glBindTexture(target, name); }

GLM_HIDDEN void glm_texture_free_pending(struct glm_texture *t);
static void flush_pending(struct glm_context *ctx, struct glm_texture *t);

void glm_texture_destroy(struct glm_texture *t)
{
    if (!t) return;
    glm_texture_free_pending(t);
    glm_backend_release(t->cube_array_view);
    glm_backend_release(t->view);
    glm_backend_release(t->backend);
    free(t);
}

GLM_EXPORT void glDeleteTextures(GLsizei n, const GLuint *names)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; ++i) {
        struct glm_texture *t = glm_texture_get(ctx, names[i]);
        if (!t) continue;
        for (int u = 0; u < GLM_MAX_TEXTURE_UNITS; ++u)
            for (int s = 0; s < GLM_TEX_SLOTS; ++s)
                if (ctx->state.units[u].bound[s] == names[i]) ctx->state.units[u].bound[s] = 0;
        pthread_mutex_lock(&ctx->share->lock);
        glm_table_set(&ctx->share->textures, names[i], NULL);
        pthread_mutex_unlock(&ctx->share->lock);
        glm_detach_deleted(ctx, GL_TEXTURE, names[i]);
        glm_texture_destroy(t);
    }
}
GLM_EXPORT void glDeleteTexturesEXT(GLsizei n, const GLuint *names) { glDeleteTextures(n, names); }
GLM_EXPORT GLboolean glIsTexture(GLuint name) { GLM_CONTEXT(ctx, GL_FALSE); struct glm_texture *t = glm_texture_get(ctx, name); return t && t->target; }

/* ---- parameters ---------------------------------------------------------- */

/* Set once any texture or sampler uses a border colour Metal has no
   sampler border for, or GL_CLAMP: draws then check for border emulation. */
int glm_custom_borders;

static bool metal_border(const float *b)
{
    bool rgb0 = b[0] == 0 && b[1] == 0 && b[2] == 0, rgb1 = b[0] == 1 && b[1] == 1 && b[2] == 1;
    return (rgb0 && (b[3] == 0 || b[3] == 1)) || (rgb1 && b[3] == 1);
}

static void sampler_parameter(struct glm_context *ctx, struct glm_sampler_state *s, GLenum pname, const float *v)
{
    GLenum e = (GLenum)v[0];
    if ((pname == GL_TEXTURE_BORDER_COLOR && !metal_border(v)) ||
        ((pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T || pname == GL_TEXTURE_WRAP_R) && e == GL_CLAMP))
        __atomic_store_n(&glm_custom_borders, 1, __ATOMIC_RELAXED);
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: s->min_filter = e; break;
    case GL_TEXTURE_MAG_FILTER: s->mag_filter = e; break;
    case GL_TEXTURE_WRAP_S: s->wrap_s = e; break;
    case GL_TEXTURE_WRAP_T: s->wrap_t = e; break;
    case GL_TEXTURE_WRAP_R: s->wrap_r = e; break;
    case GL_TEXTURE_COMPARE_MODE: s->compare_mode = e; break;
    case GL_TEXTURE_COMPARE_FUNC: s->compare_func = e; break;
    case GL_TEXTURE_BORDER_COLOR: memcpy(s->border_color, v, 16); break;
    case GL_TEXTURE_MIN_LOD: s->min_lod = v[0]; break;
    case GL_TEXTURE_MAX_LOD: s->max_lod = v[0]; break;
    case GL_TEXTURE_LOD_BIAS: s->lod_bias = v[0]; break;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT: s->max_anisotropy = v[0]; break;
    case GL_TEXTURE_SRGB_DECODE_EXT:
        if (e != GL_DECODE_EXT && e != GL_SKIP_DECODE_EXT) return glm_error(ctx, GL_INVALID_ENUM);
        s->skip_srgb_decode = e == GL_SKIP_DECODE_EXT;
        break;
    default:
        glm_log("texture/sampler parameter 0x%04x not supported", pname);
        glm_error(ctx, GL_INVALID_ENUM);
    }
}

/* Texture parameters only one profile has (Apple's: build/probes/texparam.c):
   the fixed-function ones in the legacy profile, swizzles in the core one. */
bool glm_texture_pname_in_profile(const struct glm_context *ctx, GLenum pname)
{
    bool core = ctx->profile == GLM_PROFILE_CORE;
    switch (pname) {
    case GL_TEXTURE_PRIORITY: case GL_TEXTURE_RESIDENT: case GL_TEXTURE_COMPARE_FAIL_VALUE_ARB: case GL_GENERATE_MIPMAP:
    case GL_DEPTH_TEXTURE_MODE:
        return !core;
    case GL_TEXTURE_SWIZZLE_R: case GL_TEXTURE_SWIZZLE_G: case GL_TEXTURE_SWIZZLE_B: case GL_TEXTURE_SWIZZLE_A:
    case GL_TEXTURE_SWIZZLE_RGBA:
        return core;
    case 0x82DF: /* GL_TEXTURE_IMMUTABLE_LEVELS */
        return false;
    }
    return true;
}

/* Texture parameters exist for the targets that sample with them: not
   multisample or buffer textures (Apple's: GL_INVALID_ENUM). */
static struct glm_texture *parameter_texture(struct glm_context *ctx, GLenum target)
{
    if (target == GL_TEXTURE_2D_MULTISAMPLE || target == GL_TEXTURE_2D_MULTISAMPLE_ARRAY || target == GL_TEXTURE_BUFFER) return NULL;
    return glm_texture_bound(ctx, target);
}

static void texture_parameter(GLenum target, GLenum pname, const float *v)
{
    GLM_CONTEXT(ctx);
    struct glm_texture *t = parameter_texture(ctx, target);
    if (!t) {
        glm_log("glTexParameter: target 0x%04x (pname 0x%04x) not supported", target, pname);
        return glm_error(ctx, GL_INVALID_ENUM);
    }
    if (!glm_texture_pname_in_profile(ctx, pname)) return glm_error(ctx, GL_INVALID_ENUM);
    switch (pname) {
    case GL_TEXTURE_BASE_LEVEL: t->base_level = (GLint)v[0]; return;
    case GL_TEXTURE_MAX_LEVEL: t->max_level = (GLint)v[0]; return;
    case GL_GENERATE_MIPMAP: t->generate_mipmap = v[0] != 0; return;
    case GL_DEPTH_TEXTURE_MODE: t->depth_texture_mode = (GLenum)v[0]; return;
    case GL_TEXTURE_PRIORITY: t->priority = v[0] < 0 ? 0 : v[0] > 1 ? 1 : v[0]; return;
    case GL_TEXTURE_COMPARE_FAIL_VALUE_ARB: t->compare_fail_value = v[0]; return;
    case 0x8A4B: t->apple_private[0] = (GLint)v[0]; return;
    case 0x8A7F: t->apple_private[1] = (GLint)v[0]; return;
    /* Apple keeps both (Source's togl sets them on every texture). */
    case GL_TEXTURE_STORAGE_HINT_APPLE:
        if ((GLenum)v[0] != GL_STORAGE_PRIVATE_APPLE && (GLenum)v[0] != GL_STORAGE_CACHED_APPLE &&
            (GLenum)v[0] != GL_STORAGE_SHARED_APPLE)
            return glm_error(ctx, GL_INVALID_ENUM);
        t->storage_hint = (GLenum)v[0];
        return;
    case GL_TEXTURE_MINIMIZE_STORAGE_APPLE: t->minimize_storage = v[0] != 0; return;
    case GL_TEXTURE_SWIZZLE_R: case GL_TEXTURE_SWIZZLE_G: case GL_TEXTURE_SWIZZLE_B: case GL_TEXTURE_SWIZZLE_A:
    case GL_TEXTURE_SWIZZLE_RGBA: {
        int first = pname == GL_TEXTURE_SWIZZLE_RGBA ? 0 : (int)(pname - GL_TEXTURE_SWIZZLE_R);
        int count = pname == GL_TEXTURE_SWIZZLE_RGBA ? 4 : 1;
        uint8_t values[4];
        for (int i = 0; i < count; ++i) {
            /* Validate before converting to unsigned: negative float values can
             * otherwise saturate to GL_ZERO on ARM. Preserve the existing
             * float-to-enum conversion for values inside the legal enum range. */
            if (!(v[i] >= 0.0f && v[i] <= (float)GL_ALPHA))
                return glm_error(ctx, GL_INVALID_ENUM);
            switch ((GLenum)v[i]) {
            case GL_RED: values[i] = 0; break;
            case GL_GREEN: values[i] = 1; break;
            case GL_BLUE: values[i] = 2; break;
            case GL_ALPHA: values[i] = 3; break;
            case GL_ZERO: values[i] = 4; break;
            case GL_ONE: values[i] = 5; break;
            default: return glm_error(ctx, GL_INVALID_ENUM);
            }
        }
        memcpy(t->swizzle + first, values, (size_t)count);
        return;
    }
    default: sampler_parameter(ctx, &t->sampler, pname, v);
    }
}

GLM_EXPORT void glTexParameterf(GLenum target, GLenum pname, GLfloat param) { texture_parameter(target, pname, &param); }
GLM_EXPORT void glTexParameteri(GLenum target, GLenum pname, GLint param) { float f = (float)param; texture_parameter(target, pname, &f); }
GLM_EXPORT void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params) { texture_parameter(target, pname, params); }
GLM_EXPORT void glTexParameteriv(GLenum target, GLenum pname, const GLint *params)
{
    float f[4] = {(float)params[0], 0, 0, 0};
    if (pname == GL_TEXTURE_BORDER_COLOR)
        for (int i = 0; i < 4; ++i) f[i] = (float)((2.0 * params[i] + 1) / 4294967295.0);
    if (pname == GL_TEXTURE_SWIZZLE_RGBA)
        for (int i = 0; i < 4; ++i) f[i] = (float)params[i];
    texture_parameter(target, pname, f);
}

GLM_EXPORT void glGetTexParameteriv(GLenum target, GLenum pname, GLint *params)
{
    GLM_CONTEXT(ctx);
    struct glm_texture *t = parameter_texture(ctx, target);
    if (!t) return glm_error(ctx, GL_INVALID_ENUM);
    if (!glm_texture_pname_in_profile(ctx, pname)) return glm_error(ctx, GL_INVALID_ENUM);
    const struct glm_sampler_state *s = &t->sampler;
    switch (pname) {
    case GL_TEXTURE_BORDER_COLOR:
        for (int i = 0; i < 4; ++i) params[i] = (GLint)lround(fmax(-1.0, fmin(1.0, s->border_color[i])) * 2147483647.0);
        break;
    case GL_TEXTURE_MIN_LOD: *params = (GLint)lroundf(s->min_lod); break;
    case GL_TEXTURE_MAX_LOD: *params = (GLint)lroundf(s->max_lod); break;
    case GL_TEXTURE_LOD_BIAS: *params = (GLint)lroundf(s->lod_bias); break;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT: *params = (GLint)lroundf(s->max_anisotropy); break;
    case GL_TEXTURE_PRIORITY: *params = (GLint)lroundf(t->priority); break;
    case GL_TEXTURE_COMPARE_FAIL_VALUE_ARB: *params = (GLint)lroundf(t->compare_fail_value); break;
    case 0x85B7 /* GL_TEXTURE_RANGE_LENGTH_APPLE */: *params = 0; break;
    case 0x8A4B: *params = t->apple_private[0]; break;
    case 0x8A7F: *params = t->apple_private[1]; break;
    case GL_TEXTURE_MIN_FILTER: *params = (GLint)s->min_filter; break;
    case GL_TEXTURE_MAG_FILTER: *params = (GLint)s->mag_filter; break;
    case GL_TEXTURE_WRAP_S: *params = (GLint)s->wrap_s; break;
    case GL_TEXTURE_WRAP_T: *params = (GLint)s->wrap_t; break;
    case GL_TEXTURE_WRAP_R: *params = (GLint)s->wrap_r; break;
    case GL_TEXTURE_BASE_LEVEL: *params = t->base_level; break;
    case GL_TEXTURE_MAX_LEVEL: *params = t->max_level; break;
    case GL_TEXTURE_COMPARE_MODE: *params = (GLint)s->compare_mode; break;
    case GL_TEXTURE_COMPARE_FUNC: *params = (GLint)s->compare_func; break;
    case GL_GENERATE_MIPMAP: *params = t->generate_mipmap; break;
    case GL_DEPTH_TEXTURE_MODE: *params = (GLint)t->depth_texture_mode; break;
    case GL_TEXTURE_RESIDENT: *params = GL_FALSE; break; /* as Apple answers */
    case GL_TEXTURE_STORAGE_HINT_APPLE: *params = t->storage_hint ? (GLint)t->storage_hint : GL_STORAGE_PRIVATE_APPLE; break;
    case GL_TEXTURE_MINIMIZE_STORAGE_APPLE: *params = t->minimize_storage; break;
    case GL_TEXTURE_SWIZZLE_R: case GL_TEXTURE_SWIZZLE_G: case GL_TEXTURE_SWIZZLE_B: case GL_TEXTURE_SWIZZLE_A:
    case GL_TEXTURE_SWIZZLE_RGBA: {
        static const GLint names[] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA, GL_ZERO, GL_ONE};
        if (pname == GL_TEXTURE_SWIZZLE_RGBA)
            for (int i = 0; i < 4; ++i) params[i] = names[t->swizzle[i]];
        else
            *params = names[t->swizzle[pname - GL_TEXTURE_SWIZZLE_R]];
        break;
    }
    case GL_TEXTURE_SRGB_DECODE_EXT: *params = t->sampler.skip_srgb_decode ? GL_SKIP_DECODE_EXT : GL_DECODE_EXT; break;
    case GL_TEXTURE_IMMUTABLE_FORMAT: *params = t->immutable; break;
    default: glm_error(ctx, GL_INVALID_ENUM);
    }
}

GLM_EXPORT void glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params)
{
    GLM_CONTEXT(ctx);
    struct glm_texture *t = parameter_texture(ctx, target);
    if (!t) return glm_error(ctx, GL_INVALID_ENUM);
    switch (pname) {
    case GL_TEXTURE_BORDER_COLOR: memcpy(params, t->sampler.border_color, 16); return;
    case GL_TEXTURE_MIN_LOD: *params = t->sampler.min_lod; return;
    case GL_TEXTURE_MAX_LOD: *params = t->sampler.max_lod; return;
    case GL_TEXTURE_LOD_BIAS: *params = t->sampler.lod_bias; return;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT: *params = t->sampler.max_anisotropy; return;
    case GL_TEXTURE_PRIORITY: if (ctx->profile == GLM_PROFILE_CORE) break; *params = t->priority; return;
    case GL_TEXTURE_COMPARE_FAIL_VALUE_ARB: if (ctx->profile == GLM_PROFILE_CORE) break; *params = t->compare_fail_value; return;
    }
    GLint i[4];
    glGetTexParameteriv(target, pname, i);
    for (int k = 0; k < (pname == GL_TEXTURE_SWIZZLE_RGBA ? 4 : 1); ++k) params[k] = (GLfloat)i[k];
}

/* ---- proxy targets -------------------------------------------------------- */

static int proxy_index(GLenum target)
{
    switch (target) {
    case GL_PROXY_TEXTURE_1D: return 0;
    case GL_PROXY_TEXTURE_2D: return 1;
    case GL_PROXY_TEXTURE_3D: return 2;
    case GL_PROXY_TEXTURE_CUBE_MAP: return 3;
    case GL_PROXY_TEXTURE_RECTANGLE: return 4;
    case GL_PROXY_TEXTURE_1D_ARRAY: return 5;
    case GL_PROXY_TEXTURE_2D_ARRAY: return 6;
    case 0x900B /* GL_PROXY_TEXTURE_CUBE_MAP_ARRAY */: return 7;
    default: return -1;
    }
}

GLM_EXPORT void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params)
{
    GLM_CONTEXT(ctx);
    int proxy = proxy_index(target);
    if (proxy >= 0) {
        if (level < 0 || level >= GLM_MAX_LEVELS) return glm_error(ctx, GL_INVALID_VALUE);
        const struct glm_texture_level *p = &ctx->proxy_levels[proxy][level];
        const struct glm_apple_texture *ref = p->defined ? glm_apple_texture_format(ctx, p->internal_format) : NULL;
        switch (pname) {
        case GL_TEXTURE_WIDTH: *params = p->width; return;
        case GL_TEXTURE_HEIGHT: *params = p->height; return;
        case GL_TEXTURE_DEPTH: *params = p->depth; return;
        case GL_TEXTURE_INTERNAL_FORMAT: *params = ref ? ref->reported : (GLint)p->internal_format; return;
        case GL_TEXTURE_RED_SIZE: *params = ref ? ref->size[0] : 0; return;
        case GL_TEXTURE_GREEN_SIZE: *params = ref ? ref->size[1] : 0; return;
        case GL_TEXTURE_BLUE_SIZE: *params = ref ? ref->size[2] : 0; return;
        case GL_TEXTURE_ALPHA_SIZE: *params = ref ? ref->size[3] : 0; return;
        case GL_TEXTURE_DEPTH_SIZE: *params = ref ? ref->size[6] : 0; return;
        default: *params = 0; return;
        }
    }
    struct glm_texture *t = glm_texture_bound(ctx, target);
    if (!t || level < 0 || level >= GLM_MAX_LEVELS) return glm_error(ctx, GL_INVALID_VALUE);
    const struct glm_texture_level *l = &t->levels[face_index(target)][level];
    const struct glm_apple_texture *ref = l->defined ? glm_apple_texture_format(ctx, l->internal_format) : NULL;
    bool core = ctx->profile == GLM_PROFILE_CORE;
    switch (pname) {
    case GL_TEXTURE_LUMINANCE_SIZE: case GL_TEXTURE_INTENSITY_SIZE: case GL_TEXTURE_LUMINANCE_TYPE:
    case GL_TEXTURE_INTENSITY_TYPE:
        if (core) return glm_error(ctx, GL_INVALID_ENUM);
        break;
    }
    /* Sizes and types as Apple reports them for the level's format. */
    if (ref) {
        switch (pname) {
        case GL_TEXTURE_INTERNAL_FORMAT: *params = ref->reported; return;
        case GL_TEXTURE_RED_SIZE: *params = ref->size[0]; return;
        case GL_TEXTURE_GREEN_SIZE: *params = ref->size[1]; return;
        case GL_TEXTURE_BLUE_SIZE: *params = ref->size[2]; return;
        case GL_TEXTURE_ALPHA_SIZE: *params = ref->size[3]; return;
        case GL_TEXTURE_LUMINANCE_SIZE: *params = ref->size[4]; return;
        case GL_TEXTURE_INTENSITY_SIZE: *params = ref->size[5]; return;
        case GL_TEXTURE_DEPTH_SIZE: *params = ref->size[6]; return;
        case GL_TEXTURE_STENCIL_SIZE: *params = ref->size[7]; return;
        case GL_TEXTURE_SHARED_SIZE: *params = ref->size[8]; return;
        case GL_TEXTURE_RED_TYPE: *params = ref->type[0]; return;
        case GL_TEXTURE_GREEN_TYPE: *params = ref->type[1]; return;
        case GL_TEXTURE_BLUE_TYPE: *params = ref->type[2]; return;
        case GL_TEXTURE_ALPHA_TYPE: *params = ref->type[3]; return;
        case GL_TEXTURE_LUMINANCE_TYPE: *params = ref->type[4]; return;
        case GL_TEXTURE_INTENSITY_TYPE: *params = ref->type[5]; return;
        case GL_TEXTURE_DEPTH_TYPE: *params = ref->type[6]; return;
        case GL_TEXTURE_COMPRESSED: *params = ref->compressed; return;
        }
    }
    switch (pname) {
    case GL_TEXTURE_WIDTH: *params = l->defined ? l->width : 0; break;
    case GL_TEXTURE_HEIGHT: *params = l->defined ? l->height : 0; break;
    case GL_TEXTURE_DEPTH: *params = l->defined ? l->depth : 0; break;
    case GL_TEXTURE_INTERNAL_FORMAT: *params = l->defined ? (GLint)l->internal_format : 1; break;
    case GL_TEXTURE_COMPRESSED: {
        struct glm_format_info info;
        *params = l->defined && glm_format_lookup(l->internal_format, &info) && info.compressed;
        break;
    }
    case GL_TEXTURE_COMPRESSED_IMAGE_SIZE: {
        struct glm_format_info info;
        if (!l->defined || !glm_format_lookup(l->internal_format, &info) || !info.compressed) return glm_error(ctx, GL_INVALID_OPERATION);
        *params = ((l->width + 3) / 4) * ((l->height + 3) / 4) * info.bytes_per_pixel;
        break;
    }
    case GL_TEXTURE_BORDER: *params = 0; break;
    case GL_TEXTURE_RED_SIZE: case GL_TEXTURE_GREEN_SIZE: case GL_TEXTURE_BLUE_SIZE: case GL_TEXTURE_ALPHA_SIZE:
        *params = l->defined ? 8 : 0; break;
    case GL_TEXTURE_DEPTH_SIZE: *params = 0; break;
    case GL_TEXTURE_SAMPLES: *params = multisampled(t->target) && l->defined ? t->samples : 0; break;
    case GL_TEXTURE_FIXED_SAMPLE_LOCATIONS: *params = !multisampled(t->target) || t->fixed_sample_locations; break;
    default: *params = 0; break;
    }
}

GLM_EXPORT void glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat *params)
{
    GLint i = 0;
    glGetTexLevelParameteriv(target, level, pname, &i);
    *params = (GLfloat)i;
}

/* ---- sampler objects (GL 3.3) ------------------------------------------ */

struct glm_sampler_object {
    GLuint name;
    struct glm_sampler_state state;
};

static struct glm_sampler_object *sampler_object(struct glm_context *ctx, GLuint name)
{
    return name ? glm_table_get(&ctx->share->samplers, name) : NULL;
}

GLM_EXPORT void glGenSamplers(GLsizei n, GLuint *names)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    pthread_mutex_lock(&ctx->share->lock);
    for (GLsizei i = 0; i < n; ++i) {
        struct glm_sampler_object *s = calloc(1, sizeof *s);
        s->name = names[i] = glm_table_reserve(&ctx->share->samplers);
        s->state = (struct glm_sampler_state){GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR, GL_REPEAT, GL_REPEAT, GL_REPEAT, GL_NONE,
                                              GL_LEQUAL, {0}, -1000, 1000, 0, 1};
        glm_table_set(&ctx->share->samplers, s->name, s);
    }
    pthread_mutex_unlock(&ctx->share->lock);
}

GLM_EXPORT void glDeleteSamplers(GLsizei n, const GLuint *names)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; ++i) {
        struct glm_sampler_object *s = sampler_object(ctx, names[i]);
        if (!s) continue;
        for (int u = 0; u < GLM_MAX_TEXTURE_UNITS; ++u)
            if (ctx->state.units[u].sampler == names[i]) ctx->state.units[u].sampler = 0;
        pthread_mutex_lock(&ctx->share->lock);
        glm_table_set(&ctx->share->samplers, names[i], NULL);
        pthread_mutex_unlock(&ctx->share->lock);
        free(s);
    }
}

GLM_EXPORT GLboolean glIsSampler(GLuint name) { GLM_CONTEXT(ctx, GL_FALSE); return sampler_object(ctx, name) != NULL; }

GLM_EXPORT void glBindSampler(GLuint unit, GLuint name)
{
    GLM_CONTEXT(ctx);
    if (unit >= GLM_MAX_TEXTURE_UNITS) return glm_error(ctx, GL_INVALID_VALUE);
    if (name && !sampler_object(ctx, name)) return glm_error(ctx, GL_INVALID_OPERATION);
    ctx->state.units[unit].sampler = name;
}

static void sampler_object_parameter(GLuint name, GLenum pname, const float *v)
{
    GLM_CONTEXT(ctx);
    struct glm_sampler_object *s = sampler_object(ctx, name);
    if (!s) return glm_error(ctx, GL_INVALID_OPERATION);
    sampler_parameter(ctx, &s->state, pname, v);
}

GLM_EXPORT void glSamplerParameterf(GLuint s, GLenum pname, GLfloat param) { sampler_object_parameter(s, pname, &param); }
GLM_EXPORT void glSamplerParameteri(GLuint s, GLenum pname, GLint param) { float f = (float)param; sampler_object_parameter(s, pname, &f); }
GLM_EXPORT void glSamplerParameterfv(GLuint s, GLenum pname, const GLfloat *params) { sampler_object_parameter(s, pname, params); }
GLM_EXPORT void glSamplerParameteriv(GLuint s, GLenum pname, const GLint *params)
{
    float f[4] = {(float)params[0], 0, 0, 0};
    if (pname == GL_TEXTURE_BORDER_COLOR)
        for (int i = 0; i < 4; ++i) f[i] = (float)((2.0 * params[i] + 1) / 4294967295.0);
    sampler_object_parameter(s, pname, f);
}
GLM_EXPORT void glSamplerParameterIiv(GLuint s, GLenum pname, const GLint *params)
{
    float f[4] = {(float)params[0], (float)params[1], (float)params[2], (float)params[3]};
    sampler_object_parameter(s, pname, f);
}
GLM_EXPORT void glSamplerParameterIuiv(GLuint s, GLenum pname, const GLuint *params)
{
    float f[4] = {(float)params[0], (float)params[1], (float)params[2], (float)params[3]};
    sampler_object_parameter(s, pname, f);
}

GLM_EXPORT void glGetSamplerParameterfv(GLuint name, GLenum pname, GLfloat *params)
{
    GLM_CONTEXT(ctx);
    struct glm_sampler_object *o = sampler_object(ctx, name);
    if (!o) return glm_error(ctx, GL_INVALID_OPERATION);
    const struct glm_sampler_state *s = &o->state;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: *params = (float)s->min_filter; break;
    case GL_TEXTURE_MAG_FILTER: *params = (float)s->mag_filter; break;
    case GL_TEXTURE_WRAP_S: *params = (float)s->wrap_s; break;
    case GL_TEXTURE_WRAP_T: *params = (float)s->wrap_t; break;
    case GL_TEXTURE_WRAP_R: *params = (float)s->wrap_r; break;
    case GL_TEXTURE_COMPARE_MODE: *params = (float)s->compare_mode; break;
    case GL_TEXTURE_COMPARE_FUNC: *params = (float)s->compare_func; break;
    case GL_TEXTURE_BORDER_COLOR: memcpy(params, s->border_color, 16); break;
    case GL_TEXTURE_MIN_LOD: *params = s->min_lod; break;
    case GL_TEXTURE_MAX_LOD: *params = s->max_lod; break;
    case GL_TEXTURE_LOD_BIAS: *params = s->lod_bias; break;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT: *params = s->max_anisotropy; break;
    case GL_TEXTURE_SRGB_DECODE_EXT: *params = (float)(s->skip_srgb_decode ? GL_SKIP_DECODE_EXT : GL_DECODE_EXT); break;
    default: glm_error(ctx, GL_INVALID_ENUM);
    }
}
GLM_EXPORT void glGetSamplerParameteriv(GLuint name, GLenum pname, GLint *params)
{
    GLfloat f[4] = {0};
    glGetSamplerParameterfv(name, pname, f);
    int n = pname == GL_TEXTURE_BORDER_COLOR ? 4 : 1;
    for (int i = 0; i < n; ++i)
        params[i] = pname == GL_TEXTURE_BORDER_COLOR ? (GLint)(f[i] * 2147483647.0) : (GLint)f[i];
}
GLM_EXPORT void glGetSamplerParameterIiv(GLuint name, GLenum pname, GLint *params)
{
    GLfloat f[4] = {0};
    glGetSamplerParameterfv(name, pname, f);
    for (int i = 0; i < (pname == GL_TEXTURE_BORDER_COLOR ? 4 : 1); ++i) params[i] = (GLint)f[i];
}
GLM_EXPORT void glGetSamplerParameterIuiv(GLuint name, GLenum pname, GLuint *params)
{
    GLfloat f[4] = {0};
    glGetSamplerParameterfv(name, pname, f);
    for (int i = 0; i < (pname == GL_TEXTURE_BORDER_COLOR ? 4 : 1); ++i) params[i] = (GLuint)f[i];
}

/* The sampling state a unit uses: its sampler object, else the texture's. */
static const struct glm_sampler_state *unit_sampler_state(struct glm_context *ctx, int unit, const struct glm_texture *t)
{
    struct glm_sampler_object *o = sampler_object(ctx, ctx->state.units[unit].sampler);
    if (o) return &o->state;
    return t ? &t->sampler : NULL;
}

/* ---- storage ------------------------------------------------------------- */

static int face_count(struct glm_texture *t) { return t->target == GL_TEXTURE_CUBE_MAP ? 6 : 1; }



static int mip_count(GLsizei w, GLsizei h, GLsizei d)
{
    GLsizei m = MAX(MAX(w, h), d);
    int n = 1;
    while (m > 1) { m >>= 1; ++n; }
    return n;
}

static MTLTextureSwizzleChannels swizzle_for(const struct glm_format_info *info)
{
    static const MTLTextureSwizzle map[] = {MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue,
                                            MTLTextureSwizzleAlpha, MTLTextureSwizzleZero, MTLTextureSwizzleOne};
    return MTLTextureSwizzleChannelsMake(map[info->swizzle[0]], map[info->swizzle[1]], map[info->swizzle[2]], map[info->swizzle[3]]);
}

/* Creates storage matching the base level. Existing compatible levels are
   copied across. Returns false when the base level is not defined. */
bool glm_backend_texture_ensure_storage(struct glm_context *ctx, struct glm_texture *t)
{
    int base = t->base_level >= 0 && t->base_level < GLM_MAX_LEVELS ? t->base_level : 0;
    struct glm_texture_level *b = &t->levels[0][base];
    if (!b->defined) {
        /* Uploads may define level 0 while base_level is elsewhere. */
        b = &t->levels[0][0];
        base = 0;
        if (!b->defined) return false;
    }
    struct glm_format_info info;
    if (!glm_format_lookup(b->internal_format, &info)) return false;
    GLenum target = t->target;
    GLsizei w0 = b->width << base, h0 = layers_in_height(target) ? 1 : b->height << base;
    GLsizei d0 = target == GL_TEXTURE_3D ? b->depth << base : 1;
    GLsizei layers = layers_in_height(target) ? b->height : layers_in_depth(target) ? b->depth : 1;
    if (target == GL_TEXTURE_CUBE_MAP_ARRAY) layers /= 6;
    NSUInteger want_levels = target == GL_TEXTURE_RECTANGLE_ARB || multisampled(target) ? 1
                             : (NSUInteger)mip_count(w0, h0, target == GL_TEXTURE_3D ? d0 : 1);
    MTLTextureType type;
    switch (target) {
    case GL_TEXTURE_3D: type = MTLTextureType3D; break;
    case GL_TEXTURE_CUBE_MAP: type = MTLTextureTypeCube; break;
    case GL_TEXTURE_CUBE_MAP_ARRAY: type = MTLTextureTypeCubeArray; break;
    case GL_TEXTURE_2D_ARRAY: case GL_TEXTURE_1D_ARRAY: type = MTLTextureType2DArray; break;
    case GL_TEXTURE_2D_MULTISAMPLE: type = MTLTextureType2DMultisample; break;
    case GL_TEXTURE_2D_MULTISAMPLE_ARRAY: type = MTLTextureType2DMultisampleArray; break;
    default: type = MTLTextureType2D; break;
    }
    NSUInteger samples = multisampled(target) ? (NSUInteger)MAX(t->samples, 1) : 1;
    id<MTLTexture> current = (__bridge id<MTLTexture>)t->backend;
    if (current && !t->storage_dirty && current.width == (NSUInteger)MAX(w0, 1) && current.height == (NSUInteger)MAX(h0, 1) &&
        current.pixelFormat == (MTLPixelFormat)info.metal_format && current.textureType == type &&
        current.arrayLength == (NSUInteger)MAX(layers, 1) && current.sampleCount == samples &&
        (type != MTLTextureType3D || current.depth == (NSUInteger)MAX(d0, 1)))
        return true;
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.pixelFormat = (MTLPixelFormat)info.metal_format;
    d.width = (NSUInteger)MAX(w0, 1);
    d.height = (NSUInteger)MAX(h0, 1);
    d.mipmapLevelCount = want_levels;
    d.textureType = type;
    if (type == MTLTextureType3D) d.depth = (NSUInteger)MAX(d0, 1);
    if (type == MTLTextureType2DArray || type == MTLTextureTypeCubeArray || type == MTLTextureType2DMultisampleArray)
        d.arrayLength = (NSUInteger)MAX(layers, 1);
    d.sampleCount = samples;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    if (!info.compressed) d.usage |= MTLTextureUsageRenderTarget;
    id<MTLTexture> storage = [metal_device() newTextureWithDescriptor:d];
    if (!storage) return false;
    if (current && current.pixelFormat == storage.pixelFormat && current.textureType == storage.textureType &&
        current.arrayLength == storage.arrayLength && current.sampleCount == 1) {
        /* Carry over levels whose sizes still match. */
        id<MTLBlitCommandEncoder> blit = [(__bridge id<MTLCommandBuffer>)glm_backend_command_buffer(ctx) blitCommandEncoder];
        for (NSUInteger level = 0; level < MIN(current.mipmapLevelCount, storage.mipmapLevelCount); ++level) {
            NSUInteger cw = MAX(current.width >> level, 1u), ch = MAX(current.height >> level, 1u);
            NSUInteger sw = MAX(storage.width >> level, 1u), sh = MAX(storage.height >> level, 1u);
            if (cw != sw || ch != sh || (type == MTLTextureType3D &&
                MAX(current.depth >> level, 1u) != MAX(storage.depth >> level, 1u))) continue;
            NSUInteger slices = current.textureType == MTLTextureTypeCube || current.textureType == MTLTextureTypeCubeArray
                                    ? 6 * current.arrayLength : current.arrayLength;
            for (NSUInteger slice = 0; slice < slices; ++slice)
                [blit copyFromTexture:current sourceSlice:slice sourceLevel:level toTexture:storage destinationSlice:slice
                     destinationLevel:level sliceCount:1 levelCount:1];
        }
        [blit endEncoding];
    }
    glm_backend_release(t->backend);
    t->backend = (__bridge_retained void *)storage;
    t->metal_format = info.metal_format;
    t->storage_dirty = false;
    ++t->storage_generation;
    glm_backend_framebuffer_changed(ctx);
    if (t->pending) flush_pending(ctx, t);
    return true;
}

/* Blits already converted texel data into a level region. */
static void drop_pending(struct glm_texture *t, int face, GLint level)
{
    for (struct glm_pending_level **p = &t->pending; *p;) {
        if ((*p)->face == face && (*p)->level == level) {
            struct glm_pending_level *gone = *p;
            *p = gone->next;
            free(gone->data);
            free(gone);
        } else {
            p = &(*p)->next;
        }
    }
}

GLM_HIDDEN void glm_texture_free_pending(struct glm_texture *t)
{
    while (t->pending) drop_pending(t, t->pending->face, t->pending->level);
}

/* Whether level `level` of the storage has the level's GL size. */
static bool storage_fits(struct glm_texture *t, int face, GLint level)
{
    id<MTLTexture> storage = (__bridge id<MTLTexture>)t->backend;
    const struct glm_texture_level *l = &t->levels[face][level];
    if (!storage || (NSUInteger)level >= storage.mipmapLevelCount) return false;
    if (MAX(storage.width >> level, 1u) != (NSUInteger)MAX(l->width, 1)) return false;
    if (t->target != GL_TEXTURE_1D && !layers_in_height(t->target) && MAX(storage.height >> level, 1u) != (NSUInteger)MAX(l->height, 1))
        return false;
    return true;
}

/* Uploads the pending images the (new) storage now holds. */
static void flush_pending(struct glm_context *ctx, struct glm_texture *t)
{
    for (struct glm_pending_level *p = t->pending, *next; p; p = next) {
        next = p->next;
        if (!storage_fits(t, p->face, p->level)) continue;
        struct glm_pending_level copy = *p;
        p->data = NULL;
        drop_pending(t, copy.face, copy.level);
        glm_backend_texture_image(ctx, t, copy.face, copy.level, 0, 0, 0, copy.width, copy.height, copy.depth, copy.data,
                                  copy.row_bytes, copy.image_bytes);
        free(copy.data);
        next = t->pending; /* the list changed */
    }
}

void glm_backend_texture_image(struct glm_context *ctx, struct glm_texture *t, int face, GLint level, GLint x, GLint y,
                               GLint z, GLsizei width, GLsizei height, GLsizei depth, const void *data, size_t row_bytes,
                               size_t image_bytes)
{
    if (!data) return;
    bool storage = glm_backend_texture_ensure_storage(ctx, t);
    if (level >= 0 && level < GLM_MAX_LEVELS && (!storage || !storage_fits(t, face, level))) {
        /* A whole level the storage cannot hold yet: kept for later. */
        const struct glm_texture_level *l = &t->levels[face][level];
        if (x || y || z || width != l->width || height != MAX(l->height, 1) || MAX(depth, 1) != MAX(l->depth, 1)) return;
        drop_pending(t, face, level);
        struct glm_pending_level *p = calloc(1, sizeof *p);
        size_t total = image_bytes * (size_t)MAX(depth, 1);
        p->data = malloc(total ? total : 1);
        if (!p->data) { free(p); return; }
        memcpy(p->data, data, total);
        *p = (struct glm_pending_level){t->pending, face, level, width, height, depth, row_bytes, image_bytes, p->data};
        t->pending = p;
        return;
    }
    if (level >= 0 && level < GLM_MAX_LEVELS && !x && !y && !z) drop_pending(t, face, level);
    id<MTLTexture> storage_texture = (__bridge id<MTLTexture>)t->backend;
#define storage storage_texture
    if ((NSUInteger)level >= storage.mipmapLevelCount) return;
    size_t total = image_bytes * (size_t)MAX(depth, 1);
    if (storage.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
        /* Metal copies the planes of a combined format separately: depth
           (a float) and stencil (a byte) per texel, rows 256-byte aligned. */
        NSUInteger w = (NSUInteger)width, rows = (NSUInteger)MAX(height, 1) * (NSUInteger)MAX(depth, 1);
        NSUInteger depth_row = (w * 4 + 255) & ~(NSUInteger)255, stencil_row = (w + 255) & ~(NSUInteger)255;
        id<MTLBuffer> planes[2] = {glm_new_shared_buffer(depth_row * rows, NULL), glm_new_shared_buffer(stencil_row * rows, NULL)};
        uint8_t *d = [planes[0] contents], *s = [planes[1] contents];
        for (NSUInteger r = 0; r < rows; ++r) {
            const uint8_t *src = (const uint8_t *)data + (r / (NSUInteger)MAX(height, 1)) * image_bytes +
                                 (r % (NSUInteger)MAX(height, 1)) * row_bytes;
            for (NSUInteger x = 0; x < w; ++x) {
                memcpy(d + r * depth_row + x * 4, src + x * 8, 4);
                s[r * stencil_row + x] = src[x * 8 + 4];
            }
        }
        id<MTLBlitCommandEncoder> blit = [(__bridge id<MTLCommandBuffer>)glm_backend_command_buffer(ctx) blitCommandEncoder];
        bool images = layers_in_depth(t->target);
        NSUInteger slices = images ? (NSUInteger)MAX(depth, 1) : 1, h = (NSUInteger)MAX(height, 1);
        for (int plane = 0; plane < 2; ++plane) {
            NSUInteger row = plane ? stencil_row : depth_row;
            for (NSUInteger slice = 0; slice < slices; ++slice)
                [blit copyFromBuffer:planes[plane] sourceOffset:slice * row * h sourceBytesPerRow:row sourceBytesPerImage:row * h
                          sourceSize:MTLSizeMake(w, h, 1) toTexture:storage
                    destinationSlice:(images ? (NSUInteger)z + slice : (NSUInteger)face) destinationLevel:(NSUInteger)level
                   destinationOrigin:MTLOriginMake((NSUInteger)x, (NSUInteger)y, 0)
                             options:plane ? MTLBlitOptionStencilFromDepthStencil : MTLBlitOptionDepthFromDepthStencil];
        }
        [blit endEncoding];
        glm_backend_framebuffer_changed(ctx);
        glm_backend_upload_encoded(ctx);
        return;
    }
    id<MTLBuffer> staging = glm_new_shared_buffer(total, data);
    id<MTLBlitCommandEncoder> blit = [(__bridge id<MTLCommandBuffer>)glm_backend_command_buffer(ctx) blitCommandEncoder];
    /* Array layers are Metal slices: 1D arrays one row each, the others
       one image each. */
    bool rows = layers_in_height(t->target), images = layers_in_depth(t->target);
    NSUInteger slices = rows ? (NSUInteger)MAX(height, 1) : images ? (NSUInteger)MAX(depth, 1) : 1;
    NSUInteger first = rows ? (NSUInteger)y : images ? (NSUInteger)z : (NSUInteger)face;
    for (NSUInteger slice = 0; slice < slices; ++slice) {
        MTLSize size = MTLSizeMake((NSUInteger)width, rows ? 1 : (NSUInteger)MAX(height, 1),
                                   rows || images ? 1 : (NSUInteger)MAX(depth, 1));
        [blit copyFromBuffer:staging sourceOffset:slice * (rows ? row_bytes : image_bytes) sourceBytesPerRow:row_bytes
            sourceBytesPerImage:rows ? row_bytes : image_bytes sourceSize:size toTexture:storage
               destinationSlice:first + (rows || images ? slice : 0) destinationLevel:(NSUInteger)level
              destinationOrigin:MTLOriginMake((NSUInteger)x, rows ? 0 : (NSUInteger)y, rows || images ? 0 : (NSUInteger)z)];
    }
    [blit endEncoding];
#undef storage
    glm_backend_framebuffer_changed(ctx);
    glm_backend_upload_encoded(ctx);
}

GLM_HIDDEN void *glm_compress_rgba(uint32_t f, bool punch, const float *rgba, GLsizei width, GLsizei height,
                                   size_t *row_bytes, size_t *image_bytes);

/* Compressed formats: Metal cannot filter into them (its blit mipmapping
   aborts in the driver), so levels are box-filtered here from the base
   level and compressed again. */
static void generate_compressed_mipmaps(struct glm_context *ctx, struct glm_texture *t, id<MTLTexture> storage)
{
    if (storage.textureType != MTLTextureType2D && storage.textureType != MTLTextureTypeCube) return;
    struct glm_state *s = &ctx->state;
    GLint pack[6] = {s->pack_alignment, s->pack_row_length, s->pack_skip_rows, s->pack_skip_pixels, s->pack_row_bytes, 0};
    GLuint pack_buffer = ctx->pixel_pack_buffer;
    bool pack_swap = s->pack_swap_bytes;
    s->pack_alignment = 4;
    s->pack_swap_bytes = false;
    s->pack_row_length = s->pack_skip_rows = s->pack_skip_pixels = s->pack_row_bytes = 0;
    ctx->pixel_pack_buffer = 0;
    int base = MAX(t->base_level, 0);
    for (int f = 0; f < face_count(t); ++f) {
        GLsizei w = t->levels[f][base].width, h = MAX(t->levels[f][base].height, 1);
        float *level = calloc((size_t)w * (size_t)h * 4, sizeof(float));
        if (!level) break;
        glm_backend_get_tex_image(ctx, t, f, base, GL_RGBA, GL_FLOAT, level, false);
        for (int l = base + 1; l < (int)storage.mipmapLevelCount && l < GLM_MAX_LEVELS; ++l) {
            GLsizei nw = MAX(w / 2, 1), nh = MAX(h / 2, 1);
            float *next = calloc((size_t)nw * (size_t)nh * 4, sizeof(float));
            for (GLsizei y = 0; y < nh; ++y)
                for (GLsizei x = 0; x < nw; ++x)
                    for (int c = 0; c < 4; ++c) {
                        GLsizei x0 = MIN(x * 2, w - 1), x1 = MIN(x * 2 + 1, w - 1), y0 = MIN(y * 2, h - 1), y1 = MIN(y * 2 + 1, h - 1);
                        next[((size_t)y * nw + x) * 4 + c] =
                            (level[((size_t)y0 * w + x0) * 4 + c] + level[((size_t)y0 * w + x1) * 4 + c] +
                             level[((size_t)y1 * w + x0) * 4 + c] + level[((size_t)y1 * w + x1) * 4 + c]) / 4;
                    }
            free(level);
            level = next;
            w = nw;
            h = nh;
            size_t row_bytes, image_bytes;
            void *blocks = glm_compress_rgba(t->metal_format, true, level, w, h, &row_bytes, &image_bytes);
            if (!blocks) break;
            struct glm_texture_level *dst = &t->levels[f][l];
            *dst = (struct glm_texture_level){w, h, 1, t->levels[f][base].internal_format, true};
            glm_backend_texture_image(ctx, t, f, l, 0, 0, 0, w, h, 1, blocks, row_bytes, image_bytes);
            free(blocks);
        }
        free(level);
    }
    s->pack_alignment = pack[0];
    s->pack_row_length = pack[1];
    s->pack_skip_rows = pack[2];
    s->pack_skip_pixels = pack[3];
    s->pack_row_bytes = pack[4];
    s->pack_swap_bytes = pack_swap;
    ctx->pixel_pack_buffer = pack_buffer;
}

GLM_HIDDEN void glm_unpack_texels(uint32_t metal_format, const uint8_t *src, size_t count, float *rgba);
GLM_HIDDEN bool glm_unpack_texels_int(uint32_t metal_format, const uint8_t *src, size_t count, int64_t *rgba);
GLM_HIDDEN void glm_pack_texels(uint32_t f, const float *in, size_t count, uint8_t *dst);
GLM_HIDDEN void glm_pack_texels_int(uint32_t f, const int64_t *in, size_t count, uint8_t *dst);
GLM_HIDDEN size_t glm_metal_format_bytes(uint32_t metal_format);

/* Whether Metal's blit mipmapping takes the format (colour-renderable and
   filterable; anything else aborts in the driver). */
static bool metal_mipmappable(MTLPixelFormat f, const struct glm_format_info *info)
{
    if (info->compressed || info->integer || info->depth || info->stencil) return false;
    return f != MTLPixelFormatRGB9E5Float;
}

/* Other formats: each level the 2x2 box (nearest for integers) of the
   previous, through the CPU, per face / layer. */
static void generate_cpu_mipmaps(struct glm_context *ctx, struct glm_texture *t, id<MTLTexture> storage)
{
    if (storage.textureType == MTLTextureType3D || storage.sampleCount > 1) return;
    uint32_t f = (uint32_t)storage.pixelFormat;
    size_t texel = glm_metal_format_bytes(f);
    bool integer = glm_unpack_texels_int(f, NULL, 0, NULL);
    bool depth_stencil = f == MTLPixelFormatDepth32Float_Stencil8 || f == MTLPixelFormatStencil8;
    if (!texel || depth_stencil) return;
    NSUInteger slices = storage.textureType == MTLTextureTypeCube ? 6
                        : storage.textureType == MTLTextureTypeCubeArray ? 6 * storage.arrayLength : storage.arrayLength;
    int base = MAX(t->base_level, 0);
    for (NSUInteger slice = 0; slice < slices; ++slice) {
        NSUInteger w = MAX(storage.width >> base, 1u), h = MAX(storage.height >> base, 1u);
        size_t row = w * texel;
        id<MTLBuffer> staging = glm_new_shared_buffer(row * h, NULL);
        id<MTLBlitCommandEncoder> blit = [(__bridge id<MTLCommandBuffer>)glm_backend_command_buffer(ctx) blitCommandEncoder];
        [blit copyFromTexture:storage sourceSlice:slice sourceLevel:(NSUInteger)base sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(w, h, 1) toBuffer:staging destinationOffset:0 destinationBytesPerRow:row
      destinationBytesPerImage:row * h];
        [blit endEncoding];
        glm_backend_flush(ctx, true);
        float *level = calloc(w * h * 4, sizeof(float));
        int64_t *ilevel = calloc(w * h * 4, sizeof(int64_t));
        for (NSUInteger y = 0; y < h; ++y) {
            const uint8_t *src = (const uint8_t *)[staging contents] + y * row;
            if (integer) glm_unpack_texels_int(f, src, w, ilevel + y * w * 4);
            else glm_unpack_texels(f, src, w, level + y * w * 4);
        }
        for (NSUInteger l = (NSUInteger)base + 1; l < storage.mipmapLevelCount; ++l) {
            NSUInteger nw = MAX(w / 2, 1u), nh = MAX(h / 2, 1u);
            float *next = calloc(nw * nh * 4, sizeof(float));
            int64_t *inext = calloc(nw * nh * 4, sizeof(int64_t));
            for (NSUInteger y = 0; y < nh; ++y)
                for (NSUInteger x = 0; x < nw; ++x) {
                    NSUInteger x0 = MIN(x * 2, w - 1), x1 = MIN(x * 2 + 1, w - 1), y0 = MIN(y * 2, h - 1), y1 = MIN(y * 2 + 1, h - 1);
                    for (int c = 0; c < 4; ++c) {
                        size_t o = (y * nw + x) * 4 + (size_t)c;
                        if (integer) inext[o] = ilevel[(y0 * w + x0) * 4 + (size_t)c];
                        else
                            next[o] = (level[(y0 * w + x0) * 4 + (size_t)c] + level[(y0 * w + x1) * 4 + (size_t)c] +
                                       level[(y1 * w + x0) * 4 + (size_t)c] + level[(y1 * w + x1) * 4 + (size_t)c]) / 4;
                    }
                }
            free(level); free(ilevel);
            level = next; ilevel = inext;
            w = nw; h = nh;
            size_t bytes = w * texel * h;
            uint8_t *packed = malloc(bytes);
            if (integer) glm_pack_texels_int(f, ilevel, w * h, packed);
            else glm_pack_texels(f, level, w * h, packed);
            id<MTLBuffer> upload = glm_new_shared_buffer(bytes, packed);
            free(packed);
            id<MTLBlitCommandEncoder> up = [(__bridge id<MTLCommandBuffer>)glm_backend_command_buffer(ctx) blitCommandEncoder];
            [up copyFromBuffer:upload sourceOffset:0 sourceBytesPerRow:w * texel sourceBytesPerImage:bytes
                    sourceSize:MTLSizeMake(w, h, 1) toTexture:storage destinationSlice:slice destinationLevel:l
             destinationOrigin:MTLOriginMake(0, 0, 0)];
            [up endEncoding];
        }
        free(level); free(ilevel);
    }
    glm_backend_framebuffer_changed(ctx);
    glm_backend_upload_encoded(ctx);
}

void glm_backend_generate_mipmaps(struct glm_context *ctx, struct glm_texture *t)
{
    if (!glm_backend_texture_ensure_storage(ctx, t)) return;
    id<MTLTexture> storage = (__bridge id<MTLTexture>)t->backend;
    if (storage.mipmapLevelCount < 2) return;
    struct glm_format_info info;
    if (glm_format_lookup(t->levels[0][MAX(t->base_level, 0)].internal_format, &info) && info.compressed) {
        generate_compressed_mipmaps(ctx, t, storage);
        return;
    }
    if (!metal_mipmappable(storage.pixelFormat, &info)) {
        generate_cpu_mipmaps(ctx, t, storage);
        int base = t->base_level;
        for (int f = 0; f < face_count(t); ++f) {
            struct glm_texture_level *b = &t->levels[f][base];
            for (int level = base + 1; level < (int)storage.mipmapLevelCount && level < GLM_MAX_LEVELS; ++level)
                t->levels[f][level] = (struct glm_texture_level){MAX(b->width >> (level - base), 1),
                                                               layers_in_height(t->target) ? b->height : MAX(b->height >> (level - base), 1),
                                                               b->depth, b->internal_format, true};
        }
        return;
    }
    id<MTLBlitCommandEncoder> blit = [(__bridge id<MTLCommandBuffer>)glm_backend_command_buffer(ctx) blitCommandEncoder];
    [blit generateMipmapsForTexture:storage];
    [blit endEncoding];
    glm_backend_framebuffer_changed(ctx);
    glm_backend_upload_encoded(ctx);
    /* Every level now exists in storage; record them. */
    int base = t->base_level;
    for (int f = 0; f < face_count(t); ++f) {
        struct glm_texture_level *b = &t->levels[f][base];
        for (int level = base + 1; level < (int)storage.mipmapLevelCount && level < GLM_MAX_LEVELS; ++level) {
            struct glm_texture_level *l = &t->levels[f][level];
            l->width = MAX(b->width >> (level - base), 1);
            l->height = layers_in_height(t->target) ? b->height : MAX(b->height >> (level - base), 1);
            l->depth = t->target == GL_TEXTURE_3D ? MAX(b->depth >> (level - base), 1) : b->depth;
            l->internal_format = b->internal_format;
            l->defined = true;
        }
    }
}

/* ---- image specification ------------------------------------------------- */

static bool define_level(struct glm_context *ctx, struct glm_texture *t, GLenum target, GLint level, GLenum internal,
                         GLsizei w, GLsizei h, GLsizei d)
{
    if (level < 0 || level >= GLM_MAX_LEVELS || w < 0 || h < 0 || d < 0) {
        glm_error(ctx, GL_INVALID_VALUE);
        return false;
    }
    struct glm_format_info info;
    if (!glm_texture_format_allowed(internal, ctx->profile == GLM_PROFILE_CORE) || !glm_format_lookup(internal, &info)) {
        glm_log("unsupported internal format 0x%04x", internal);
        glm_error(ctx, GL_INVALID_VALUE);
        return false;
    }
    /* GL 4.1 section 3.8.3 excludes volume depth/depth-stencil images.
       Reject before changing the level or creating a Metal 3D texture. */
    if (target == GL_TEXTURE_3D && (info.depth || info.stencil)) {
        glm_error(ctx, GL_INVALID_OPERATION);
        return false;
    }
    struct glm_texture_level *l = &t->levels[face_index(target)][level];
    bool changed = !l->defined || l->width != w || l->height != h || l->depth != d || l->internal_format != internal;
    *l = (struct glm_texture_level){w, h, d, internal, true};
    if (changed) {
        /* Rebuild storage only if this level no longer fits it. */
        id<MTLTexture> storage = (__bridge id<MTLTexture>)t->backend;
        if (!storage || storage.pixelFormat != (MTLPixelFormat)info.metal_format ||
            (NSUInteger)level >= storage.mipmapLevelCount ||
            MAX(storage.width >> level, 1u) != (NSUInteger)MAX(w, 1) ||
            (t->target != GL_TEXTURE_1D && !layers_in_height(t->target) && MAX(storage.height >> level, 1u) != (NSUInteger)MAX(h, 1)) ||
            (t->target == GL_TEXTURE_3D && MAX(storage.depth >> level, 1u) != (NSUInteger)MAX(d, 1)) ||
            (layered(t->target) && storage.arrayLength != (NSUInteger)MAX(layers_in_height(t->target) ? h
                                                                         : t->target == GL_TEXTURE_CUBE_MAP_ARRAY ? d / 6 : d, 1)))
            t->storage_dirty = true;
    }
    return true;
}

static void upload(struct glm_context *ctx, struct glm_texture *t, GLenum target, GLint level, GLint x, GLint y, GLint z,
                   GLsizei w, GLsizei h, GLsizei d, GLenum format, GLenum type, const void *pixels)
{
    const void *source = pixels;
    if (ctx->pixel_unpack_buffer) {
        struct glm_buffer *buffer = glm_buffer_get(ctx, ctx->pixel_unpack_buffer);
        const uint8_t *contents = buffer ? glm_backend_buffer_contents(ctx, buffer, false) : NULL;
        if (!contents) return glm_error(ctx, GL_INVALID_OPERATION);
        source = contents + (uintptr_t)pixels;
    }
    if (!source || !w || !h) return;
    struct glm_texture_level *l = &t->levels[face_index(target)][level];
    struct glm_format_info info;
    if (!glm_format_lookup(l->internal_format, &info)) return;
    size_t row_bytes, image_bytes;
    void *converted = glm_convert_upload(ctx, &info, w, h, d, format, type, source, &row_bytes, &image_bytes);
    if (!converted) {
        glm_log("unsupported upload format 0x%04x type 0x%04x", format, type);
        return glm_error(ctx, GL_INVALID_ENUM);
    }
    if (l->internal_format == GL_R3_G3_B2) {
        /* Apple's 3-3-2 precision: the top bits, replicated back to 8. */
        uint8_t *p = converted;
        for (size_t i = 0; i < image_bytes * (size_t)(d ? d : 1); i += 4) {
            for (int c = 0; c < 2; ++c) {
                unsigned v = p[i + c] >> 5;
                p[i + c] = (uint8_t)(v << 5 | v << 2 | v >> 1);
            }
            p[i + 2] = (uint8_t)((p[i + 2] >> 6) * 0x55);
        }
    }
    glm_backend_texture_image(ctx, t, face_index(target), level, x, y, z, w, h, d, converted, row_bytes, image_bytes);
    free(converted);
    if (t->generate_mipmap && level == t->base_level) glm_backend_generate_mipmaps(ctx, t);
}

/* glTexImage on a proxy target: the image is checked against Apple's
   limits (16384, 2048 for 3D sizes and array layers) and recorded, or
   every property of the proxy level becomes 0. Nothing is stored. */
static void proxy_image(struct glm_context *ctx, int proxy, GLenum target, GLint level, GLenum internal, GLsizei w,
                        GLsizei h, GLsizei d, GLenum format, GLenum type)
{
    GLenum error = glm_tex_image_error(internal, format, type, ctx->profile == GLM_PROFILE_CORE);
    if (error) return glm_error(ctx, error);
    if (level < 0 || level >= GLM_MAX_LEVELS || w < 0 || h < 0 || d < 0) return glm_error(ctx, GL_INVALID_VALUE);
    if (target == GL_PROXY_TEXTURE_RECTANGLE && level) return glm_error(ctx, GL_INVALID_VALUE);
    GLsizei max = (target == GL_PROXY_TEXTURE_3D ? 2048 : 16384) >> level, layers = 2048;
    bool fits = w <= max;
    switch (target) {
    case GL_PROXY_TEXTURE_1D: fits &= h == 1 && d == 1; break;
    case GL_PROXY_TEXTURE_1D_ARRAY: fits &= h <= layers && d == 1; break;
    case GL_PROXY_TEXTURE_2D_ARRAY: case 0x900B: fits &= h <= max && d <= layers; break;
    case GL_PROXY_TEXTURE_3D: fits &= h <= max && d <= max; break;
    case GL_PROXY_TEXTURE_CUBE_MAP: fits &= h == w && d == 1; break;
    default: fits &= h <= max && d == 1; break;
    }
    ctx->proxy_levels[proxy][level] = fits ? (struct glm_texture_level){w, h, d, internal, true} : (struct glm_texture_level){0};
}

/* Whether `target` names images of glTex[Sub]Image`dims`D (proxies only
   for glTexImage). */
static bool image_target(GLenum target, int dims, bool sub)
{
    switch (target) {
    case GL_TEXTURE_1D: return dims == 1;
    case GL_PROXY_TEXTURE_1D: return dims == 1 && !sub;
    case GL_TEXTURE_2D: case GL_TEXTURE_RECTANGLE: case GL_TEXTURE_1D_ARRAY:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_X: case GL_TEXTURE_CUBE_MAP_NEGATIVE_X: case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y: case GL_TEXTURE_CUBE_MAP_POSITIVE_Z: case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
        return dims == 2;
    case GL_PROXY_TEXTURE_2D: case GL_PROXY_TEXTURE_RECTANGLE: case GL_PROXY_TEXTURE_1D_ARRAY: case GL_PROXY_TEXTURE_CUBE_MAP:
        return dims == 2 && !sub;
    case GL_TEXTURE_3D: case GL_TEXTURE_2D_ARRAY: case GL_TEXTURE_CUBE_MAP_ARRAY: return dims == 3;
    case GL_PROXY_TEXTURE_3D: case GL_PROXY_TEXTURE_2D_ARRAY: case 0x900B: return dims == 3 && !sub;
    default: return false;
    }
}

/* Sizes past Apple's limits (16384, 2048 for 3D textures and array layers,
   each halved per level) are invalid values; cube faces are square. */
static bool image_size_valid(GLenum target, GLint level, GLsizei w, GLsizei h, GLsizei d)
{
    if (level < 0 || level >= GLM_MAX_LEVELS || w < 0 || h < 0 || d < 0) return false;
    GLsizei max = (target == GL_TEXTURE_3D ? 2048 : 16384) >> level;
    switch (target) {
    case GL_TEXTURE_1D: return w <= max;
    case GL_TEXTURE_1D_ARRAY: return w <= max && h <= 2048;
    case GL_TEXTURE_2D_ARRAY: case GL_TEXTURE_CUBE_MAP_ARRAY: return w <= max && h <= max && d <= 2048;
    case GL_TEXTURE_3D: return w <= max && h <= max && d <= max;
    case GL_TEXTURE_RECTANGLE: return level == 0 && w <= 16384 && h <= 16384;
    case GL_TEXTURE_CUBE_MAP_POSITIVE_X: case GL_TEXTURE_CUBE_MAP_NEGATIVE_X: case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y: case GL_TEXTURE_CUBE_MAP_POSITIVE_Z: case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
        return w == h && w <= max;
    default: return w <= max && h <= max;
    }
}

static void tex_image(int dims, GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLsizei d, GLint border,
                      GLenum format, GLenum type, const GLvoid *pixels);

GLM_EXPORT void glTexImage3D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLsizei d, GLint border,
                             GLenum format, GLenum type, const GLvoid *pixels)
{
    tex_image(3, target, level, internal, w, h, d, border, format, type, pixels);
}

static void tex_image(int dims, GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLsizei d, GLint border,
                      GLenum format, GLenum type, const GLvoid *pixels)
{
    GLM_CONTEXT(ctx);
    if (!image_target(target, dims, false)) return glm_error(ctx, GL_INVALID_ENUM);
    /* Uncompressed uploads may request RGTC for a volume. GL 4.1 section
       3.8.3 requires uncompressed fallback when that compression format cannot
       represent the target; the CompressedTexImage target restrictions do not
       apply to TexImage. Keep signed formats signed, matching Apple's storage. */
    if (ctx->profile == GLM_PROFILE_CORE && (target == GL_TEXTURE_3D || target == GL_PROXY_TEXTURE_3D)) {
        switch (internal) {
        case GL_COMPRESSED_RED_RGTC1: internal = GL_R8; break;
        case GL_COMPRESSED_SIGNED_RED_RGTC1: internal = GL_R8_SNORM; break;
        case GL_COMPRESSED_RG_RGTC2: internal = GL_RG8; break;
        case GL_COMPRESSED_SIGNED_RG_RGTC2: internal = GL_RG8_SNORM; break;
        }
    }
    int proxy = proxy_index(target);
    if (target == GL_PROXY_TEXTURE_3D) {
        struct glm_format_info info;
        if (glm_format_lookup((GLenum)internal, &info) && (info.depth || info.stencil))
            return glm_error(ctx, GL_INVALID_OPERATION);
    }
    if (proxy >= 0) return proxy_image(ctx, proxy, target, level, (GLenum)internal, w, h, d, format, type);
    static int logged;
    if (d > 1 && logged < 6) {
        ++logged;
        glm_log("glTexImage3D target 0x%04x level %d internal 0x%04x %dx%dx%d format 0x%04x type 0x%04x unpack buffer %u", target,
                level, internal, w, h, d, format, type, ctx->pixel_unpack_buffer);
    }
    (void)border;
    struct glm_texture *t = glm_texture_bound(ctx, target);
    if (!t) return glm_error(ctx, GL_INVALID_ENUM);
    GLenum error = glm_tex_image_error((GLenum)internal, format, type, ctx->profile == GLM_PROFILE_CORE);
    if (error) return glm_error(ctx, error);
    if (!image_size_valid(target, level, w, h, d)) return glm_error(ctx, GL_INVALID_VALUE);
    if (t->immutable) return glm_error(ctx, GL_INVALID_OPERATION);
    if (!t->target) t->target = binding_target(target);
    if (!define_level(ctx, t, target, level, (GLenum)internal, w, h, d)) return;
    if (t->storage_dirty && level == t->base_level) glm_backend_texture_ensure_storage(ctx, t);
    if (pixels || ctx->pixel_unpack_buffer) upload(ctx, t, target, level, 0, 0, 0, w, h, d, format, type, pixels);
    else glm_backend_texture_ensure_storage(ctx, t);
}

/* ---- immutable storage (ARB_texture_storage) and multisample images ------- */

static void storage(GLenum target, GLsizei levels, GLenum internal, GLsizei w, GLsizei h, GLsizei d)
{
    GLM_CONTEXT(ctx);
    struct glm_texture *t = glm_texture_bound(ctx, target);
    if (!t) return glm_error(ctx, GL_INVALID_ENUM);
    if (t->immutable) return glm_error(ctx, GL_INVALID_OPERATION);
    if (levels < 1 || w < 1 || h < 1 || d < 1) return glm_error(ctx, GL_INVALID_VALUE);
    if (!t->target) t->target = target;
    int faces = target == GL_TEXTURE_CUBE_MAP ? 6 : 1;
    for (GLsizei level = 0; level < levels && level < GLM_MAX_LEVELS; ++level) {
        GLsizei lw = MAX(w >> level, 1);
        GLsizei lh = layers_in_height(target) ? h : MAX(h >> level, 1);
        GLsizei ld = target == GL_TEXTURE_3D ? MAX(d >> level, 1) : d;
        for (int f = 0; f < faces; ++f) {
            GLenum face = faces == 6 ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)f : target;
            if (!define_level(ctx, t, face, level, internal, lw, lh, ld)) return;
        }
    }
    t->immutable = true;
    t->immutable_levels = levels;
    glm_backend_texture_ensure_storage(ctx, t);
}

GLM_EXPORT void glTexStorage1D(GLenum target, GLsizei levels, GLenum internal, GLsizei w) { storage(target, levels, internal, w, 1, 1); }
GLM_EXPORT void glTexStorage2D(GLenum target, GLsizei levels, GLenum internal, GLsizei w, GLsizei h)
{
    storage(target, levels, internal, w, h, 1);
}
GLM_EXPORT void glTexStorage3D(GLenum target, GLsizei levels, GLenum internal, GLsizei w, GLsizei h, GLsizei d)
{
    storage(target, levels, internal, w, h, d);
}

static void multisample_image(GLenum target, GLsizei samples, GLenum internal, GLsizei w, GLsizei h, GLsizei d,
                              GLboolean fixed_locations)
{
    GLM_CONTEXT(ctx);
    struct glm_texture *t = glm_texture_bound(ctx, target);
    if (!t || !multisampled(target)) return glm_error(ctx, GL_INVALID_ENUM);
    if (samples < 1 || w < 1 || h < 1 || d < 1) return glm_error(ctx, GL_INVALID_VALUE);
    if (!t->target) t->target = target;
    /* A multisample Metal texture requires at least two samples. Choose a
       supported count at or above the request, including requests for one. */
    GLsizei metal_samples = 0;
    for (GLsizei count = 2; count <= 8; count *= 2)
        if (count >= samples && [metal_device() supportsTextureSampleCount:(NSUInteger)count]) {
            metal_samples = count;
            break;
        }
    if (!metal_samples) return glm_error(ctx, GL_INVALID_VALUE);
    if (t->samples != metal_samples) t->storage_dirty = true;
    t->samples = metal_samples;
    /* This backend always uses Metal's fixed sample positions. Apple reports
       that actual layout even when the caller permits variable positions. */
    t->fixed_sample_locations = true;
    if (!define_level(ctx, t, target, 0, internal, w, h, d)) return;
    glm_backend_texture_ensure_storage(ctx, t);
}

GLM_EXPORT void glTexImage2DMultisample(GLenum target, GLsizei samples, GLenum internal, GLsizei w, GLsizei h,
                                        GLboolean fixed_locations)
{
    multisample_image(target, samples, internal, w, h, 1, fixed_locations);
}
GLM_EXPORT void glTexImage3DMultisample(GLenum target, GLsizei samples, GLenum internal, GLsizei w, GLsizei h, GLsizei d,
                                        GLboolean fixed_locations)
{
    multisample_image(target, samples, internal, w, h, d, fixed_locations);
}

/* Apple exposes Metal's standard sample-index coordinates through this query.
   The framebuffer origin conversion does not invert the reported pattern. */
GLM_EXPORT void glGetMultisamplefv(GLenum pname, GLuint index, GLfloat *value)
{
    GLM_CONTEXT(ctx);
    if (pname != GL_SAMPLE_POSITION) return glm_error(ctx, GL_INVALID_ENUM);
    NSUInteger samples = glm_backend_draw_samples(ctx);
    if (index >= samples) return glm_error(ctx, GL_INVALID_VALUE);
    if (samples <= 1) {
        value[0] = value[1] = 0.5f;
        return;
    }
    MTLSamplePosition positions[16];
    [metal_device() getDefaultSamplePositions:positions count:samples];
    value[0] = positions[index].x;
    value[1] = positions[index].y;
}

GLM_EXPORT void glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format,
                             GLenum type, const GLvoid *pixels)
{
    tex_image(2, target, level, internal, w, h, 1, border, format, type, pixels);
}

GLM_EXPORT void glTexImage1D(GLenum target, GLint level, GLint internal, GLsizei w, GLint border, GLenum format,
                             GLenum type, const GLvoid *pixels)
{
    tex_image(1, target, level, internal, w, 1, 1, border, format, type, pixels);
}

static void tex_sub_image(int dims, GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d,
                          GLenum format, GLenum type, const GLvoid *pixels);

GLM_EXPORT void glTexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d,
                                GLenum format, GLenum type, const GLvoid *pixels)
{
    tex_sub_image(3, target, level, x, y, z, w, h, d, format, type, pixels);
}

static void tex_sub_image(int dims, GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d,
                          GLenum format, GLenum type, const GLvoid *pixels)
{
    GLM_CONTEXT(ctx);
    if (!image_target(target, dims, true)) return glm_error(ctx, GL_INVALID_ENUM);
    struct glm_texture *t = glm_texture_bound(ctx, target);
    if (!t || level < 0 || level >= GLM_MAX_LEVELS) return glm_error(ctx, GL_INVALID_VALUE);
    struct glm_texture_level *l = &t->levels[face_index(target)][level];
    GLenum error = glm_format_type_error(format, type, ctx->profile == GLM_PROFILE_CORE);
    if (error) return glm_error(ctx, error);
    if (!l->defined) return glm_error(ctx, GL_INVALID_OPERATION);
    error = glm_tex_image_error(l->internal_format, format, type, ctx->profile == GLM_PROFILE_CORE);
    if (error) return glm_error(ctx, error);
    if (x < 0 || y < 0 || z < 0 || x + w > l->width || y + h > l->height) return glm_error(ctx, GL_INVALID_VALUE);
    upload(ctx, t, target, level, x, y, z, w, h, d, format, type, pixels);
}

GLM_EXPORT void glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                                GLenum type, const GLvoid *pixels)
{
    tex_sub_image(2, target, level, x, y, 0, w, h, 1, format, type, pixels);
}

GLM_EXPORT void glTexSubImage1D(GLenum target, GLint level, GLint x, GLsizei w, GLenum format, GLenum type, const GLvoid *pixels)
{
    tex_sub_image(1, target, level, x, 0, 0, w, 1, 1, format, type, pixels);
}

static void compressed3d(GLenum target, GLint level, GLenum internal, GLint x, GLint y, GLint z, GLsizei w, GLsizei h,
                         GLsizei d, GLsizei image_size, const GLvoid *data, bool sub)
{
    GLM_CONTEXT(ctx);
    struct glm_texture *t = glm_texture_bound(ctx, target);
    if (!t) return glm_error(ctx, GL_INVALID_ENUM);
    if (!t->target) t->target = binding_target(target);
    if (t->immutable && !sub) return glm_error(ctx, GL_INVALID_OPERATION);
    /* Block compression is per 2D image: arrays only, not volumes. */
    if (d > 1 && !layers_in_depth(t->target)) return glm_error(ctx, GL_INVALID_OPERATION);
    if (!sub && !define_level(ctx, t, target, level, internal, w, h, d)) return;
    struct glm_texture_level *l = &t->levels[face_index(target)][level];
    struct glm_format_info info;
    if (!glm_format_lookup(sub ? l->internal_format : internal, &info) || !info.compressed) return glm_error(ctx, GL_INVALID_ENUM);
    const void *source = data;
    if (ctx->pixel_unpack_buffer) {
        struct glm_buffer *buffer = glm_buffer_get(ctx, ctx->pixel_unpack_buffer);
        const uint8_t *contents = buffer ? glm_backend_buffer_contents(ctx, buffer, false) : NULL;
        if (!contents) return glm_error(ctx, GL_INVALID_OPERATION);
        source = contents + (uintptr_t)data;
    }
    size_t row_bytes = (size_t)((w + 3) / 4) * (size_t)info.bytes_per_pixel;
    size_t image_bytes = row_bytes * (size_t)((h + 3) / 4);
    if ((size_t)image_size < image_bytes * (size_t)MAX(d, 1)) return glm_error(ctx, GL_INVALID_VALUE);
    if (!glm_backend_texture_ensure_storage(ctx, t)) return;
    /* Levels smaller than a block still occupy whole blocks in Metal; the
       copy size must cover the level exactly. */
    glm_backend_texture_image(ctx, t, face_index(target), level, x, y, z, w, h, MAX(d, 1), source, row_bytes, image_bytes);
}

static void compressed(GLenum target, GLint level, GLenum internal, GLint x, GLint y, GLsizei w, GLsizei h, GLsizei image_size,
                       const GLvoid *data, bool sub)
{
    compressed3d(target, level, internal, x, y, 0, w, h, 1, image_size, data, sub);
}

GLM_EXPORT void glCompressedTexImage3D(GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLsizei d,
                                       GLint border, GLsizei image_size, const GLvoid *data)
{ (void)border; compressed3d(target, level, internal, 0, 0, 0, w, h, d, image_size, data, false); }
GLM_EXPORT void glCompressedTexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h,
                                          GLsizei d, GLenum format, GLsizei image_size, const GLvoid *data)
{ compressed3d(target, level, format, x, y, z, w, h, d, image_size, data, true); }
/* No 1D compressed formats exist. */
GLM_EXPORT void glCompressedTexImage1D(GLenum target, GLint level, GLenum internal, GLsizei w, GLint border,
                                       GLsizei image_size, const GLvoid *data)
{
    GLM_CONTEXT(ctx);
    (void)target; (void)level; (void)internal; (void)w; (void)border; (void)image_size; (void)data;
    glm_error(ctx, GL_INVALID_ENUM);
}
GLM_EXPORT void glCompressedTexSubImage1D(GLenum target, GLint level, GLint x, GLsizei w, GLenum format,
                                          GLsizei image_size, const GLvoid *data)
{
    GLM_CONTEXT(ctx);
    (void)target; (void)level; (void)x; (void)w; (void)format; (void)image_size; (void)data;
    glm_error(ctx, GL_INVALID_ENUM);
}

GLM_EXPORT void glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels)
{
    GLM_CONTEXT(ctx);
    struct glm_texture *t = glm_texture_bound(ctx, target);
    if (!t || level < 0 || level >= GLM_MAX_LEVELS) return glm_error(ctx, GL_INVALID_VALUE);
    const struct glm_texture_level *level_info = &t->levels[face_index(target)][level];
    struct glm_format_info level_format;
    bool depth_level = level_info->defined && glm_format_lookup(level_info->internal_format, &level_format) &&
                       (level_format.depth || level_format.stencil);
    GLenum format_error = glm_read_error(level_info->defined ? level_info->internal_format : GL_RGBA8,
                                         depth_level && level_format.depth, depth_level && level_format.stencil, format,
                                         type, ctx->profile == GLM_PROFILE_CORE, false);
    if (format_error) return glm_error(ctx, format_error);
    uint8_t *destination = pixels;
    if (ctx->pixel_pack_buffer) {
        struct glm_buffer *buffer = glm_buffer_get(ctx, ctx->pixel_pack_buffer);
        uint8_t *contents = buffer ? glm_backend_buffer_contents(ctx, buffer, true) : NULL;
        if (!contents) return glm_error(ctx, GL_INVALID_OPERATION);
        destination = contents + (uintptr_t)pixels;
    }
    glm_backend_get_tex_image(ctx, t, face_index(target), level, format, type, destination, false);
}

GLM_EXPORT void glGetCompressedTexImage(GLenum target, GLint level, GLvoid *pixels)
{
    GLM_CONTEXT(ctx);
    struct glm_texture *t = glm_texture_bound(ctx, target);
    if (!t || level < 0 || level >= GLM_MAX_LEVELS) return glm_error(ctx, GL_INVALID_VALUE);
    struct glm_format_info info;
    const struct glm_texture_level *l = &t->levels[face_index(target)][level];
    if (!l->defined || !glm_format_lookup(l->internal_format, &info) || !info.compressed)
        return glm_error(ctx, GL_INVALID_OPERATION);
    uint8_t *destination = pixels;
    if (ctx->pixel_pack_buffer) {
        struct glm_buffer *buffer = glm_buffer_get(ctx, ctx->pixel_pack_buffer);
        uint8_t *contents = buffer ? glm_backend_buffer_contents(ctx, buffer, true) : NULL;
        if (!contents) return glm_error(ctx, GL_INVALID_OPERATION);
        destination = contents + (uintptr_t)pixels;
    }
    glm_backend_get_tex_image(ctx, t, face_index(target), level, 0, 0, destination, true);
}
GLM_EXPORT void glGetCompressedTexImageARB(GLenum target, GLint level, GLvoid *pixels) { glGetCompressedTexImage(target, level, pixels); }

GLM_EXPORT void glCompressedTexImage2D(GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLint border,
                                       GLsizei image_size, const GLvoid *data)
{ (void)border; compressed(target, level, internal, 0, 0, w, h, image_size, data, false); }
GLM_EXPORT void glCompressedTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                                          GLsizei image_size, const GLvoid *data)
{ compressed(target, level, format, x, y, w, h, image_size, data, true); }
GLM_EXPORT void glCompressedTexImage2DARB(GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLint border,
                                          GLsizei image_size, const GLvoid *data)
{ glCompressedTexImage2D(target, level, internal, w, h, border, image_size, data); }
GLM_EXPORT void glCompressedTexSubImage2DARB(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                                             GLsizei image_size, const GLvoid *data)
{ glCompressedTexSubImage2D(target, level, x, y, w, h, format, image_size, data); }

GLM_EXPORT void glGenerateMipmap(GLenum target)
{
    GLM_CONTEXT(ctx);
    struct glm_texture *t = glm_texture_bound(ctx, target);
    if (!t) return glm_error(ctx, GL_INVALID_ENUM);
    glm_backend_generate_mipmaps(ctx, t);
}
GLM_EXPORT void glGenerateMipmapEXT(GLenum target) { glGenerateMipmap(target); }

static GLenum copy_error(struct glm_context *ctx, GLenum internal);

static void copy_tex_sub_image(int dims, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x,
                               GLint y, GLsizei w, GLsizei h);

GLM_EXPORT void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei w, GLsizei h)
{
    copy_tex_sub_image(2, target, level, xoffset, yoffset, 0, x, y, w, h);
}

static void copy_tex_sub_image(int dims, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x,
                               GLint y, GLsizei w, GLsizei h)
{
    GLM_CONTEXT(ctx);
    if (!image_target(target, dims, true)) return glm_error(ctx, GL_INVALID_ENUM);
    if (level < 0 || level >= GLM_MAX_LEVELS || w < 0 || h < 0 || xoffset < 0 || yoffset < 0 || zoffset < 0)
        return glm_error(ctx, GL_INVALID_VALUE);
    struct glm_texture *t = glm_texture_bound(ctx, target);
    int face = face_index(target);
    if (!t || !t->levels[face][level].defined) return glm_error(ctx, GL_INVALID_OPERATION);
    GLenum error = copy_error(ctx, t->levels[face][level].internal_format);
    if (error) return glm_error(ctx, error);
    if (dims == 3) {
        if (!glm_backend_texture_ensure_storage(ctx, t)) return glm_error(ctx, GL_INVALID_OPERATION);
        glm_backend_copy_framebuffer_to_texture(ctx, t, 0, level, xoffset, yoffset, zoffset, x, y, w, h);
        return;
    }
    if (!glm_backend_texture_ensure_storage(ctx, t)) return glm_error(ctx, GL_INVALID_OPERATION);
    glm_backend_copy_framebuffer_to_texture(ctx, t, face, level, xoffset, yoffset, 0, x, y, w, h);
}

GLM_EXPORT void glCopyTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y,
                                    GLsizei w, GLsizei h)
{
    copy_tex_sub_image(3, target, level, xoffset, yoffset, zoffset, x, y, w, h);
}

GLM_EXPORT void glCopyTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLint x, GLint y, GLsizei w)
{
    copy_tex_sub_image(1, target, level, xoffset, 0, 0, x, y, w, 1);
}

static void copy_tex_image(int dims, GLenum target, GLint level, GLenum internal, GLint x, GLint y, GLsizei w, GLsizei h);

GLM_EXPORT void glCopyTexImage1D(GLenum target, GLint level, GLenum internal, GLint x, GLint y, GLsizei w, GLint border)
{
    (void)border;
    copy_tex_image(1, target, level, internal, x, y, w, 1);
}

/* Copying the read buffer into `internal`, as Apple's implementation
   checks it (build/probes/copysweep.c): a format glTexImage2D takes (not
   1-4), then depth formats from a depth buffer only, integer formats from
   an integer buffer only, the others from a normalized / float one. */
static GLenum copy_error(struct glm_context *ctx, GLenum internal)
{
    if (internal <= 4 || !glm_apple_texture_format(ctx, internal)) return GL_INVALID_ENUM;
    if (glm_framebuffer_status_of(ctx, ctx->read_framebuffer) != GL_FRAMEBUFFER_COMPLETE)
        return GL_INVALID_FRAMEBUFFER_OPERATION;
    bool depth, stencil;
    GLenum source = glm_read_buffer_format(ctx, &depth, &stencil);
    struct glm_format_info want, have;
    if (!glm_format_lookup(internal, &want)) return GL_INVALID_ENUM;
    if (want.depth || want.stencil)
        return (want.depth && !depth) || (want.stencil && !stencil) ? GL_INVALID_OPERATION : GL_NO_ERROR;
    if (!source || !glm_format_lookup(source, &have) || have.depth || have.stencil) return GL_INVALID_OPERATION;
    return want.integer == have.integer ? GL_NO_ERROR : GL_INVALID_OPERATION;
}

GLM_EXPORT void glCopyTexImage2D(GLenum target, GLint level, GLenum internal, GLint x, GLint y, GLsizei w, GLsizei h, GLint border)
{
    (void)border;
    copy_tex_image(2, target, level, internal, x, y, w, h);
}

static void copy_tex_image(int dims, GLenum target, GLint level, GLenum internal, GLint x, GLint y, GLsizei w, GLsizei h)
{
    GLM_CONTEXT(ctx);
    if (!image_target(target, dims, true) || target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY)
        return glm_error(ctx, GL_INVALID_ENUM);
    struct glm_texture *t = glm_texture_bound(ctx, target);
    if (!t) return glm_error(ctx, GL_INVALID_ENUM);
    GLenum error = copy_error(ctx, internal);
    if (error) return glm_error(ctx, error);
    if (!image_size_valid(target, level, w, h, 1)) return glm_error(ctx, GL_INVALID_VALUE);
    if (!t->target) t->target = binding_target(target);
    if (!define_level(ctx, t, target, level, internal, w, h, 1)) return;
    copy_tex_sub_image(dims, target, level, 0, 0, 0, x, y, w, h);
}

/* ---- framebuffer attachments -------------------------------------------- */

id<MTLTexture> glm_texture_view_for_attachment(struct glm_context *ctx, const struct glm_attachment *a, NSUInteger *slice,
                                               NSUInteger *level);
id<MTLTexture> glm_texture_view_for_attachment(struct glm_context *ctx, const struct glm_attachment *a, NSUInteger *slice,
                                               NSUInteger *level)
{
    struct glm_texture *t = glm_texture_get(ctx, a->name);
    if (!t || !glm_backend_texture_ensure_storage(ctx, t)) return nil;
    *level = (NSUInteger)a->level;
    *slice = a->face ? (NSUInteger)(a->face - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : (NSUInteger)a->layer;
    return (__bridge id<MTLTexture>)t->backend;
}

/* ---- sampling ------------------------------------------------------------ */

static bool mipmapped_filter(GLenum f) { return f != GL_NEAREST && f != GL_LINEAR; }

/* GL 2.1 3.8.10 completeness for the levels a draw can use. */
static bool complete(struct glm_texture *t)
{
    int base = t->base_level;
    if (base < 0 || base >= GLM_MAX_LEVELS) return false;
    for (int f = 0; f < face_count(t); ++f)
        if (!t->levels[f][base].defined) return false;
    if (!mipmapped_filter(t->sampler.min_filter) || multisampled(t->target)) return true;
    const struct glm_texture_level *b = &t->levels[0][base];
    bool rows = layers_in_height(t->target);
    int last = base + mip_count(b->width, rows ? 1 : b->height, t->target == GL_TEXTURE_3D ? b->depth : 1) - 1;
    if (t->max_level < last) last = t->max_level;
    if (t->immutable && base + t->immutable_levels - 1 < last) last = base + t->immutable_levels - 1;
    for (int f = 0; f < face_count(t); ++f)
        for (int level = base; level <= last && level < GLM_MAX_LEVELS; ++level) {
            const struct glm_texture_level *l = &t->levels[f][level];
            if (!l->defined || l->internal_format != b->internal_format || l->width != MAX(b->width >> (level - base), 1) ||
                l->height != (rows ? b->height : MAX(b->height >> (level - base), 1)))
                return false;
        }
    return true;
}

static bool complete(struct glm_texture *t);
struct glm_texture *glm_unit_texture(struct glm_context *ctx, int unit, int *target_code)
{
    const struct glm_texture_unit *u = &ctx->state.units[unit];
    int slot = -1;
    *target_code = 0;
    /* Fixed-function priority: cube > 3D > rect > 2D > 1D. */
    if (u->enabled_cube) { slot = GLM_TEX_CUBE; *target_code = 4; }
    else if (u->enabled_3d) { slot = GLM_TEX_3D; *target_code = 3; }
    else if (u->enabled_rect) { slot = GLM_TEX_RECT; *target_code = 5; }
    else if (u->enabled_2d) { slot = GLM_TEX_2D; *target_code = 2; }
    else if (u->enabled_1d) { slot = GLM_TEX_1D; *target_code = 2; }
    if (slot < 0) return NULL;
    GLuint name = u->bound[slot];
    struct glm_texture *t = name ? glm_texture_get(ctx, name) : default_texture(ctx, slot);
    /* An incomplete texture disables the unit (GL 2.1 3.8.15). */
    if (t && !complete(t)) {
        *target_code = 0;
        return NULL;
    }
    return t;
}

static MTLSamplerAddressMode address_mode(GLenum wrap)
{
    switch (wrap) {
    case GL_CLAMP_TO_EDGE: return MTLSamplerAddressModeClampToEdge;
    case GL_MIRRORED_REPEAT: return MTLSamplerAddressModeMirrorRepeat;
    case GL_CLAMP: return MTLSamplerAddressModeClampToEdge;
    case GL_CLAMP_TO_BORDER: return MTLSamplerAddressModeClampToBorderColor;
    case GL_MIRROR_CLAMP_TO_EDGE_EXT: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}

/* Apple GPUs expose seamful cube filtering through this descriptor SPI.
   B in the runtime signatures is a C bool, including on x86_64. */
static bool native_cube_seams_supported(void)
{
    static dispatch_once_t once;
    static bool supported;
    dispatch_once(&once, ^{
        id<MTLDevice> device = (__bridge id<MTLDevice>)glm_backend_device();
        if (![device supportsFamily:MTLGPUFamilyApple1]) return;
        MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
        SEL setter = sel_registerName("setForceSeamsOnCubemapFiltering:");
        SEL getter = sel_registerName("forceSeamsOnCubemapFiltering");
        if (![descriptor respondsToSelector:setter] || ![descriptor respondsToSelector:getter]) return;
        ((void (*)(id, SEL, bool))objc_msgSend)(descriptor, setter, true);
        supported = ((bool (*)(id, SEL))objc_msgSend)(descriptor, getter);
        ((void (*)(id, SEL, bool))objc_msgSend)(descriptor, setter, false);
        supported = supported && !((bool (*)(id, SEL))objc_msgSend)(descriptor, getter);
    });
    return supported;
}

static bool native_cube_seams(struct glm_context *ctx, const struct glm_sampler_state *s, bool cube)
{
    return cube && !ctx->state.texture_cube_seamless && s && s->wrap_s == GL_CLAMP_TO_EDGE &&
           s->wrap_t == GL_CLAMP_TO_EDGE && native_cube_seams_supported();
}

static id<MTLSamplerState> sampler_for(const struct glm_sampler_state *s, float unit_bias, bool mipmapped, bool rect,
                                     bool cube_seams)
{
    static NSMutableDictionary<NSData *, id<MTLSamplerState>> *cache;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    struct { struct glm_sampler_state s; float bias; uint8_t mip, rect, cube_seams; } key;
    memset(&key, 0, sizeof key);
    key.s = *s;
    key.s.skip_srgb_decode = false; /* a view format, not sampler state */
    key.bias = unit_bias;
    key.mip = mipmapped;
    key.rect = rect;
    key.cube_seams = cube_seams;
    /* Multi-texture draws alternate a small set of samplers. Keep that
       working set per thread, avoiding NSData allocation and the shared
       dictionary lock on every texture binding. The dictionary owns the
       immutable Metal states for the lifetime of the process. */
    enum { HOT_SAMPLERS = 64 };
    static __thread struct {
        __typeof__(key) key;
        __unsafe_unretained id<MTLSamplerState> state;
    } hot[HOT_SAMPLERS];
    uint32_t hash = 2166136261u;
    const unsigned char *bytes = (const unsigned char *)&key;
    for (size_t i = 0; i < sizeof key; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    unsigned slot = hash & (HOT_SAMPLERS - 1);
    if (hot[slot].state && !memcmp(&key, &hot[slot].key, sizeof key)) return hot[slot].state;
    NSData *data = [NSData dataWithBytes:&key length:sizeof key];
    pthread_mutex_lock(&lock);
    if (!cache) cache = [NSMutableDictionary dictionary];
    id<MTLSamplerState> state = cache[data];
    if (!state) {
        MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
        if (cube_seams)
            ((void (*)(id, SEL, bool))objc_msgSend)(d, sel_registerName("setForceSeamsOnCubemapFiltering:"), true);
        d.magFilter = s->mag_filter == GL_NEAREST ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
        GLenum min = s->min_filter;
        d.minFilter = (min == GL_NEAREST || min == GL_NEAREST_MIPMAP_NEAREST || min == GL_NEAREST_MIPMAP_LINEAR)
                          ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
        d.mipFilter = !mipmapped || !mipmapped_filter(min) ? MTLSamplerMipFilterNotMipmapped
                      : (min == GL_NEAREST_MIPMAP_NEAREST || min == GL_LINEAR_MIPMAP_NEAREST) ? MTLSamplerMipFilterNearest
                                                                                              : MTLSamplerMipFilterLinear;
        d.sAddressMode = address_mode(s->wrap_s);
        d.tAddressMode = address_mode(s->wrap_t);
        d.rAddressMode = address_mode(s->wrap_r);
        /* Metal has three border colours; pick the closest. */
        const float *b = s->border_color;
        d.borderColor = b[3] < 0.5f ? MTLSamplerBorderColorTransparentBlack
                        : (b[0] + b[1] + b[2]) > 1.5f ? MTLSamplerBorderColorOpaqueWhite : MTLSamplerBorderColorOpaqueBlack;
        d.lodMinClamp = MAX(s->min_lod, 0.0f);
        d.lodMaxClamp = MIN(s->max_lod, 1000.0f);
        d.maxAnisotropy = (NSUInteger)MAX(1.0f, MIN(s->max_anisotropy, 16.0f));
        if (s->compare_mode == GL_COMPARE_R_TO_TEXTURE) {
            static const MTLCompareFunction functions[] = {MTLCompareFunctionNever, MTLCompareFunctionLess, MTLCompareFunctionEqual,
                                                           MTLCompareFunctionLessEqual, MTLCompareFunctionGreater,
                                                           MTLCompareFunctionNotEqual, MTLCompareFunctionGreaterEqual,
                                                           MTLCompareFunctionAlways};
            d.compareFunction = functions[(s->compare_func - GL_NEVER) & 7];
        }
        if (rect) {
            /* Rectangle textures: texel coordinates, one level, clamping. */
            d.normalizedCoordinates = NO;
            d.mipFilter = MTLSamplerMipFilterNotMipmapped;
            d.sAddressMode = d.sAddressMode == MTLSamplerAddressModeClampToBorderColor ? MTLSamplerAddressModeClampToZero
                                                                                     : MTLSamplerAddressModeClampToEdge;
            d.tAddressMode = d.tAddressMode == MTLSamplerAddressModeClampToBorderColor ? MTLSamplerAddressModeClampToZero
                                                                                     : MTLSamplerAddressModeClampToEdge;
            d.rAddressMode = MTLSamplerAddressModeClampToEdge;
            d.lodMinClamp = 0;
            d.lodMaxClamp = 0;
            d.maxAnisotropy = 1;
        }
        d.supportArgumentBuffers = NO;
        state = [(__bridge id<MTLDevice>)glm_backend_device() newSamplerStateWithDescriptor:d];
        cache[data] = state;
    }
    pthread_mutex_unlock(&lock);
    hot[slot].key = key;
    hot[slot].state = state;
    return state;
}

/* A 1x1 (0,0,0,1) texture per type for incomplete textures (GL 3.8.10). */
static id<MTLTexture> incomplete_texture(MTLTextureType type)
{
    static id<MTLTexture> textures[16];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (!textures[type]) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor new];
        d.textureType = type;
        d.pixelFormat = MTLPixelFormatRGBA8Unorm;
        d.width = d.height = 1;
        bool ms = type == MTLTextureType2DMultisample || type == MTLTextureType2DMultisampleArray;
        d.storageMode = ms ? MTLStorageModePrivate : MTLStorageModeShared;
        if (ms) d.sampleCount = 4;
        id<MTLTexture> t = [metal_device() newTextureWithDescriptor:d];
        const uint8_t black[4] = {0, 0, 0, 255};
        NSUInteger slices = type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray ? 6 : 1;
        if (!ms)
            for (NSUInteger s = 0; s < slices; ++s)
                [t replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:s withBytes:black bytesPerRow:4 bytesPerImage:4];
        textures[type] = t;
    }
    id<MTLTexture> result = textures[type];
    pthread_mutex_unlock(&lock);
    return result;
}

/* The texture a draw samples: a view over [base, max] levels so Metal's
   LOD 0 is GL's base level. */
/* The non-sRGB twin of an sRGB format (GL_SKIP_DECODE_EXT). */
static MTLPixelFormat linear_format(MTLPixelFormat f)
{
    switch (f) {
    case MTLPixelFormatRGBA8Unorm_sRGB: return MTLPixelFormatRGBA8Unorm;
    case MTLPixelFormatBGRA8Unorm_sRGB: return MTLPixelFormatBGRA8Unorm;
    case MTLPixelFormatBC1_RGBA_sRGB: return MTLPixelFormatBC1_RGBA;
    case MTLPixelFormatBC2_RGBA_sRGB: return MTLPixelFormatBC2_RGBA;
    case MTLPixelFormatBC3_RGBA_sRGB: return MTLPixelFormatBC3_RGBA;
    case MTLPixelFormatBC7_RGBAUnorm_sRGB: return MTLPixelFormatBC7_RGBAUnorm;
    case MTLPixelFormatR8Unorm_sRGB: return MTLPixelFormatR8Unorm;
    case MTLPixelFormatRG8Unorm_sRGB: return MTLPixelFormatRG8Unorm;
    default: return f;
    }
}

/* The texture a draw samples: a view over [base, max] levels so Metal's
   LOD 0 is GL's base level, with the format's swizzle (luminance, alpha...)
   composed with GL_TEXTURE_SWIZZLE_*, and sRGB decoding optional. The view
   is kept until any of that changes. */
static id<MTLTexture> sampling_view(struct glm_context *ctx, struct glm_texture *t, bool skip_decode, bool *mipmapped)
{
    *mipmapped = false;
    if (!complete(t) || !glm_backend_texture_ensure_storage(ctx, t)) return nil;
    id<MTLTexture> storage = (__bridge id<MTLTexture>)t->backend;
    NSUInteger base = (NSUInteger)MAX(t->base_level, 0);
    NSUInteger last = storage.mipmapLevelCount - 1;
    if (t->max_level >= 0 && (NSUInteger)t->max_level < last) last = (NSUInteger)t->max_level;
    if (base > last) return nil;
    *mipmapped = last > base;
    struct glm_format_info info;
    glm_format_lookup(t->levels[0][base].internal_format, &info);
    /* Depth reads as GL_DEPTH_TEXTURE_MODE says in the legacy profile, as
       red in the core one (Apple's: build/probes/depthmode.c). */
    if (info.depth) {
        static const uint8_t luminance[4] = {0, 0, 0, 5}, intensity[4] = {0, 0, 0, 0}, alpha[4] = {4, 4, 4, 0},
                             red[4] = {0, 4, 4, 5};
        GLenum mode = ctx->profile == GLM_PROFILE_CORE ? GL_RED : t->depth_texture_mode;
        memcpy(info.swizzle, mode == GL_INTENSITY ? intensity : mode == GL_ALPHA ? alpha : mode == GL_RED ? red : luminance, 4);
    }
    uint8_t swizzle[4];
    for (int c = 0; c < 4; ++c) swizzle[c] = t->swizzle[c] < 4 ? info.swizzle[t->swizzle[c]] : t->swizzle[c];
    MTLPixelFormat format = skip_decode ? linear_format(storage.pixelFormat) : storage.pixelFormat;
    bool identity = swizzle[0] == 0 && swizzle[1] == 1 && swizzle[2] == 2 && swizzle[3] == 3;
    if (base == 0 && last == storage.mipmapLevelCount - 1 && identity && format == storage.pixelFormat) return storage;
    uint32_t key = (uint32_t)base | (uint32_t)last << 5 | (uint32_t)(swizzle[0] | swizzle[1] << 3 | swizzle[2] << 6 |
                                                                       swizzle[3] << 9) << 10 |
                   (uint32_t)(format != storage.pixelFormat) << 22;
    if (t->view && t->view_storage == t->backend && t->view_key == key) return (__bridge id<MTLTexture>)t->view;
    static const MTLTextureSwizzle map[] = {MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue,
                                            MTLTextureSwizzleAlpha, MTLTextureSwizzleZero, MTLTextureSwizzleOne};
    NSUInteger slices = storage.textureType == MTLTextureTypeCube || storage.textureType == MTLTextureTypeCubeArray
                            ? 6 * storage.arrayLength : storage.arrayLength;
    id<MTLTexture> view = [storage newTextureViewWithPixelFormat:format textureType:storage.textureType
                                                          levels:NSMakeRange(base, last - base + 1)
                                                          slices:NSMakeRange(0, slices)
                                                         swizzle:MTLTextureSwizzleChannelsMake(map[swizzle[0]], map[swizzle[1]],
                                                                                               map[swizzle[2]], map[swizzle[3]])];
    glm_backend_release(t->view);
    t->view = (__bridge_retained void *)view;
    t->view_storage = t->backend;
    t->view_key = key;
    return view;
}

/* Convert the selected sampling view, not the full storage: LOD zero,
   swizzle and sRGB behavior must match the native cube sampler. */
static id<MTLTexture> cube_sampling_array_view(struct glm_texture *t, id<MTLTexture> source)
{
    if (!source || (source.textureType != MTLTextureTypeCube && source.textureType != MTLTextureTypeCubeArray)) return nil;
    if (t && t->cube_array_view && t->cube_array_source == (__bridge void *)source)
        return (__bridge id<MTLTexture>)t->cube_array_view;
    id<MTLTexture> view = [source newTextureViewWithPixelFormat:source.pixelFormat textureType:MTLTextureType2DArray
                                                       levels:NSMakeRange(0, source.mipmapLevelCount)
                                                       slices:NSMakeRange(0, source.textureType == MTLTextureTypeCubeArray ? 6 * source.arrayLength : 6)
                                                      swizzle:source.swizzle];
    if (t) {
        glm_backend_release(t->cube_array_view);
        t->cube_array_view = (__bridge_retained void *)view;
        t->cube_array_source = (__bridge void *)source;
    }
    return view;
}

static bool emulated_border(const struct glm_texture *t, const struct glm_sampler_state *s, int binding, float *color,
                            float *clamp);
static void sampling_lod_metadata(float (*rows)[4], int slot, const struct glm_sampler_state *state, bool mipmapped);

static int target_binding(int target_code)
{
    static const int bindings[] = {-1, GLM_TEX_1D, GLM_TEX_2D, GLM_TEX_3D, GLM_TEX_CUBE, GLM_TEX_RECT};
    return target_code >= 0 && target_code <= 5 ? bindings[target_code] : -1;
}

bool glm_unit_border(struct glm_context *ctx, int unit, int target_code, float *color, float *clamp)
{
    int target = 0;
    struct glm_texture *t = glm_unit_texture(ctx, unit, &target);
    const struct glm_sampler_state *state = t ? unit_sampler_state(ctx, unit, t) : NULL;
    return state && emulated_border(t, state, target_binding(target_code), color, clamp);
}

/* Fixed-function textures; units with border emulation (key->unit[].border)
   get their GLMBorder colour and GL_CLAMP axes in `border_data`. */
bool glm_bind_textures(struct glm_context *ctx, id<MTLRenderCommandEncoder> encoder, const struct glm_ff_key *key,
                       float (*border_data)[4], float (*lod_bias)[4])
{
    for (int u = 0; u < GLM_FF_UNITS; ++u) {
        if (!key->unit[u].target) continue;
        int target = 0;
        struct glm_texture *t = glm_unit_texture(ctx, u, &target);
        bool mipmapped = false;
        const struct glm_sampler_state *s = unit_sampler_state(ctx, u, t);
        id<MTLTexture> view = t ? sampling_view(ctx, t, s && s->skip_srgb_decode, &mipmapped) : nil;
        static const MTLTextureType types[] = {MTLTextureType2D, MTLTextureType2D, MTLTextureType2D, MTLTextureType3D,
                                               MTLTextureTypeCube, MTLTextureType2D};
        if (!view) view = incomplete_texture(types[key->unit[u].target]);
        struct glm_sampler_state fallback = {GL_NEAREST, GL_NEAREST, GL_REPEAT, GL_REPEAT, GL_REPEAT, GL_NONE, GL_LEQUAL, {0}, 0, 0, 0, 1};
        if (!s) s = &fallback;
        if (key->unit[u].target == 4) {
            id<MTLTexture> array_view = cube_sampling_array_view(t, view);
            if (!array_view) return false;
            bool cube_seams = native_cube_seams(ctx, s, true);
            lod_bias[u][0] = s->lod_bias + ctx->state.units[u].env.lod_bias;
            lod_bias[u][1] = ctx->state.texture_cube_seamless || cube_seams ? 0 : 1;
            lod_bias[u][2] = s->wrap_s != GL_CLAMP_TO_EDGE || s->wrap_t != GL_CLAMP_TO_EDGE;
            sampling_lod_metadata(lod_bias, u, s, mipmapped);
            glm_encoder_texture(ctx, 2, (__bridge void *)array_view, NULL, (NSUInteger)(64 + u));
        }
        if (key->unit[u].border && s && glm_unit_border(ctx, u, key->unit[u].target, border_data[u], border_data[32 + u])) {
            struct glm_sampler_state emulated = *s;
            GLenum *wraps[3] = {&emulated.wrap_s, &emulated.wrap_t, &emulated.wrap_r};
            for (int a = 0; a < 3; ++a)
                if (*wraps[a] == GL_CLAMP) *wraps[a] = GL_CLAMP_TO_BORDER;
            memset(emulated.border_color, 0, sizeof emulated.border_color);
            id<MTLSamplerState> black = sampler_for(&emulated, ctx->state.units[u].env.lod_bias, mipmapped,
                                                    key->unit[u].target == 5, native_cube_seams(ctx, &emulated, key->unit[u].target == 4));
            for (int c = 0; c < 4; ++c) emulated.border_color[c] = 1;
            id<MTLSamplerState> white = sampler_for(&emulated, ctx->state.units[u].env.lod_bias, mipmapped,
                                                    key->unit[u].target == 5, native_cube_seams(ctx, &emulated, key->unit[u].target == 4));
            glm_encoder_texture(ctx, 2, (__bridge void *)view, (__bridge void *)black, (NSUInteger)u);
            glm_encoder_texture(ctx, 2, (__bridge void *)view, (__bridge void *)white, (NSUInteger)(u + GLM_FF_UNITS));
            continue;
        }
        id<MTLSamplerState> sampler = sampler_for(s ? s : &fallback, ctx->state.units[u].env.lod_bias, mipmapped,
                                                  key->unit[u].target == 5, native_cube_seams(ctx, s, key->unit[u].target == 4));
        glm_encoder_texture(ctx, 2, (__bridge void *)view, (__bridge void *)sampler, (NSUInteger)u);
    }
    return true;
}

/* ---- buffer textures (GL 3.1) --------------------------------------------- */

GLM_EXPORT void glTexBuffer(GLenum target, GLenum internal_format, GLuint buffer)
{
    GLM_CONTEXT(ctx);
    if (target != GL_TEXTURE_BUFFER) return glm_error(ctx, GL_INVALID_ENUM);
    struct glm_format_info info;
    if (!glm_format_lookup(internal_format, &info) || info.compressed || info.depth) return glm_error(ctx, GL_INVALID_ENUM);
    if (buffer && !glm_buffer_get(ctx, buffer)) return glm_error(ctx, GL_INVALID_OPERATION);
    struct glm_texture *t = glm_texture_bound(ctx, GL_TEXTURE_BUFFER);
    if (!t) return;
    t->buffer = buffer;
    t->buffer_format = internal_format;
    glm_backend_release(t->view);
    t->view = NULL;
}
GLM_EXPORT void glTexBufferARB(GLenum target, GLenum internal_format, GLuint buffer) { glTexBuffer(target, internal_format, buffer); }
GLM_EXPORT void glTexBufferEXT(GLenum target, GLenum internal_format, GLuint buffer) { glTexBuffer(target, internal_format, buffer); }

/* A texture-buffer view of the buffer object's current storage. */
static id<MTLTexture> buffer_texture_view(struct glm_context *ctx, struct glm_texture *t)
{
    struct glm_buffer *buffer = t->buffer ? glm_buffer_get(ctx, t->buffer) : NULL;
    if (!buffer || !buffer->backend) return nil;
    if (t->view && t->view_storage == buffer->backend) return (__bridge id<MTLTexture>)t->view;
    struct glm_format_info info;
    if (!glm_format_lookup(t->buffer_format, &info)) return nil;
    id<MTLBuffer> storage = (__bridge id<MTLBuffer>)buffer->backend;
    NSUInteger texel = (NSUInteger)info.bytes_per_pixel;
    NSUInteger width = texel ? MIN((NSUInteger)buffer->size / texel, (NSUInteger)(1u << 26)) : 0;
    if (!width) return nil;
    MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:(MTLPixelFormat)info.metal_format
                                                                                    width:width
                                                                          resourceOptions:storage.resourceOptions
                                                                                    usage:MTLTextureUsageShaderRead];
    id<MTLTexture> view = [storage newTextureWithDescriptor:d offset:0 bytesPerRow:width * texel];
    glm_backend_release(t->view);
    t->view = (__bridge_retained void *)view;
    t->view_storage = buffer->backend;
    return view;
}

static id<MTLTexture> empty_buffer_texture(void)
{
    static id<MTLTexture> texture;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        id<MTLBuffer> storage = glm_new_shared_buffer(256, NULL);
        MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                        width:1
                                                                              resourceOptions:storage.resourceOptions
                                                                                        usage:MTLTextureUsageShaderRead];
        texture = [storage newTextureWithDescriptor:d offset:0 bytesPerRow:256];
    });
    return texture;
}

/* The texture binding a GLSL sampler type reads. */
int glm_sampler_binding(uint32_t type)
{
    switch (type) {
    case GL_SAMPLER_1D: case GL_SAMPLER_1D_SHADOW: case GL_INT_SAMPLER_1D: case GL_UNSIGNED_INT_SAMPLER_1D: return GLM_TEX_1D;
    case GL_SAMPLER_3D: case GL_INT_SAMPLER_3D: case GL_UNSIGNED_INT_SAMPLER_3D: return GLM_TEX_3D;
    case GL_SAMPLER_CUBE: case GL_SAMPLER_CUBE_SHADOW: case GL_INT_SAMPLER_CUBE: case GL_UNSIGNED_INT_SAMPLER_CUBE:
        return GLM_TEX_CUBE;
    case GL_SAMPLER_2D_RECT: case GL_SAMPLER_2D_RECT_SHADOW: case GL_INT_SAMPLER_2D_RECT: case GL_UNSIGNED_INT_SAMPLER_2D_RECT:
        return GLM_TEX_RECT;
    case GL_SAMPLER_2D_ARRAY: case GL_SAMPLER_2D_ARRAY_SHADOW: case GL_INT_SAMPLER_2D_ARRAY: case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
        return GLM_TEX_2D_ARRAY;
    case GL_SAMPLER_1D_ARRAY: case GL_SAMPLER_1D_ARRAY_SHADOW: case GL_INT_SAMPLER_1D_ARRAY: case GL_UNSIGNED_INT_SAMPLER_1D_ARRAY:
        return GLM_TEX_1D_ARRAY;
    case GL_SAMPLER_BUFFER: case GL_INT_SAMPLER_BUFFER: case GL_UNSIGNED_INT_SAMPLER_BUFFER: return GLM_TEX_BUFFER;
    case GL_SAMPLER_2D_MULTISAMPLE: case GL_INT_SAMPLER_2D_MULTISAMPLE: case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE:
        return GLM_TEX_2D_MULTISAMPLE;
    case GL_SAMPLER_2D_MULTISAMPLE_ARRAY: case GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
    case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        return GLM_TEX_2D_MULTISAMPLE_ARRAY;
    case 0x900C: case 0x900D: case 0x900E: case 0x900F: return GLM_TEX_CUBE_ARRAY; /* GL_*SAMPLER_CUBE_MAP_ARRAY* */
    default: return GLM_TEX_2D;
    }
}

/* Program samplers: slot i of the linked program reads the unit its
   sampler uniform names, through the binding for the sampler's type. */
/* Border emulation (see glm_compile_request.border_samplers) for sampling
   `t` through `s`: GL_CLAMP with linear filtering (the coordinate is
   clamped, the filter reaches the border), or a border colour Metal's
   samplers cannot give. Fills the border colour as sampled (converted to
   the texture's base format and swizzled) and the GL_CLAMP axes. */
static bool emulated_border(const struct glm_texture *t, const struct glm_sampler_state *s, int binding, float *color,
                            float *clamp)
{
    if (!t || s->compare_mode != GL_NONE) return false;
    if (binding != GLM_TEX_1D && binding != GLM_TEX_2D && binding != GLM_TEX_3D && binding != GLM_TEX_RECT &&
        binding != GLM_TEX_2D_ARRAY && binding != GLM_TEX_1D_ARRAY)
        return false;
    int base = MAX(t->base_level, 0);
    struct glm_format_info info;
    if (base >= GLM_MAX_LEVELS || !glm_format_lookup(t->levels[0][base].internal_format, &info) || info.integer) return false;
    bool linear = s->mag_filter == GL_LINEAR || (s->min_filter != GL_NEAREST && s->min_filter != GL_NEAREST_MIPMAP_NEAREST);
    const GLenum wraps[3] = {s->wrap_s, s->wrap_t, s->wrap_r};
    int axes = binding == GLM_TEX_3D ? 3 : binding == GLM_TEX_1D || binding == GLM_TEX_1D_ARRAY ? 1 : 2;
    bool any = false;
    for (int a = 0; a < 4; ++a) clamp[a] = 0;
    for (int a = 0; a < axes; ++a) {
        if (wraps[a] == GL_CLAMP && linear) {
            clamp[a] = binding == GLM_TEX_RECT ? 0 : 1;
            any = true;
        } else if (wraps[a] == GL_CLAMP_TO_BORDER && !metal_border(s->border_color)) {
            any = true;
        }
    }
    if (!any) return false;
    float storage[4] = {0, 0, 0, 1}, format[4];
    bool filled[4] = {false};
    for (int c = 0; c < 4; ++c) {
        int channel = info.swizzle[c];
        if (channel < 4 && !filled[channel]) {
            storage[channel] = s->border_color[c];
            filled[channel] = true;
        }
    }
    for (int c = 0; c < 4; ++c) format[c] = info.swizzle[c] < 4 ? storage[info.swizzle[c]] : info.swizzle[c] == 4 ? 0 : 1;
    for (int c = 0; c < 4; ++c) color[c] = t->swizzle[c] < 4 ? format[t->swizzle[c]] : t->swizzle[c] == 4 ? 0 : 1;
    return true;
}

static int program_sampler_binding(const struct glm_uniform_info *uniform)
{
    int arb_unit, arb_target;
    if (sscanf(uniform->name, "glm_arb_tex%d_%d", &arb_unit, &arb_target) == 2) return -1;
    return glm_sampler_binding(uniform->type);
}

uint32_t glm_program_border_mask(struct glm_context *ctx, const struct glm_program *program, float (*data)[4])
{
    uint32_t mask = 0;
    for (int i = 0; i < program->result.uniform_count; ++i) {
        const struct glm_uniform_info *uniform = &program->result.uniforms[i];
        if (uniform->sampler_slot < 0 || uniform->sampler_slot >= 32 || uniform->offset >= 0 || uniform->array_size != 1)
            continue;
        int binding = program_sampler_binding(uniform);
        if (binding < 0) continue;
        int unit = program->sampler_units[uniform->sampler_slot];
        if (unit < 0 || unit >= GLM_MAX_TEXTURE_UNITS) unit = 0;
        GLuint name = ctx->state.units[unit].bound[binding];
        struct glm_texture *t = name ? glm_texture_get(ctx, name) : NULL;
        const struct glm_sampler_state *state = unit_sampler_state(ctx, unit, t);
        if (state && emulated_border(t, state, binding, data[uniform->sampler_slot], data[32 + uniform->sampler_slot]))
            mask |= 1u << uniform->sampler_slot;
    }
    /* Metal has 16 sampler slots per stage. */
    if (program->result.sampler_count + __builtin_popcount(mask) > 16) return 0;
    return mask;
}

/* Metadata follows the effective sampler, including sampler-object overrides.
   The first 64 rows retain shader bias and cube flags. */
static void sampling_lod_metadata(float (*rows)[4], int slot, const struct glm_sampler_state *state, bool mipmapped)
{
    if (slot < 0 || slot >= GLM_LOD_METADATA_BASE) return;
    float *metadata = rows[GLM_LOD_METADATA_BASE + slot];
    metadata[0] = state->min_lod;
    metadata[1] = state->max_lod;
    metadata[2] = MAX(1.0f, MIN(state->max_anisotropy, 16.0f));
    GLenum min = state->min_filter;
    metadata[3] = !mipmapped || !mipmapped_filter(min) ? 0 :
                  (min == GL_NEAREST_MIPMAP_NEAREST || min == GL_LINEAR_MIPMAP_NEAREST) ? 1 : 2;
}

bool glm_bind_program_textures(struct glm_context *ctx, id<MTLRenderCommandEncoder> encoder,
                               const struct glm_program *program, unsigned stages, uint32_t border_mask,
                               float (*lod_bias)[4])
{
    for (int i = 0; i < program->result.uniform_count; ++i) {
        const struct glm_uniform_info *uniform = &program->result.uniforms[i];
        if (uniform->sampler_slot < 0 || uniform->offset >= 0) continue;
        int slot;
        MTLTextureType type;
        /* Translated ARB programs name their samplers by unit and target. */
        int arb_unit, arb_target;
        if (sscanf(uniform->name, "glm_arb_tex%d_%d", &arb_unit, &arb_target) == 2) {
            static const int slots[] = {GLM_TEX_1D, GLM_TEX_2D, GLM_TEX_3D, GLM_TEX_CUBE, GLM_TEX_RECT, GLM_TEX_1D,
                                        GLM_TEX_2D, GLM_TEX_RECT};
            slot = slots[arb_target & 7];
            type = slot == GLM_TEX_3D ? MTLTextureType3D : slot == GLM_TEX_CUBE ? MTLTextureTypeCube : MTLTextureType2D;
            if (uniform->sampler_slot < 64) ((struct glm_program *)program)->sampler_units[uniform->sampler_slot] = arb_unit;
        } else {
            slot = glm_sampler_binding(uniform->type);
            static const MTLTextureType types[GLM_TEX_SLOTS] = {
                [GLM_TEX_1D] = MTLTextureType2D, [GLM_TEX_2D] = MTLTextureType2D, [GLM_TEX_3D] = MTLTextureType3D,
                [GLM_TEX_CUBE] = MTLTextureTypeCube, [GLM_TEX_RECT] = MTLTextureType2D,
                [GLM_TEX_2D_ARRAY] = MTLTextureType2DArray, [GLM_TEX_BUFFER] = MTLTextureTypeTextureBuffer,
                [GLM_TEX_2D_MULTISAMPLE] = MTLTextureType2DMultisample,
                [GLM_TEX_2D_MULTISAMPLE_ARRAY] = MTLTextureType2DMultisampleArray,
                [GLM_TEX_1D_ARRAY] = MTLTextureType2DArray, [GLM_TEX_CUBE_ARRAY] = MTLTextureTypeCubeArray};
            type = types[slot];
        }
        for (int e = 0; e < uniform->array_size; ++e) {
            int metal_slot = uniform->sampler_slot + e;
            if (metal_slot >= 64) break;
            int unit = program->sampler_units[metal_slot];
            if (unit < 0 || unit >= GLM_MAX_TEXTURE_UNITS) unit = 0;
            GLuint name = ctx->state.units[unit].bound[slot];
            struct glm_texture *t = name ? glm_texture_get(ctx, name) : default_texture(ctx, slot);
            bool mipmapped = false;
            const struct glm_sampler_state *state = unit_sampler_state(ctx, unit, t);
            id<MTLTexture> view;
            if (slot == GLM_TEX_BUFFER) {
                view = t ? buffer_texture_view(ctx, t) : nil;
                if (!view) view = empty_buffer_texture();
            } else {
                view = t ? sampling_view(ctx, t, state && state->skip_srgb_decode, &mipmapped) : nil;
                if (!view) view = incomplete_texture(type);
            }
            struct glm_sampler_state fallback = {GL_NEAREST, GL_NEAREST, GL_REPEAT, GL_REPEAT, GL_REPEAT, GL_NONE, GL_LEQUAL, {0}, 0, 0, 0, 1};
            if (!state) state = &fallback;
            lod_bias[metal_slot][0] = state->lod_bias;
            sampling_lod_metadata(lod_bias, metal_slot, state, mipmapped);
            bool cube_seams = native_cube_seams(ctx, state, uniform->type == GL_SAMPLER_CUBE ||
                                                           uniform->type == GL_SAMPLER_CUBE_SHADOW ||
                                                           uniform->type == GL_SAMPLER_CUBE_MAP_ARRAY);
            if ((uniform->type == GL_SAMPLER_CUBE_SHADOW || uniform->type == GL_SAMPLER_CUBE || uniform->type == GL_SAMPLER_CUBE_MAP_ARRAY) && metal_slot < 32) {
                id<MTLTexture> array_view = cube_sampling_array_view(t, view);
                if (!array_view) return false;
                lod_bias[metal_slot][1] = ctx->state.texture_cube_seamless || cube_seams ? 0 : 1;
                lod_bias[metal_slot][2] = state->wrap_s != GL_CLAMP_TO_EDGE || state->wrap_t != GL_CLAMP_TO_EDGE;
                glm_encoder_texture(ctx, stages, (__bridge void *)array_view, NULL, (NSUInteger)(64 + metal_slot));
            }
            float bias = ctx->state.units[unit].env.lod_bias;
            if (metal_slot < 32 && (border_mask >> metal_slot) & 1) {
                /* Border emulation: transparent black here, opaque white
                   on the glm_bw_ sampler after the program's own. */
                struct glm_sampler_state emulated = *state;
                GLenum *wraps[3] = {&emulated.wrap_s, &emulated.wrap_t, &emulated.wrap_r};
                for (int a = 0; a < 3; ++a)
                    if (*wraps[a] == GL_CLAMP) *wraps[a] = GL_CLAMP_TO_BORDER;
                memset(emulated.border_color, 0, sizeof emulated.border_color);
                id<MTLSamplerState> black = sampler_for(&emulated, bias, mipmapped, slot == GLM_TEX_RECT, cube_seams);
                for (int c = 0; c < 4; ++c) emulated.border_color[c] = 1;
                id<MTLSamplerState> white = sampler_for(&emulated, bias, mipmapped, slot == GLM_TEX_RECT, cube_seams);
                int white_slot = program->result.sampler_count + __builtin_popcount(border_mask & ((1u << metal_slot) - 1));
                if (white_slot < 64) lod_bias[white_slot][0] = state->lod_bias;
                glm_encoder_texture(ctx, stages, (__bridge void *)view, (__bridge void *)black, (NSUInteger)metal_slot);
                glm_encoder_texture(ctx, stages, (__bridge void *)view, (__bridge void *)white, (NSUInteger)white_slot);
                continue;
            }
            id<MTLSamplerState> sampler = sampler_for(state, bias, mipmapped, slot == GLM_TEX_RECT, cube_seams);
            glm_encoder_texture(ctx, stages, (__bridge void *)view, (__bridge void *)sampler, (NSUInteger)metal_slot);
        }
    }
    return true;
}

/* A geometry kernel's samplers: units from the program's sampler of the same name. */
bool glm_bind_compute_textures(struct glm_context *ctx, id<MTLComputeCommandEncoder> encoder,
                               const struct glm_compile_result *kernel, const struct glm_program *source)
{
    float lod_bias[GLM_LOD_ROW_COUNT][4] = {{0}};
    for (int i = 0; i < kernel->uniform_count; ++i) {
        const struct glm_uniform_info *uniform = &kernel->uniforms[i];
        if (uniform->sampler_slot < 0 || uniform->offset >= 0) continue;
        int source_slot = -1;
        for (int k = 0; k < source->result.uniform_count; ++k)
            if (!strcmp(source->result.uniforms[k].name, uniform->name) && source->result.uniforms[k].sampler_slot >= 0 &&
                source->result.uniforms[k].sampler_slot < GLM_LOD_METADATA_BASE)
                source_slot = source->result.uniforms[k].sampler_slot;
        int slot = uniform->type == GL_SAMPLER_3D ? GLM_TEX_3D :
                   (uniform->type == GL_SAMPLER_CUBE || uniform->type == GL_SAMPLER_CUBE_SHADOW) ? GLM_TEX_CUBE
                 : uniform->type == GL_SAMPLER_2D_ARRAY ? GLM_TEX_2D_ARRAY : GLM_TEX_2D;
        for (int element = 0; element < uniform->array_size; ++element) {
            int metal_slot = uniform->sampler_slot + element;
            if (metal_slot >= GLM_LOD_METADATA_BASE) break;
            int unit = source_slot >= 0 && source_slot + element < GLM_LOD_METADATA_BASE
                         ? source->sampler_units[source_slot + element] : 0;
            if (unit < 0 || unit >= GLM_MAX_TEXTURE_UNITS) unit = 0;
            GLuint name = ctx->state.units[unit].bound[slot];
            struct glm_texture *t = name ? glm_texture_get(ctx, name) : default_texture(ctx, slot);
            bool mipmapped = false;
            const struct glm_sampler_state *state = unit_sampler_state(ctx, unit, t);
            id<MTLTexture> view = t ? sampling_view(ctx, t, state && state->skip_srgb_decode, &mipmapped) : nil;
            if (!view) view = incomplete_texture(slot == GLM_TEX_3D ? MTLTextureType3D : slot == GLM_TEX_CUBE ? MTLTextureTypeCube
                                                 : slot == GLM_TEX_2D_ARRAY ? MTLTextureType2DArray : MTLTextureType2D);
            struct glm_sampler_state fallback = {GL_NEAREST, GL_NEAREST, GL_REPEAT, GL_REPEAT, GL_REPEAT, GL_NONE, GL_LEQUAL, {0}, 0, 0, 0, 1};
            if (!state) state = &fallback;
            lod_bias[metal_slot][0] = state->lod_bias;
            sampling_lod_metadata(lod_bias, metal_slot, state, mipmapped);
            bool cube_seams = native_cube_seams(ctx, state, uniform->type == GL_SAMPLER_CUBE ||
                                                           uniform->type == GL_SAMPLER_CUBE_SHADOW ||
                                                           uniform->type == GL_SAMPLER_CUBE_MAP_ARRAY);
            if ((uniform->type == GL_SAMPLER_CUBE_SHADOW || uniform->type == GL_SAMPLER_CUBE || uniform->type == GL_SAMPLER_CUBE_MAP_ARRAY) && metal_slot < 32) {
                id<MTLTexture> array_view = cube_sampling_array_view(t, view);
                if (!array_view) return false;
                lod_bias[metal_slot][1] = ctx->state.texture_cube_seamless || cube_seams ? 0 : 1;
                lod_bias[metal_slot][2] = state->wrap_s != GL_CLAMP_TO_EDGE || state->wrap_t != GL_CLAMP_TO_EDGE;
                [encoder setTexture:array_view atIndex:(NSUInteger)(64 + metal_slot)];
            }
            [encoder setTexture:view atIndex:(NSUInteger)metal_slot];
            [encoder setSamplerState:sampler_for(state, 0, mipmapped, false, cube_seams) atIndex:(NSUInteger)metal_slot];
        }
    }
    if (kernel->sampler_count) [encoder setBytes:lod_bias length:sizeof lod_bias atIndex:GLM_SLOT_LOD_BIAS];
    return true;
}
