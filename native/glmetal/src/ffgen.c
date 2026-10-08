/* Fixed-function vertex and fragment processing (GL 2.1 sections 2.10-2.14,
 * 3.8.13-3.11) generated as MSL for the current state. */
#include "ffgen.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- key and uniforms ---------------------------------------------------- */

static uint8_t gen_mode(const struct glm_texgen *g)
{
    if (!g->enabled) return 0;
    switch (g->mode) {
    case GL_OBJECT_LINEAR: return 1;
    case GL_EYE_LINEAR: return 2;
    case GL_SPHERE_MAP: return 3;
    case GL_REFLECTION_MAP: return 4;
    case GL_NORMAL_MAP: return 5;
    default: return 0;
    }
}

/* The texture environment's base format (GL 2.1 table 3.20) from the
   format's sampling swizzle: 1 RGB (R and RG too), 2 alpha, 3 luminance
   (depth), 4 luminance alpha, 5 intensity, 0 RGBA. */
static uint8_t base_format_class(GLenum internal)
{
    struct glm_format_info info;
    if (!glm_format_lookup(internal, &info)) return 0;
    const uint8_t *w = info.swizzle;
    if (info.depth) return 3;
    if (w[0] == 4 && w[1] == 4 && w[2] == 4) return 2;
    if (w[0] == w[1] && w[1] == w[2]) return w[3] == 5 ? 3 : w[3] == w[0] ? 5 : 4;
    return w[3] == 5 ? 1 : 0;
}

GLM_HIDDEN struct glm_texture *glm_unit_texture(struct glm_context *ctx, int unit, int *target_code);
GLM_HIDDEN bool glm_unit_border(struct glm_context *ctx, int unit, int target_code, float *color, float *clamp);
GLM_HIDDEN extern int glm_custom_borders;

void glm_ff_key_build(struct glm_context *ctx, int color_attachments, bool ff_vertex, bool ff_fragment,
                      struct glm_ff_key *k)
{
    const struct glm_state *s = &ctx->state;
    memset(k, 0, sizeof *k);
    k->all_outputs = !ff_fragment;
    k->front_only = !ff_vertex && !s->vertex_program_two_side;
    k->wide_line_expansion = ff_vertex && ctx->wide_line_expansion;
    k->color_attachments = (uint8_t)(color_attachments ? color_attachments : 1);
    if (ff_fragment && s->color_logic_op && s->logic_op != GL_COPY) k->logic_op = (uint8_t)(s->logic_op - GL_CLEAR + 1);
    if (s->lighting) {
        k->lighting = 1;
        k->two_side = s->light_model_two_side;
        k->local_viewer = s->light_model_local_viewer;
        k->separate_specular = s->light_model_separate_specular;
        for (int i = 0; i < GLM_MAX_LIGHTS; ++i) {
            const struct glm_light *l = &s->lights[i];
            if (!l->enabled) continue;
            k->light_enabled |= (uint8_t)(1 << i);
            if (l->position[3] != 0) k->light_positional |= (uint8_t)(1 << i);
            if (l->spot_cutoff != 180) k->light_spot |= (uint8_t)(1 << i);
        }
        if (s->color_material) {
            /* 1 emission, 2 ambient, 3 diffuse, 4 specular, 5 ambient and diffuse. */
            switch (s->color_material_mode) {
            case GL_EMISSION: k->color_material = 1; break;
            case GL_AMBIENT: k->color_material = 2; break;
            case GL_DIFFUSE: k->color_material = 3; break;
            case GL_SPECULAR: k->color_material = 4; break;
            default: k->color_material = 5; break;
            }
            k->color_material_face = s->color_material_face == GL_FRONT ? 1 : s->color_material_face == GL_BACK ? 2 : 3;
        }
    }
    k->normalize = s->normalize;
    k->rescale_normal = s->rescale_normal;
    if (s->fog) {
        k->fog = s->fog_mode == GL_LINEAR ? 1 : s->fog_mode == GL_EXP ? 2 : 3;
    }
    k->fog_from_coord = (s->fog || k->all_outputs) && s->fog_coord_source == GL_FOG_COORD;
    k->alpha_func = s->alpha_test ? (uint8_t)(s->alpha_func - GL_NEVER) : 7;
    for (int i = 0; i < GLM_MAX_CLIP_PLANES; ++i)
        if (!ctx->user_clip_preprocessed && s->clip_plane_enabled[i]) k->clip_planes |= (uint8_t)(1 << i);
    for (int u = 0; u < GLM_FF_UNITS; ++u) {
        int target = 0;
        struct glm_texture *texture = ff_fragment ? glm_unit_texture(ctx, u, &target) : NULL;
        const struct glm_texture_unit *unit = &s->units[u];
        if (texture) k->unit[u].target = (uint8_t)target;
        if (texture || k->all_outputs)
            for (int c = 0; c < 4; ++c) {
                k->unit[u].gen[c] = gen_mode(&unit->gen[c]);
                if (k->unit[u].gen[c] == 2) k->texgen_needs_eye = 1;
                if (k->unit[u].gen[c] >= 3) k->texgen_needs_eye = k->texgen_needs_normal = 1;
            }
        if (!texture) continue;
        const struct glm_texenv *e = &unit->env;
        k->unit[u].env_mode = (uint16_t)e->mode;
        if (e->mode == GL_COMBINE) {
            k->unit[u].combine_rgb = (uint16_t)e->combine_rgb;
            k->unit[u].combine_alpha = (uint16_t)e->combine_alpha;
            for (int i = 0; i < 3; ++i) {
                k->unit[u].src_rgb[i] = (uint16_t)e->source_rgb[i];
                k->unit[u].src_alpha[i] = (uint16_t)e->source_alpha[i];
                k->unit[u].op_rgb[i] = (uint16_t)e->operand_rgb[i];
                k->unit[u].op_alpha[i] = (uint16_t)e->operand_alpha[i];
            }
            k->unit[u].rgb_scale = (uint8_t)e->rgb_scale;
            k->unit[u].alpha_scale = (uint8_t)e->alpha_scale;
        }
        int face = target == 4 ? 0 : 0;
        int base = texture->base_level < GLM_MAX_LEVELS ? texture->base_level : 0;
        k->unit[u].format_base = base_format_class(texture->levels[face][base].internal_format);
        k->unit[u].shadow = texture->sampler.compare_mode == GL_COMPARE_R_TO_TEXTURE;
        float color[4], clamp[4];
        if (__atomic_load_n(&glm_custom_borders, __ATOMIC_RELAXED) && !k->unit[u].shadow)
            k->unit[u].border = glm_unit_border(ctx, u, target, color, clamp);
    }
}

