/* Fixed-function state: lighting, materials, fog, texture environment,
 * texture coordinate generation and clip planes. Positions, directions and
 * planes are transformed by the modelview matrix current at specification,
 * as GL 2.1 specifies. */
#include "glm_internal.h"

#include <math.h>
#include <string.h>

static void transform_point(const float *m, const float *in, float *out)
{
    float r[4];
    for (int i = 0; i < 4; ++i) r[i] = m[i] * in[0] + m[4 + i] * in[1] + m[8 + i] * in[2] + m[12 + i] * in[3];
    memcpy(out, r, sizeof r);
}

static const float *modelview(struct glm_context *ctx)
{
    return ctx->state.modelview.m[ctx->state.modelview.depth];
}

/* Plane p in object space to eye space: p * inverse(modelview). */
static void transform_plane(struct glm_context *ctx, const double *in, double *out)
{
    float inverse[16];
    glm_mat4_invert(inverse, modelview(ctx));
    for (int c = 0; c < 4; ++c)
        out[c] = in[0] * inverse[c * 4] + in[1] * inverse[c * 4 + 1] + in[2] * inverse[c * 4 + 2] + in[3] * inverse[c * 4 + 3];
}

/* ---- lights -------------------------------------------------------------- */

GLM_EXPORT void glLightfv(GLenum light, GLenum pname, const GLfloat *params)
{
    GLM_CONTEXT(ctx);
    if (light < GL_LIGHT0 || light >= GL_LIGHT0 + GLM_MAX_LIGHTS) return glm_error(ctx, GL_INVALID_ENUM);
    struct glm_light *l = &ctx->state.lights[light - GL_LIGHT0];
    switch (pname) {
    case GL_AMBIENT: memcpy(l->ambient, params, 16); break;
    case GL_DIFFUSE: memcpy(l->diffuse, params, 16); break;
    case GL_SPECULAR: memcpy(l->specular, params, 16); break;
    case GL_POSITION: transform_point(modelview(ctx), params, l->position); break;
    case GL_SPOT_DIRECTION: {
        const float *m = modelview(ctx);
        for (int i = 0; i < 3; ++i) l->spot_direction[i] = m[i] * params[0] + m[4 + i] * params[1] + m[8 + i] * params[2];
        break;
    }
    case GL_SPOT_EXPONENT: l->spot_exponent = params[0]; break;
    case GL_SPOT_CUTOFF: l->spot_cutoff = params[0]; break;
    case GL_CONSTANT_ATTENUATION: l->attenuation[0] = params[0]; break;
    case GL_LINEAR_ATTENUATION: l->attenuation[1] = params[0]; break;
    case GL_QUADRATIC_ATTENUATION: l->attenuation[2] = params[0]; break;
    default: glm_error(ctx, GL_INVALID_ENUM);
    }
}
GLM_EXPORT void glLightf(GLenum light, GLenum pname, GLfloat param) { glLightfv(light, pname, &param); }
GLM_EXPORT void glLighti(GLenum light, GLenum pname, GLint param) { GLfloat f = (GLfloat)param; glLightfv(light, pname, &f); }
GLM_EXPORT void glLightiv(GLenum light, GLenum pname, const GLint *params)
{
    GLfloat f[4];
    int n = pname == GL_AMBIENT || pname == GL_DIFFUSE || pname == GL_SPECULAR || pname == GL_POSITION ? 4 : pname == GL_SPOT_DIRECTION ? 3 : 1;
    for (int i = 0; i < n; ++i) f[i] = (GLfloat)params[i];
    glLightfv(light, pname, f);
}