static void copy4(float *dst, const float *src) { memcpy(dst, src, 4 * sizeof(float)); }

/* Which uniforms a key's shaders read. The FF struct and the packed
   uniform data are both laid out from this, in the same order. */
struct ff_layout {
    int textures, clip, lights, texgen, texenv;
    bool normal, rescale, lighting, fog, alpha;
};

static void ff_layout_of(const struct glm_ff_key *k, struct ff_layout *l)
{
    memset(l, 0, sizeof *l);
    for (int u = 0; u < GLM_FF_UNITS; ++u) {
        bool outputs = k->unit[u].target || k->all_outputs;
        if (outputs) l->textures = u + 1;
        if (k->unit[u].target) l->texenv = u + 1;
        for (int c = 0; c < 4; ++c)
            if (outputs && (k->unit[u].gen[c] == 1 || k->unit[u].gen[c] == 2)) l->texgen = u + 1;
    }
    for (int i = 0; i < 8; ++i)
        if (k->clip_planes & (1 << i)) l->clip = i + 1;
    l->lighting = k->lighting;
    if (k->lighting)
        for (int i = 0; i < GLM_MAX_LIGHTS; ++i)
            if (k->light_enabled & (1 << i)) l->lights = i + 1;
    l->normal = k->lighting || k->texgen_needs_normal;
    l->rescale = l->normal && k->rescale_normal && !k->normalize;
    l->fog = k->fog != 0;
    l->alpha = k->alpha_func != 7;
}