GLM_EXPORT void glGetLightfv(GLenum light, GLenum pname, GLfloat *params)
{
    GLM_CONTEXT(ctx);
    if (light < GL_LIGHT0 || light >= GL_LIGHT0 + GLM_MAX_LIGHTS) return glm_error(ctx, GL_INVALID_ENUM);
    struct glm_light *l = &ctx->state.lights[light - GL_LIGHT0];
    switch (pname) {
    case GL_AMBIENT: memcpy(params, l->ambient, 16); break;
    case GL_DIFFUSE: memcpy(params, l->diffuse, 16); break;
    case GL_SPECULAR: memcpy(params, l->specular, 16); break;
    case GL_POSITION: memcpy(params, l->position, 16); break;
    case GL_SPOT_DIRECTION: memcpy(params, l->spot_direction, 12); break;
    case GL_SPOT_EXPONENT: *params = l->spot_exponent; break;
    case GL_SPOT_CUTOFF: *params = l->spot_cutoff; break;
    case GL_CONSTANT_ATTENUATION: *params = l->attenuation[0]; break;
    case GL_LINEAR_ATTENUATION: *params = l->attenuation[1]; break;
    case GL_QUADRATIC_ATTENUATION: *params = l->attenuation[2]; break;
    default: glm_error(ctx, GL_INVALID_ENUM);
    }
}

GLM_EXPORT void glLightModelfv(GLenum pname, const GLfloat *params)
{
    GLM_CONTEXT(ctx);
    switch (pname) {
    case GL_LIGHT_MODEL_AMBIENT: memcpy(ctx->state.light_model_ambient, params, 16); break;
    case GL_LIGHT_MODEL_LOCAL_VIEWER: ctx->state.light_model_local_viewer = params[0] != 0; break;
    case GL_LIGHT_MODEL_TWO_SIDE: ctx->state.light_model_two_side = params[0] != 0; break;
    case GL_LIGHT_MODEL_COLOR_CONTROL: ctx->state.light_model_separate_specular = params[0] == GL_SEPARATE_SPECULAR_COLOR; break;
    default: glm_error(ctx, GL_INVALID_ENUM);
    }
}
GLM_EXPORT void glLightModelf(GLenum pname, GLfloat param) { glLightModelfv(pname, &param); }
GLM_EXPORT void glLightModeli(GLenum pname, GLint param) { GLfloat f = (GLfloat)param; glLightModelfv(pname, &f); }
GLM_EXPORT void glLightModeliv(GLenum pname, const GLint *params)
{
    GLfloat f[4] = {0};
    int n = pname == GL_LIGHT_MODEL_AMBIENT ? 4 : 1;
    for (int i = 0; i < n; ++i) f[i] = pname == GL_LIGHT_MODEL_AMBIENT ? (GLfloat)((2.0 * params[i] + 1) / 4294967295.0) : (GLfloat)params[i];
    glLightModelfv(pname, f);
}

/* ---- materials ----------------------------------------------------------- */

/* With GL_COLOR_MATERIAL the tracked material parameters follow the
   current colour; brought up to date whenever they are observed or
   tracking changes (draws read the colour itself). */
GLM_HIDDEN void glm_color_material_sync(struct glm_context *ctx)
{
    struct glm_state *s = &ctx->state;
    if (!s->color_material) return;
    const float *color = s->current[GLM_ATTR_COLOR];
    for (int f = 0; f < 2; ++f) {
        if ((f == 0 && s->color_material_face == GL_BACK) || (f == 1 && s->color_material_face == GL_FRONT)) continue;
        struct glm_material *m = &s->material[f];
        switch (s->color_material_mode) {
        case GL_AMBIENT: memcpy(m->ambient, color, 16); break;
        case GL_DIFFUSE: memcpy(m->diffuse, color, 16); break;
        case GL_AMBIENT_AND_DIFFUSE: memcpy(m->ambient, color, 16); memcpy(m->diffuse, color, 16); break;
        case GL_SPECULAR: memcpy(m->specular, color, 16); break;
        case GL_EMISSION: memcpy(m->emission, color, 16); break;
        }
    }
}

GLM_EXPORT void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params)
{
    GLM_CONTEXT(ctx);
    for (int f = 0; f < 2; ++f) {
        if (f == 0 && face == GL_BACK) continue;
        if (f == 1 && face == GL_FRONT) continue;
        /* Parameters the current colour tracks ignore glMaterial. */
        const struct glm_state *s = &ctx->state;
        bool face_tracked = s->color_material && !((f == 0 && s->color_material_face == GL_BACK) ||
                                                   (f == 1 && s->color_material_face == GL_FRONT));
        GLenum tracked = face_tracked ? s->color_material_mode : 0;
        if (tracked && (pname == tracked || (tracked == GL_AMBIENT_AND_DIFFUSE && (pname == GL_AMBIENT || pname == GL_DIFFUSE)) ||
                        (pname == GL_AMBIENT_AND_DIFFUSE && (tracked == GL_AMBIENT || tracked == GL_DIFFUSE))))
            continue;
        struct glm_material *m = &ctx->state.material[f];
        switch (pname) {
        case GL_AMBIENT: memcpy(m->ambient, params, 16); break;
        case GL_DIFFUSE: memcpy(m->diffuse, params, 16); break;
        case GL_AMBIENT_AND_DIFFUSE: memcpy(m->ambient, params, 16); memcpy(m->diffuse, params, 16); break;
        case GL_SPECULAR: memcpy(m->specular, params, 16); break;
        case GL_EMISSION: memcpy(m->emission, params, 16); break;
        case GL_SHININESS: m->shininess = params[0]; break;
        case GL_COLOR_INDEXES: break;
        default: return glm_error(ctx, GL_INVALID_ENUM);
        }
    }
}
GLM_EXPORT void glMaterialf(GLenum face, GLenum pname, GLfloat param) { glMaterialfv(face, pname, &param); }
GLM_EXPORT void glMateriali(GLenum face, GLenum pname, GLint param) { GLfloat f = (GLfloat)param; glMaterialfv(face, pname, &f); }
GLM_EXPORT void glMaterialiv(GLenum face, GLenum pname, const GLint *params)
{
    GLfloat f[4];
    int n = pname == GL_SHININESS ? 1 : 4;
    for (int i = 0; i < n; ++i) f[i] = pname == GL_SHININESS ? (GLfloat)params[i] : (GLfloat)((2.0 * params[i] + 1) / 4294967295.0);
    glMaterialfv(face, pname, f);
}

/* Colours read as integers map [-1, 1] onto the full GLint range. */
static GLint glm_float_to_int_color(float v) { return (GLint)(fmax(fmin(v, 1.0), -1.0) * 2147483647.0); }

GLM_EXPORT void glGetMaterialfv(GLenum face, GLenum pname, GLfloat *params)
{
    GLM_CONTEXT(ctx);
    if (face != GL_FRONT && face != GL_BACK) return glm_error(ctx, GL_INVALID_ENUM);
    glm_color_material_sync(ctx);
    const struct glm_material *m = &ctx->state.material[face == GL_BACK];
    switch (pname) {
    case GL_AMBIENT: memcpy(params, m->ambient, 16); break;
    case GL_DIFFUSE: memcpy(params, m->diffuse, 16); break;
    case GL_SPECULAR: memcpy(params, m->specular, 16); break;
    case GL_EMISSION: memcpy(params, m->emission, 16); break;
    case GL_SHININESS: *params = m->shininess; break;
    case GL_COLOR_INDEXES: params[0] = 0; params[1] = 1; params[2] = 1; break;
    default: glm_error(ctx, GL_INVALID_ENUM);
    }
}
GLM_EXPORT void glGetMaterialiv(GLenum face, GLenum pname, GLint *params)
{
    GLfloat f[4] = {0};
    glGetMaterialfv(face, pname, f);
    int n = pname == GL_SHININESS ? 1 : pname == GL_COLOR_INDEXES ? 3 : 4;
    for (int i = 0; i < n; ++i) params[i] = n == 4 ? glm_float_to_int_color(f[i]) : (GLint)lroundf(f[i]);
}

GLM_EXPORT void glGetLightiv(GLenum light, GLenum pname, GLint *params)
{
    GLfloat f[4] = {0};
    glGetLightfv(light, pname, f);
    int n = pname == GL_AMBIENT || pname == GL_DIFFUSE || pname == GL_SPECULAR || pname == GL_POSITION ? 4
          : pname == GL_SPOT_DIRECTION ? 3 : 1;
    bool color = pname == GL_AMBIENT || pname == GL_DIFFUSE || pname == GL_SPECULAR;
    for (int i = 0; i < n; ++i) params[i] = color ? glm_float_to_int_color(f[i]) : (GLint)lroundf(f[i]);
}