size_t glm_ff_uniforms_pack(struct glm_context *ctx, const struct glm_ff_key *k, float *out)
{
    struct ff_layout l;
    ff_layout_of(k, &l);
    const struct glm_state *s = &ctx->state;
    float *o = out;
    const float *mv = s->modelview.m[s->modelview.depth], *p = s->projection.m[s->projection.depth];
    glm_mat4_multiply(o, p, mv);
    o += 16;
    memcpy(o, mv, 16 * sizeof(float));
    o += 16;
    if (l.normal || l.rescale) {
        /* Normal matrix: inverse transpose of the modelview's upper 3x3.
           The inverse is memoized: the modelview rarely changes. */
        static __thread float last_mv[16], last_inverse[16];
        static __thread bool last_valid;
        float inverse[16];
        if (last_valid && !memcmp(mv, last_mv, sizeof last_mv)) {
            memcpy(inverse, last_inverse, sizeof inverse);
        } else {
            glm_mat4_invert(inverse, mv);
            memcpy(last_mv, mv, sizeof last_mv);
            memcpy(last_inverse, inverse, sizeof last_inverse);
            last_valid = true;
        }
        if (l.normal) {
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) o[c * 4 + r] = inverse[r * 4 + c];
            o[3] = o[7] = o[11] = o[12] = o[13] = o[14] = 0;
            o[15] = 1;
            o += 16;
        }
        if (l.rescale) {
            /* GL 2.1 2.11.3: rescale factor from the inverse's third row. */
            float row = sqrtf(inverse[2] * inverse[2] + inverse[6] * inverse[6] + inverse[10] * inverse[10]);
            o[0] = row > 0 ? 1.0f / row : 1.0f;
            o[1] = o[2] = o[3] = 0;
            o += 4;
        }
    }
    for (int t = 0; t < l.textures; ++t, o += 16) memcpy(o, s->texture[t].m[s->texture[t].depth], 16 * sizeof(float));
    for (int i = 0; i < l.clip; ++i, o += 4)
        for (int c = 0; c < 4; ++c) o[c] = (float)s->clip_planes[i][c];
    if (l.lights) {
        int n = l.lights;
        for (int i = 0; i < n; ++i) copy4(o + 4 * i, s->lights[i].ambient);
        o += 4 * n;
        for (int i = 0; i < n; ++i) copy4(o + 4 * i, s->lights[i].diffuse);
        o += 4 * n;
        for (int i = 0; i < n; ++i) copy4(o + 4 * i, s->lights[i].specular);
        o += 4 * n;
        for (int i = 0; i < n; ++i) copy4(o + 4 * i, s->lights[i].position);
        o += 4 * n;
        for (int i = 0; i < n; ++i) {
            const struct glm_light *light = &s->lights[i];
            o[4 * i + 0] = light->spot_direction[0];
            o[4 * i + 1] = light->spot_direction[1];
            o[4 * i + 2] = light->spot_direction[2];
            o[4 * i + 3] = light->spot_cutoff == 180 ? -1.0f : cosf(light->spot_cutoff * (float)M_PI / 180.0f);
        }
        o += 4 * n;
        for (int i = 0; i < n; ++i) {
            const struct glm_light *light = &s->lights[i];
            o[4 * i + 0] = light->attenuation[0];
            o[4 * i + 1] = light->attenuation[1];
            o[4 * i + 2] = light->attenuation[2];
            o[4 * i + 3] = light->spot_exponent;
        }
        o += 4 * n;
    }
    if (l.lighting) {
        for (int f = 0; f < 2; ++f) copy4(o + 4 * f, s->material[f].ambient);
        o += 8;
        for (int f = 0; f < 2; ++f) copy4(o + 4 * f, s->material[f].diffuse);
        o += 8;
        for (int f = 0; f < 2; ++f) copy4(o + 4 * f, s->material[f].specular);
        o += 8;
        for (int f = 0; f < 2; ++f) copy4(o + 4 * f, s->material[f].emission);
        o += 8;
        o[0] = s->material[0].shininess;
        o[1] = s->material[1].shininess;
        o[2] = o[3] = 0;
        o += 4;
        copy4(o, s->light_model_ambient);
        o += 4;
    }
    for (int t = 0; t < l.texgen; ++t)
        for (int c = 0; c < 4; ++c, o += 4) copy4(o, s->units[t].gen[c].object_plane);
    for (int t = 0; t < l.texgen; ++t)
        for (int c = 0; c < 4; ++c, o += 4) copy4(o, s->units[t].gen[c].eye_plane);
    for (int t = 0; t < l.texenv; ++t, o += 4) copy4(o, s->units[t].env.color);
    if (l.fog) {
        copy4(o, s->fog_color);
        o[4] = s->fog_density;
        o[5] = s->fog_start;
        o[6] = s->fog_end;
        o[7] = s->fog_end != s->fog_start ? 1.0f / (s->fog_end - s->fog_start) : 0;
        o += 8;
    }
    if (l.alpha) {
        o[0] = s->alpha_ref;
        o[1] = o[2] = o[3] = 0;
        o += 4;
    }
    o[0] = s->point_size;
    o[1] = o[2] = o[3] = 0;
    o += 4;
    return (size_t)(o - out) * sizeof(float);
}


/* ---- source generation --------------------------------------------------- */

struct text {
    char *data;
    size_t length, capacity;
};