GLM_EXPORT void glColorMaterial(GLenum face, GLenum mode)
{
    GLM_CONTEXT(ctx);
    glm_color_material_sync(ctx);
    ctx->state.color_material_face = face;
    ctx->state.color_material_mode = mode;
    glm_color_material_sync(ctx);
}

/* ---- fog ----------------------------------------------------------------- */

GLM_EXPORT void glFogfv(GLenum pname, const GLfloat *params)
{
    GLM_CONTEXT(ctx);
    struct glm_state *s = &ctx->state;
    switch (pname) {
    case GL_FOG_MODE: s->fog_mode = (GLenum)params[0]; break;
    case GL_FOG_DENSITY: s->fog_density = params[0]; break;
    case GL_FOG_START: s->fog_start = params[0]; break;
    case GL_FOG_END: s->fog_end = params[0]; break;
    case GL_FOG_COLOR: memcpy(s->fog_color, params, 16); break;
    case GL_FOG_COORD_SRC: s->fog_coord_source = (GLenum)params[0]; break;
    case GL_FOG_INDEX: case GL_FOG_HINT: break;
    default: glm_error(ctx, GL_INVALID_ENUM);
    }
}
GLM_EXPORT void glFogf(GLenum pname, GLfloat param) { glFogfv(pname, &param); }
GLM_EXPORT void glFogi(GLenum pname, GLint param) { GLfloat f = (GLfloat)param; glFogfv(pname, &f); }
GLM_EXPORT void glFogiv(GLenum pname, const GLint *params)
{
    GLfloat f[4];
    if (pname == GL_FOG_COLOR) for (int i = 0; i < 4; ++i) f[i] = (GLfloat)((2.0 * params[i] + 1) / 4294967295.0);
    else f[0] = (GLfloat)params[0];
    glFogfv(pname, f);
}

/* ---- texture environment ------------------------------------------------- */

static struct glm_texenv *active_env(struct glm_context *ctx) { return &ctx->state.units[ctx->state.active_texture - GL_TEXTURE0].env; }

GLM_EXPORT void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params)
{
    GLM_CONTEXT(ctx);
    if (target == GL_TEXTURE_FILTER_CONTROL) {
        if (pname == GL_TEXTURE_LOD_BIAS) active_env(ctx)->lod_bias = params[0];
        return;
    }
    if (target == GL_POINT_SPRITE) return;
    if (target != GL_TEXTURE_ENV) return glm_error(ctx, GL_INVALID_ENUM);
    struct glm_texenv *e = active_env(ctx);
    GLenum v = (GLenum)params[0];
    switch (pname) {
    case GL_TEXTURE_ENV_MODE: e->mode = v; break;
    case GL_TEXTURE_ENV_COLOR: memcpy(e->color, params, 16); break;
    case GL_COMBINE_RGB: e->combine_rgb = v; break;
    case GL_COMBINE_ALPHA: e->combine_alpha = v; break;
    case GL_SOURCE0_RGB: case GL_SOURCE1_RGB: case GL_SOURCE2_RGB: e->source_rgb[pname - GL_SOURCE0_RGB] = v; break;
    case GL_SOURCE0_ALPHA: case GL_SOURCE1_ALPHA: case GL_SOURCE2_ALPHA: e->source_alpha[pname - GL_SOURCE0_ALPHA] = v; break;
    case GL_OPERAND0_RGB: case GL_OPERAND1_RGB: case GL_OPERAND2_RGB: e->operand_rgb[pname - GL_OPERAND0_RGB] = v; break;
    case GL_OPERAND0_ALPHA: case GL_OPERAND1_ALPHA: case GL_OPERAND2_ALPHA: e->operand_alpha[pname - GL_OPERAND0_ALPHA] = v; break;
    case GL_RGB_SCALE: e->rgb_scale = params[0]; break;
    case GL_ALPHA_SCALE: e->alpha_scale = params[0]; break;
    default: glm_error(ctx, GL_INVALID_ENUM);
    }
}
GLM_EXPORT void glTexEnvf(GLenum target, GLenum pname, GLfloat param) { glTexEnvfv(target, pname, &param); }
GLM_EXPORT void glTexEnvi(GLenum target, GLenum pname, GLint param) { GLfloat f = (GLfloat)param; glTexEnvfv(target, pname, &f); }
GLM_EXPORT void glTexEnviv(GLenum target, GLenum pname, const GLint *params)
{
    GLfloat f[4];
    if (pname == GL_TEXTURE_ENV_COLOR) for (int i = 0; i < 4; ++i) f[i] = (GLfloat)((2.0 * params[i] + 1) / 4294967295.0);
    else f[0] = (GLfloat)params[0];
    glTexEnvfv(target, pname, f);
}

GLM_EXPORT void glGetTexEnvfv(GLenum target, GLenum pname, GLfloat *params)
{
    GLM_CONTEXT(ctx);
    const struct glm_texenv *e = active_env(ctx);
    if (target == GL_TEXTURE_FILTER_CONTROL && pname == GL_TEXTURE_LOD_BIAS) {
        *params = e->lod_bias;
        return;
    }
    if (target == GL_POINT_SPRITE && pname == GL_COORD_REPLACE) {
        *params = 0;
        return;
    }
    if (target != GL_TEXTURE_ENV) return glm_error(ctx, GL_INVALID_ENUM);
    switch (pname) {
    case GL_TEXTURE_ENV_MODE: *params = (GLfloat)e->mode; break;
    case GL_TEXTURE_ENV_COLOR: memcpy(params, e->color, 16); break;
    case GL_COMBINE_RGB: *params = (GLfloat)e->combine_rgb; break;
    case GL_COMBINE_ALPHA: *params = (GLfloat)e->combine_alpha; break;
    case GL_SOURCE0_RGB: case GL_SOURCE1_RGB: case GL_SOURCE2_RGB: *params = (GLfloat)e->source_rgb[pname - GL_SOURCE0_RGB]; break;
    case GL_SOURCE0_ALPHA: case GL_SOURCE1_ALPHA: case GL_SOURCE2_ALPHA:
        *params = (GLfloat)e->source_alpha[pname - GL_SOURCE0_ALPHA];
        break;
    case GL_OPERAND0_RGB: case GL_OPERAND1_RGB: case GL_OPERAND2_RGB: *params = (GLfloat)e->operand_rgb[pname - GL_OPERAND0_RGB]; break;
    case GL_OPERAND0_ALPHA: case GL_OPERAND1_ALPHA: case GL_OPERAND2_ALPHA:
        *params = (GLfloat)e->operand_alpha[pname - GL_OPERAND0_ALPHA];
        break;
    case GL_RGB_SCALE: *params = e->rgb_scale; break;
    case GL_ALPHA_SCALE: *params = e->alpha_scale; break;
    default: glm_error(ctx, GL_INVALID_ENUM);
    }
}
GLM_EXPORT void glGetTexEnviv(GLenum target, GLenum pname, GLint *params)
{
    GLfloat f[4] = {0};
    glGetTexEnvfv(target, pname, f);
    if (pname == GL_TEXTURE_ENV_COLOR)
        for (int i = 0; i < 4; ++i) params[i] = glm_float_to_int_color(f[i]);
    else
        params[0] = (GLint)lroundf(f[0]);
}

/* ---- texture coordinate generation --------------------------------------- */