static void emit(struct text *t, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void emit(struct text *t, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char line[2048];
    int n = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (n < 0) return;
    char *large = NULL;
    if ((size_t)n >= sizeof line) {
        large = malloc((size_t)n + 1);
        if (!large) return;
        va_start(args, format);
        vsnprintf(large, (size_t)n + 1, format, args);
        va_end(args);
    }
    if (t->length + (size_t)n + 1 > t->capacity) {
        t->capacity = (t->capacity + (size_t)n + 1) * 2;
        t->data = realloc(t->data, t->capacity);
    }
    memcpy(t->data + t->length, large ? large : line, (size_t)n + 1);
    t->length += (size_t)n;
    free(large);
}

static void emit_uniforms(struct text *t, const struct glm_ff_key *k)
{
    struct ff_layout l;
    ff_layout_of(k, &l);
    emit(t, "struct FF {\n  float4x4 mvp, modelview;\n");
    if (l.normal) emit(t, "  float4x4 normal_matrix;\n");
    if (l.rescale) emit(t, "  float4 rescale;\n");
    if (l.textures) emit(t, "  float4x4 texture_matrix[%d];\n", l.textures);
    if (l.clip) emit(t, "  float4 clip_planes[%d];\n", l.clip);
    if (l.lights)
        emit(t, "  float4 light_ambient[%d], light_diffuse[%d], light_specular[%d], light_position[%d], light_spot[%d],"
                " light_attenuation[%d];\n", l.lights, l.lights, l.lights, l.lights, l.lights, l.lights);
    if (l.lighting)
        emit(t, "  float4 material_ambient[2], material_diffuse[2], material_specular[2], material_emission[2];\n"
                "  float4 material_shininess, scene_ambient;\n");
    if (l.texgen) emit(t, "  float4 texgen_object[%d][4], texgen_eye[%d][4];\n", l.texgen, l.texgen);
    if (l.texenv) emit(t, "  float4 texenv_color[%d];\n", l.texenv);
    if (l.fog) emit(t, "  float4 fog_color, fog_params;\n");
    if (l.alpha) emit(t, "  float4 alpha_ref;\n");
    emit(t, "  float4 point_size;\n};\n");
}

/* Vertex inputs: GLM_MAX_ATTRIBS float4 slots (see vertex.c). */
static void emit_vertex_shader(struct text *t, const struct glm_ff_key *k)
{
    int clip_count = 0;
    for (int i = 0; i < 8; ++i)
        if (k->clip_planes & (1 << i)) clip_count = i + 1;
    emit(t, "struct VIn {\n");
    emit(t, "  float4 position [[attribute(0)]];\n  float4 normal [[attribute(2)]];\n  float4 color [[attribute(3)]];\n"
            "  float4 secondary [[attribute(4)]];\n  float4 fog [[attribute(5)]];\n");
    for (int u = 0; u < GLM_FF_UNITS; ++u) emit(t, "  float4 tex%d [[attribute(%d)]];\n", u, GLM_ATTR_TEX0 + u);
    emit(t, "};\n");
    /* Varyings at the shared legacy locations, so programs can supply
       either stage. */
    static const char varyings[] =
        "  float4 front_color [[user(locn0)]];\n  float4 front_secondary [[user(locn1)]];\n"
        "  float4 back_color [[user(locn2)]];\n  float4 back_secondary [[user(locn3)]];\n"
        "  float fog_coord [[user(locn4)]];\n";
    emit(t, "struct V2F {\n  float4 position [[position]];\n%s  float point_size [[point_size]];\n", varyings);
    for (int u = 0; u < GLM_FF_UNITS; ++u)
        if (k->unit[u].target || k->all_outputs) emit(t, "  float4 tex%d [[user(locn%d)]];\n", u, GLM_VARYING_TEXCOORD0 + u);
    if (clip_count) emit(t, "  float clip [[clip_distance]] [%d];\n", clip_count);
    emit(t, "};\n");
    /* The fragment stage reads the same varyings without clip distances and
       point size, which Metal does not allow in stage_in. */
    emit(t, "struct F2 {\n  float4 position [[position]];\n%s", varyings);
    for (int u = 0; u < GLM_FF_UNITS; ++u)
        if (k->unit[u].target) emit(t, "  float4 tex%d [[user(locn%d)]];\n", u, GLM_VARYING_TEXCOORD0 + u);
    emit(t, "};\n");

    if (k->lighting) {
        /* GL 2.1 equation 2.5, per light. */
        emit(t, "static float4 light_color(constant FF &ff, float3 P, float3 N, float4 ambient_m, float4 diffuse_m,\n"
                "                          float4 specular_m, float shininess, thread float4 &spec_out) {\n"
                "  float4 c = float4(0.0); spec_out = float4(0.0);\n");
        for (int i = 0; i < GLM_MAX_LIGHTS; ++i) {
            if (!(k->light_enabled & (1 << i))) continue;
            bool positional = k->light_positional & (1 << i), spot = k->light_spot & (1 << i);
            emit(t, "  {\n");
            if (positional) {
                emit(t, "    float3 V = ff.light_position[%d].xyz / ff.light_position[%d].w - P; float d = length(V); float3 L = V / d;\n", i, i);
                emit(t, "    float att = 1.0 / (ff.light_attenuation[%d].x + ff.light_attenuation[%d].y * d + ff.light_attenuation[%d].z * d * d);\n", i, i, i);
            } else {
                emit(t, "    float3 L = normalize(ff.light_position[%d].xyz); float att = 1.0;\n", i);
            }
            if (spot) {
                emit(t, "    float sd = dot(-L, normalize(ff.light_spot[%d].xyz));\n"
                        "    att *= sd >= ff.light_spot[%d].w ? pow(max(sd, 0.0), ff.light_attenuation[%d].w) : 0.0;\n", i, i, i);
            }
            emit(t, "    float ndl = max(dot(N, L), 0.0);\n");
            emit(t, "    float3 E = %s;\n", k->local_viewer ? "normalize(-P)" : "float3(0.0, 0.0, 1.0)");
            emit(t, "    float3 H = normalize(L + E); float ndh = max(dot(N, H), 0.0);\n");
            emit(t, "    float spec = ndl > 0.0 ? (shininess == 0.0 ? 1.0 : pow(ndh, shininess)) : 0.0;\n");
            emit(t, "    c += att * (ambient_m * ff.light_ambient[%d] + ndl * diffuse_m * ff.light_diffuse[%d]);\n", i, i);
            emit(t, "    spec_out += att * spec * specular_m * ff.light_specular[%d];\n  }\n", i);
        }
        emit(t, "  return c;\n}\n");
    }

    emit(t, "vertex V2F ff_vertex(VIn in [[stage_in]], constant FF &ff [[buffer(16)]]) {\n  V2F out;\n");
    emit(t, "  float4 eye = ff.modelview * in.position;\n  float4 clip = ff.mvp * in.position;\n");
    bool need_normal = k->lighting || k->texgen_needs_normal;
    if (need_normal) {
        emit(t, "  float3 N = (ff.normal_matrix * float4(in.normal.xyz, 0.0)).xyz;\n");
        if (k->rescale_normal && !k->normalize) emit(t, "  N *= ff.rescale.x;\n");
        if (k->normalize || k->lighting) emit(t, "  N = %s;\n", k->normalize ? "normalize(N)" : "N");
    }
    if (k->lighting) {
        /* Material colours, with colour material substituting the current
           colour for the chosen property. */
        const char *mat[2][4];
        char buffers[2][4][64];
        static const char *const props[] = {"emission", "ambient", "diffuse", "specular"};
        for (int f = 0; f < 2; ++f)
            for (int p = 0; p < 4; ++p) {
                snprintf(buffers[f][p], sizeof buffers[f][p], "ff.material_%s[%d]", props[p], f);
                mat[f][p] = buffers[f][p];
            }
        if (k->color_material) {
            /* mode: 1 emission, 2 ambient, 3 diffuse, 4 specular, 5 ambient+diffuse (GL_AMBIENT_AND_DIFFUSE). */
            static const unsigned modes[] = {0, GL_EMISSION, GL_AMBIENT, GL_DIFFUSE, GL_SPECULAR, GL_AMBIENT_AND_DIFFUSE};
            unsigned mode = modes[k->color_material < 6 ? k->color_material : 5];
            for (int f = 0; f < 2; ++f) {
                if (!(k->color_material_face & (1 << f))) continue;
                if (mode == GL_EMISSION) mat[f][0] = "in.color";
                if (mode == GL_AMBIENT || mode == GL_AMBIENT_AND_DIFFUSE) mat[f][1] = "in.color";
                if (mode == GL_DIFFUSE || mode == GL_AMBIENT_AND_DIFFUSE) mat[f][2] = "in.color";
                if (mode == GL_SPECULAR) mat[f][3] = "in.color";
            }
        }
        for (int f = 0; f < (k->two_side ? 2 : 1); ++f) {
            const char *side = f ? "back" : "front";
            emit(t, "  { float4 spec; float3 n = %s;\n", f ? "-N" : "N");
            emit(t, "    float4 lit = light_color(ff, eye.xyz / eye.w, n, %s, %s, %s, ff.material_shininess.%c, spec);\n",
                 mat[f][1], mat[f][2], mat[f][3], f ? 'y' : 'x');
            emit(t, "    float4 base = %s + %s * ff.scene_ambient + lit;\n", mat[f][0], mat[f][1]);
            if (k->separate_specular) {
                emit(t, "    out.%s_color = float4(saturate(base.rgb), %s.a);\n", side, mat[f][2]);
                emit(t, "    out.%s_secondary = float4(saturate(spec.rgb), 0.0);\n", side);
            } else {
                emit(t, "    out.%s_color = float4(saturate(base.rgb + spec.rgb), %s.a);\n", side, mat[f][2]);
                emit(t, "    out.%s_secondary = float4(0.0);\n", side);
            }
            emit(t, "  }\n");
        }
        if (!k->two_side) emit(t, "  out.back_color = out.front_color; out.back_secondary = out.front_secondary;\n");
    } else {
        emit(t, "  out.front_color = out.back_color = saturate(in.color);\n");
        emit(t, "  out.front_secondary = out.back_secondary = saturate(in.secondary);\n");
    }
    for (int u = 0; u < GLM_FF_UNITS; ++u) {
        if (!k->unit[u].target && !k->all_outputs) continue;
        emit(t, "  { float4 tc = in.tex%d;\n", u);
        bool sphere = false, reflect_needed = false;
        for (int c = 0; c < 4; ++c) {
            if (k->unit[u].gen[c] == 3) sphere = true;
            if (k->unit[u].gen[c] >= 3) reflect_needed = true;
        }
        if (reflect_needed) emit(t, "    float3 u = normalize(eye.xyz); float3 r = reflect(u, N);\n");
        if (sphere) emit(t, "    float m = 2.0 * sqrt(r.x * r.x + r.y * r.y + (r.z + 1.0) * (r.z + 1.0));\n");
        static const char comp[] = "xyzw";
        for (int c = 0; c < 4; ++c) {
            switch (k->unit[u].gen[c]) {
            case 1: emit(t, "    tc.%c = dot(ff.texgen_object[%d][%d], in.position);\n", comp[c], u, c); break;
            /* Eye planes are stored already multiplied by the inverse
               modelview in effect when they were specified. */
            case 2: emit(t, "    tc.%c = dot(ff.texgen_eye[%d][%d], eye);\n", comp[c], u, c); break;
            case 3: emit(t, "    tc.%c = %s;\n", comp[c], c == 0 ? "r.x / m + 0.5" : c == 1 ? "r.y / m + 0.5" : "0.0"); break;
            case 4: emit(t, "    tc.%c = r.%c;\n", comp[c], comp[c < 3 ? c : 2]); break;
            case 5: emit(t, "    tc.%c = N.%c;\n", comp[c], comp[c < 3 ? c : 2]); break;
            }
        }
        emit(t, "    out.tex%d = ff.texture_matrix[%d] * tc; }\n", u, u);
    }
    /* Apple interpolates the signed eye depth and takes its magnitude per
       fragment, so fog does not vanish where a primitive crosses z = 0. */
    if (k->fog || k->all_outputs) emit(t, "  out.fog_coord = %s;\n", k->fog_from_coord ? "in.fog.x" : "-eye.z / eye.w");
    else emit(t, "  out.fog_coord = 0.0;\n");
    /* Clip planes are stored in eye space (transformed when specified). */
    for (int i = 0; i < clip_count; ++i) {
        if (k->clip_planes & (1 << i)) emit(t, "  out.clip[%d] = dot(ff.clip_planes[%d], eye);\n", i, i);
        else emit(t, "  out.clip[%d] = 1.0;\n", i);
    }
    emit(t, "  out.point_size = %s;\n", k->wide_line_expansion ? "1.0" : "ff.point_size.x");
    if (k->wide_line_expansion) emit(t, "  clip.xy += in.fog.yz * clip.w;\n");
    /* GL clip space to Metal: y flipped (framebuffers store GL rows
       top-down), z from [-w, w] to [0, w]. */
    emit(t, "  out.position = float4(clip.x, -clip.y, (clip.z + clip.w) * 0.5, clip.w);\n  return out;\n}\n");
}

static const char *combine_source(uint16_t source, int u, bool alpha)
{
    static char buffer[8][64];
    static int next;
    char *out = buffer[next++ & 7];
    switch (source) {
    case GL_TEXTURE: snprintf(out, 64, "t%d", u); break;
    case GL_CONSTANT: snprintf(out, 64, "ff.texenv_color[%d]", u); break;
    case GL_PRIMARY_COLOR: snprintf(out, 64, "primary"); break;
    case GL_PREVIOUS: snprintf(out, 64, "prev"); break;
    default:
        if (source >= GL_TEXTURE0 && source < GL_TEXTURE0 + 8) snprintf(out, 64, "t%d", source - GL_TEXTURE0);
        else snprintf(out, 64, "prev");
    }
    (void)alpha;
    return out;
}

static void emit_operand(struct text *t, const char *name, const char *src, uint16_t op)
{
    switch (op) {
    case GL_SRC_COLOR: emit(t, "    float4 %s = %s;\n", name, src); break;
    case GL_ONE_MINUS_SRC_COLOR: emit(t, "    float4 %s = 1.0 - %s;\n", name, src); break;
    case GL_SRC_ALPHA: emit(t, "    float4 %s = float4(%s.a);\n", name, src); break;
    case GL_ONE_MINUS_SRC_ALPHA: emit(t, "    float4 %s = float4(1.0 - %s.a);\n", name, src); break;
    default: emit(t, "    float4 %s = %s;\n", name, src); break;
    }
}

static const char *combine_expression(uint16_t function)
{
    switch (function) {
    case GL_REPLACE: return "a0";
    case GL_MODULATE: return "a0 * a1";
    case GL_ADD: return "a0 + a1";
    case GL_ADD_SIGNED: return "a0 + a1 - 0.5";
    case GL_INTERPOLATE: return "a0 * a2 + a1 * (1.0 - a2)";
    case GL_SUBTRACT: return "a0 - a1";
    case GL_DOT3_RGB: case GL_DOT3_RGBA:
        return "float4(4.0 * ((a0.r - 0.5) * (a1.r - 0.5) + (a0.g - 0.5) * (a1.g - 0.5) + (a0.b - 0.5) * (a1.b - 0.5)))";
    default: return "a0 * a1";
    }
}

static void emit_fragment_shader(struct text *t, const struct glm_ff_key *k)
{
    static const char *const texture_types[] = {"", "texture1d<float>", "texture2d<float>", "texture3d<float>",
                                                "texturecube<float>", "texture2d<float>"};
    bool cube = false;
    for (int u = 0; u < GLM_FF_UNITS; ++u) cube |= k->unit[u].target == 4;
    if (cube) emit(t, "%s\n", glm_cube_msl_helpers());
    emit(t, "struct FOut {\n");
    for (int i = 0; i < k->color_attachments; ++i) emit(t, "  float4 c%d [[color(%d)]];\n", i, i);
    emit(t, "};\n");
    bool border = false;
    for (int u = 0; u < GLM_FF_UNITS; ++u) border |= k->unit[u].target && k->unit[u].border;
    if (border) {
        /* Border emulation: the sample with a transparent black border, plus
           the border colour weighted by what an opaque white border adds. */
        emit(t, "struct GLMBorder { float4 color[32]; float4 clamp[32]; };\n");
        emit(t, "static float4 glm_border(float4 a, float4 w, float4 c) { float4 v = mix(a, w, c); "
                "return select(select(v, a, c == 0.0f), w, c == 1.0f); }\n");
    }
    border = false;
    emit(t, "fragment FOut ff_fragment(F2 in [[stage_in]], bool front [[front_facing]], constant FF &ff [[buffer(16)]]");
    if (cube) emit(t, ",\n    constant float4* glm_cube_lod [[buffer(%d)]]", GLM_SLOT_FF_CUBE_LOD);
    /* Logic ops read the framebuffer (Apple GPUs fetch it in the shader). */
    if (k->logic_op)
        for (int i = 0; i < k->color_attachments; ++i) emit(t, ",\n    float4 dst%d [[color(%d)]]", i, i);
    for (int u = 0; u < GLM_FF_UNITS; ++u) {
        if (!k->unit[u].target) continue;
        if (k->unit[u].shadow && (k->unit[u].target == 2 || k->unit[u].target == 5))
            emit(t, ",\n    depth2d<float> tex%d [[texture(%d)]], sampler smp%d [[sampler(%d)]]", u, u, u, u);
        else
            emit(t, ",\n    %s tex%d [[texture(%d)]], sampler smp%d [[sampler(%d)]]", texture_types[k->unit[u].target], u, u, u, u);
        if (k->unit[u].target == 4)
            emit(t, ",\n    texture2d_array<float> glm_cube_faces%d [[texture(%d)]]", u, 64 + u);
        if (k->unit[u].border) {
            emit(t, ",\n    sampler smpw%d [[sampler(%d)]]", u, u + GLM_FF_UNITS);
            border = true;
        }
    }
    if (border) emit(t, ",\n    constant GLMBorder &bd [[buffer(22)]]");
    emit(t, ") {\n");
    /* Metal reports front-facing for the winding it was told is front;
       the backend sets that from GL's glFrontFace. */
    if (k->front_only) {
        emit(t, "  float4 primary = in.front_color;\n  float4 secondary = in.front_secondary;\n");
    } else {
        emit(t, "  float4 primary = front ? in.front_color : in.back_color;\n");
        emit(t, "  float4 secondary = front ? in.front_secondary : in.back_secondary;\n");
    }
    emit(t, "  float4 prev = primary;\n");
    for (int u = 0; u < GLM_FF_UNITS; ++u) {
        if (!k->unit[u].target) continue;
        int target = k->unit[u].target;
        if (k->unit[u].border && target != 4) {
            static const char *const coords[] = {"", "in.tex%d.x / in.tex%d.w", "in.tex%d.xy / in.tex%d.w",
                                                 "in.tex%d.xyz / in.tex%d.w", "", "in.tex%d.xy / in.tex%d.w"};
            static const char *const swizzles[] = {"", ".x", ".xy", ".xyz", "", ".xy"};
            emit(t, "  auto c%d = ", u);
            emit(t, coords[target], u, u);
            emit(t, ";\n  c%d = mix(c%d, clamp(c%d, 0.0, 1.0), bd.clamp[%d]%s);\n", u, u, u, u, swizzles[target]);
            emit(t, "  float4 t%d = glm_border(tex%d.sample(smp%d, c%d), tex%d.sample(smpw%d, c%d), bd.color[%d]);\n", u, u, u,
                 u, u, u, u, u);
            target = 0; /* sampled */
        }
        /* Fixed-function texturing divides by q (GL 2.1 3.8.8). */
        switch (target) {
        case 1: emit(t, "  float4 t%d = tex%d.sample(smp%d, in.tex%d.x / in.tex%d.w);\n", u, u, u, u, u); break;
        case 2:
            if (k->unit[u].shadow)
                emit(t, "  float4 t%d = float4(tex%d.sample_compare(smp%d, in.tex%d.xy / in.tex%d.w, in.tex%d.z / in.tex%d.w));\n",
                     u, u, u, u, u, u, u);
            else emit(t, "  float4 t%d = tex%d.sample(smp%d, in.tex%d.xy / in.tex%d.w);\n", u, u, u, u, u);
            break;
        case 3: emit(t, "  float4 t%d = tex%d.sample(smp%d, in.tex%d.xyz / in.tex%d.w);\n", u, u, u, u, u); break;
        case 4:
            emit(t, "  float4 t%d = glm_cube_float_bias(tex%d, glm_cube_faces%d, smp%d, in.tex%d.xyz, "
                    "glm_cube_lod[%d].y, glm_cube_lod[%d].z, glm_cube_lod[%d], glm_cube_lod[%d].x);\n",
                 u, u, u, u, u, u, u, GLM_LOD_METADATA_BASE + u, u);
            break;
        case 5:
            /* Rectangle samplers take texel coordinates. */
            emit(t, "  float4 t%d = tex%d.sample(smp%d, in.tex%d.xy / in.tex%d.w);\n", u, u, u, u, u);
            break;
        }
        /* Texture environment (GL 2.1 table 3.22/3.23) by base format. */
        int fmt = k->unit[u].format_base;
        uint16_t mode = k->unit[u].env_mode;
        const char *cf = "prev";
        if (mode == GL_COMBINE) {
            const struct glm_ff_key *kk = k;
            emit(t, "  {\n");
            for (int i = 0; i < 3; ++i) {
                char name[8];
                snprintf(name, sizeof name, "r%d", i);
                emit_operand(t, name, combine_source(kk->unit[u].src_rgb[i], u, false), kk->unit[u].op_rgb[i]);
                snprintf(name, sizeof name, "q%d", i);
                emit_operand(t, name, combine_source(kk->unit[u].src_alpha[i], u, true), kk->unit[u].op_alpha[i]);
            }
            emit(t, "    float4 a0 = r0, a1 = r1, a2 = r2;\n    float3 rgb = (%s).rgb;\n", combine_expression(kk->unit[u].combine_rgb));
            emit(t, "    a0 = q0; a1 = q1; a2 = q2;\n    float alpha = (%s).a;\n", combine_expression(kk->unit[u].combine_alpha));
            if (kk->unit[u].combine_rgb == GL_DOT3_RGBA) emit(t, "    alpha = rgb.r;\n");
            emit(t, "    prev = saturate(float4(rgb * %d.0, alpha * %d.0));\n  }\n",
                 kk->unit[u].rgb_scale ? kk->unit[u].rgb_scale : 1, kk->unit[u].alpha_scale ? kk->unit[u].alpha_scale : 1);
            continue;
        }
        char tv[8];
        snprintf(tv, sizeof tv, "t%d", u);
        switch (mode) {
        case GL_REPLACE:
            switch (fmt) {
            case 2: emit(t, "  prev = float4(%s.rgb, %s.a);\n", cf, tv); break;
            case 1: case 3: emit(t, "  prev = float4(%s.rgb, %s.a);\n", tv, cf); break;
            default: emit(t, "  prev = %s;\n", tv); break;
            }
            break;
        case GL_MODULATE:
            switch (fmt) {
            case 2: emit(t, "  prev = float4(%s.rgb, %s.a * %s.a);\n", cf, cf, tv); break;
            case 1: case 3: emit(t, "  prev = float4(%s.rgb * %s.rgb, %s.a);\n", cf, tv, cf); break;
            default: emit(t, "  prev = %s * %s;\n", cf, tv); break;
            }
            break;
        case GL_DECAL:
            if (fmt == 1) emit(t, "  prev = float4(%s.rgb, %s.a);\n", tv, cf);
            else emit(t, "  prev = float4(mix(%s.rgb, %s.rgb, %s.a), %s.a);\n", cf, tv, tv, cf);
            break;
        case GL_BLEND:
            switch (fmt) {
            case 2: emit(t, "  prev = float4(%s.rgb, %s.a * %s.a);\n", cf, cf, tv); break;
            case 5: emit(t, "  prev = mix(%s, ff.texenv_color[%d], %s);\n", cf, u, tv); break;
            case 1: case 3: emit(t, "  prev = float4(mix(%s.rgb, ff.texenv_color[%d].rgb, %s.rgb), %s.a);\n", cf, u, tv, cf); break;
            default: emit(t, "  prev = float4(mix(%s.rgb, ff.texenv_color[%d].rgb, %s.rgb), %s.a * %s.a);\n", cf, u, tv, cf, tv); break;
            }
            break;
        case GL_ADD:
            switch (fmt) {
            case 2: emit(t, "  prev = float4(%s.rgb, %s.a * %s.a);\n", cf, cf, tv); break;
            case 5: emit(t, "  prev = saturate(%s + %s);\n", cf, tv); break;
            case 1: case 3: emit(t, "  prev = float4(saturate(%s.rgb + %s.rgb), %s.a);\n", cf, tv, cf); break;
            default: emit(t, "  prev = float4(saturate(%s.rgb + %s.rgb), %s.a * %s.a);\n", cf, tv, cf, tv); break;
            }
            break;
        default: emit(t, "  prev = %s * %s;\n", cf, tv); break;
        }
    }
    emit(t, "  float4 color = float4(saturate(prev.rgb + secondary.rgb), prev.a);\n");
    if (k->fog) {
        emit(t, "  float z = %s;\n", k->fog_from_coord ? "in.fog_coord" : "abs(in.fog_coord)");
        switch (k->fog) {
        case 1: emit(t, "  float f = (ff.fog_params.z - z) * ff.fog_params.w;\n"); break;
        case 2: emit(t, "  float f = exp(-ff.fog_params.x * z);\n"); break;
        case 3: emit(t, "  float f = exp(-(ff.fog_params.x * z) * (ff.fog_params.x * z));\n"); break;
        }
        emit(t, "  color.rgb = mix(ff.fog_color.rgb, color.rgb, saturate(f));\n");
    }
    static const char *const alpha_tests[] = {"false", "color.a < ff.alpha_ref.x", "color.a == ff.alpha_ref.x",
                                              "color.a <= ff.alpha_ref.x", "color.a > ff.alpha_ref.x",
                                              "color.a != ff.alpha_ref.x", "color.a >= ff.alpha_ref.x", "true"};
    if (k->alpha_func != 7) emit(t, "  if (!(%s)) discard_fragment();\n", alpha_tests[k->alpha_func]);
    emit(t, "  FOut out;\n");
    if (k->logic_op) {
        /* GL 2.1 table 4.5 on the 8-bit values. */
        static const char *const ops[] = {"0u", "s & d", "s & ~d", "s", "~s & d", "d", "s ^ d", "s | d", "~(s | d)",
                                          "~(s ^ d)", "~d", "s | ~d", "~s", "~s | d", "~(s & d)", "~0u"};
        emit(t, "  uint4 s = uint4(round(saturate(color) * 255.0));\n");
        for (int i = 0; i < k->color_attachments; ++i)
            emit(t, "  { uint4 d = uint4(round(saturate(dst%d) * 255.0)); out.c%d = float4((%s) & 255u) / 255.0; }\n", i, i,
                 ops[(k->logic_op - 1) & 15]);
    } else {
        for (int i = 0; i < k->color_attachments; ++i) emit(t, "  out.c%d = color;\n", i);
    }
    emit(t, "  return out;\n}\n");
}

char *glm_ff_generate(const struct glm_ff_key *k)
{
    struct text t = {0};
    emit(&t, "#include <metal_stdlib>\nusing namespace metal;\n#pragma clang fp contract(fast)\n");
    emit_uniforms(&t, k);
    emit_vertex_shader(&t, k);
    emit_fragment_shader(&t, k);
    return t.data;
}