GLM_EXPORT void glTexGenfv(GLenum coord, GLenum pname, const GLfloat *params)
{
    GLM_CONTEXT(ctx);
    if (coord < GL_S || coord > GL_Q) return glm_error(ctx, GL_INVALID_ENUM);
    struct glm_texgen *g = &ctx->state.units[ctx->state.active_texture - GL_TEXTURE0].gen[coord - GL_S];
    switch (pname) {
    case GL_TEXTURE_GEN_MODE: g->mode = (GLenum)params[0]; break;
    case GL_OBJECT_PLANE: memcpy(g->object_plane, params, 16); break;
    case GL_EYE_PLANE: {
        double in[4] = {params[0], params[1], params[2], params[3]}, out[4];
        transform_plane(ctx, in, out);
        for (int i = 0; i < 4; ++i) g->eye_plane[i] = (float)out[i];
        break;
    }
    default: glm_error(ctx, GL_INVALID_ENUM);
    }
}
GLM_EXPORT void glTexGenf(GLenum coord, GLenum pname, GLfloat param) { glTexGenfv(coord, pname, &param); }
GLM_EXPORT void glTexGeni(GLenum coord, GLenum pname, GLint param) { GLfloat f = (GLfloat)param; glTexGenfv(coord, pname, &f); }
GLM_EXPORT void glTexGend(GLenum coord, GLenum pname, GLdouble param) { GLfloat f = (GLfloat)param; glTexGenfv(coord, pname, &f); }
GLM_EXPORT void glTexGeniv(GLenum coord, GLenum pname, const GLint *params)
{
    GLfloat f[4];
    int n = pname == GL_TEXTURE_GEN_MODE ? 1 : 4;
    for (int i = 0; i < n; ++i) f[i] = (GLfloat)params[i];
    glTexGenfv(coord, pname, f);
}
GLM_EXPORT void glTexGendv(GLenum coord, GLenum pname, const GLdouble *params)
{
    GLfloat f[4];
    int n = pname == GL_TEXTURE_GEN_MODE ? 1 : 4;
    for (int i = 0; i < n; ++i) f[i] = (GLfloat)params[i];
    glTexGenfv(coord, pname, f);
}

/* Eye planes are returned in eye coordinates, as stored. */
GLM_EXPORT void glGetTexGenfv(GLenum coord, GLenum pname, GLfloat *params)
{
    GLM_CONTEXT(ctx);
    if (coord < GL_S || coord > GL_Q) return glm_error(ctx, GL_INVALID_ENUM);
    const struct glm_texgen *g = &ctx->state.units[ctx->state.active_texture - GL_TEXTURE0].gen[coord - GL_S];
    switch (pname) {
    case GL_TEXTURE_GEN_MODE: *params = (GLfloat)g->mode; break;
    case GL_OBJECT_PLANE: memcpy(params, g->object_plane, 16); break;
    case GL_EYE_PLANE: memcpy(params, g->eye_plane, 16); break;
    default: glm_error(ctx, GL_INVALID_ENUM);
    }
}
GLM_EXPORT void glGetTexGeniv(GLenum coord, GLenum pname, GLint *params)
{
    GLfloat f[4] = {0};
    glGetTexGenfv(coord, pname, f);
    for (int i = 0; i < (pname == GL_TEXTURE_GEN_MODE ? 1 : 4); ++i) params[i] = (GLint)lroundf(f[i]);
}
GLM_EXPORT void glGetTexGendv(GLenum coord, GLenum pname, GLdouble *params)
{
    GLfloat f[4] = {0};
    glGetTexGenfv(coord, pname, f);
    for (int i = 0; i < (pname == GL_TEXTURE_GEN_MODE ? 1 : 4); ++i) params[i] = f[i];
}

/* ---- clip planes --------------------------------------------------------- */

GLM_EXPORT void glClipPlane(GLenum plane, const GLdouble *equation)
{
    GLM_CONTEXT(ctx);
    if (plane < GL_CLIP_PLANE0 || plane >= GL_CLIP_PLANE0 + GLM_MAX_CLIP_PLANES) return glm_error(ctx, GL_INVALID_ENUM);
    transform_plane(ctx, equation, ctx->state.clip_planes[plane - GL_CLIP_PLANE0]);
}

GLM_EXPORT void glGetClipPlane(GLenum plane, GLdouble *equation)
{
    GLM_CONTEXT(ctx);
    if (plane < GL_CLIP_PLANE0 || plane >= GL_CLIP_PLANE0 + GLM_MAX_CLIP_PLANES) return glm_error(ctx, GL_INVALID_ENUM);
    memcpy(equation, ctx->state.clip_planes[plane - GL_CLIP_PLANE0], 4 * sizeof(double));
}

/* ---- texture units ------------------------------------------------------- */

GLM_EXPORT void glActiveTexture(GLenum texture)
{
    GLM_CONTEXT(ctx);
    if (texture < GL_TEXTURE0 || texture >= GL_TEXTURE0 + GLM_MAX_TEXTURE_UNITS) return glm_error(ctx, GL_INVALID_ENUM);
    ctx->state.active_texture = texture;
}
GLM_EXPORT void glActiveTextureARB(GLenum texture) { glActiveTexture(texture); }
