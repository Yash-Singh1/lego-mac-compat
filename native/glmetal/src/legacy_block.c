/* Fills the GLMLegacy uniform block (legacy_block.h) from context state. */
#include "legacy_block.h"

#include "glm_internal.h"
#include "shader_compiler.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

GLM_HIDDEN void glm_legacy_block_build(struct glm_context *ctx, uint32_t parts, struct glm_legacy_block *b);
GLM_HIDDEN bool glm_legacy_parts_equal(const struct glm_legacy_block *a, const struct glm_legacy_block *b, uint32_t parts);

static void transpose(float *out, const float *m)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[c * 4 + r] = m[r * 4 + c];
}

/* matrix, inverse, transpose, inverse transpose */
static void variants(const float *m, float *plain, float *inverse, float *transposed, float *inverse_transposed)
{
    memcpy(plain, m, 16 * sizeof(float));
    if (!glm_mat4_invert(inverse, m)) memcpy(inverse, m, 16 * sizeof(float));
    transpose(transposed, m);
    transpose(inverse_transposed, inverse);
}

static void copy4(float *dst, const float *src) { memcpy(dst, src, 4 * sizeof(float)); }

static void product(float *out, const float *a, const float *b)
{
    for (int i = 0; i < 4; ++i) out[i] = a[i] * b[i];
}

/* The matrix variants of the last block this thread built: inversions are
   the expensive part and matrices rarely change between draws. */
static __thread struct {
    bool valid;
    float mv[16], p[16], texture[8][16];
    struct glm_legacy_block block;
} matrix_cache;

/* Byte ranges of each part (enum glm_legacy_part). */
static const struct { size_t begin, end; } part_ranges[] = {
    {offsetof(struct glm_legacy_block, modelview), offsetof(struct glm_legacy_block, texture)},
    {offsetof(struct glm_legacy_block, texture), offsetof(struct glm_legacy_block, normal_matrix)},
    {offsetof(struct glm_legacy_block, normal_matrix), offsetof(struct glm_legacy_block, clip_plane)},
    {offsetof(struct glm_legacy_block, clip_plane), offsetof(struct glm_legacy_block, texture_env_color)},
    {offsetof(struct glm_legacy_block, texture_env_color), offsetof(struct glm_legacy_block, eye_plane_s)},
    {offsetof(struct glm_legacy_block, eye_plane_s), offsetof(struct glm_legacy_block, light_model_ambient)},
    {offsetof(struct glm_legacy_block, light_model_ambient), offsetof(struct glm_legacy_block, fog_color)},
    {offsetof(struct glm_legacy_block, fog_color), offsetof(struct glm_legacy_block, depth_near)},
    {offsetof(struct glm_legacy_block, depth_near), offsetof(struct glm_legacy_block, point_size)},
    {offsetof(struct glm_legacy_block, point_size), offsetof(struct glm_legacy_block, normal_scale)},
    {offsetof(struct glm_legacy_block, two_side), offsetof(struct glm_legacy_block, pad2)},
};

bool glm_legacy_parts_equal(const struct glm_legacy_block *a, const struct glm_legacy_block *b, uint32_t parts)
{
    for (unsigned i = 0; i < sizeof part_ranges / sizeof part_ranges[0]; ++i)
        if ((parts & (1u << i)) && memcmp((const uint8_t *)a + part_ranges[i].begin, (const uint8_t *)b + part_ranges[i].begin,
                                          part_ranges[i].end - part_ranges[i].begin))
            return false;
    if ((parts & GLM_LEGACY_NORMAL) && a->normal_scale != b->normal_scale) return false;
    return true;
}

/* Fills the parts of `b` named by `parts`; the rest is left as it was. */
void glm_legacy_block_build(struct glm_context *ctx, uint32_t parts, struct glm_legacy_block *b)
{
    const struct glm_state *s = &ctx->state;
    struct glm_legacy_block *c = &matrix_cache.block;
    if (parts & (GLM_LEGACY_MATRICES | GLM_LEGACY_NORMAL)) {
        const float *mv = s->modelview.m[s->modelview.depth], *p = s->projection.m[s->projection.depth];
        bool mv_same = matrix_cache.valid && !memcmp(mv, matrix_cache.mv, sizeof matrix_cache.mv);
        bool p_same = matrix_cache.valid && !memcmp(p, matrix_cache.p, sizeof matrix_cache.p);
        if (!mv_same) {
            variants(mv, c->modelview, c->modelview_inverse, c->modelview_transpose, c->modelview_inverse_transpose);
            memcpy(matrix_cache.mv, mv, sizeof matrix_cache.mv);
        }
        if (!p_same) {
            variants(p, c->projection, c->projection_inverse, c->projection_transpose, c->projection_inverse_transpose);
            memcpy(matrix_cache.p, p, sizeof matrix_cache.p);
        }
        if (!mv_same || !p_same || !matrix_cache.valid) {
            float mvp[16];
            glm_mat4_multiply(mvp, p, mv);
            variants(mvp, c->mvp, c->mvp_inverse, c->mvp_transpose, c->mvp_inverse_transpose);
        }
        if (!matrix_cache.valid) {
            for (int t = 0; t < 8; ++t) memset(matrix_cache.texture[t], 0xff, sizeof matrix_cache.texture[t]);
            matrix_cache.valid = true;
        }
        if (parts & GLM_LEGACY_MATRICES) memcpy(b, c, offsetof(struct glm_legacy_block, texture));
        if (parts & GLM_LEGACY_NORMAL) {
            /* gl_NormalMatrix: transpose of the inverse of the upper 3x3, as columns. */
            for (int col = 0; col < 3; ++col) {
                for (int r = 0; r < 3; ++r) b->normal_matrix[col][r] = c->modelview_inverse[r * 4 + col];
                b->normal_matrix[col][3] = 0;
            }
            const float *inv = c->modelview_inverse;
            float row = sqrtf(inv[2] * inv[2] + inv[6] * inv[6] + inv[10] * inv[10]);
            b->normal_scale = row > 0 ? 1.0f / row : 1.0f;
        }
    }
    if (parts & GLM_LEGACY_TEXTURE) {
        for (int t = 0; t < 8; ++t) {
            const float *m = s->texture[t].m[s->texture[t].depth];
            if (memcmp(m, matrix_cache.texture[t], sizeof matrix_cache.texture[t])) {
                variants(m, c->texture[t], c->texture_inverse[t], c->texture_transpose[t], c->texture_inverse_transpose[t]);
                memcpy(matrix_cache.texture[t], m, sizeof matrix_cache.texture[t]);
            }
        }
        memcpy((uint8_t *)b + part_ranges[1].begin, (uint8_t *)c + part_ranges[1].begin, part_ranges[1].end - part_ranges[1].begin);
    }
    if (parts & GLM_LEGACY_CLIP)
        for (int i = 0; i < 8; ++i) {
            for (int k = 0; k < 4; ++k) b->clip_plane[i][k] = (float)s->clip_planes[i][k];
            b->clip_plane_enabled[i / 4][i % 4] = s->clip_plane_enabled[i] ? 1.0f : 0.0f;
        }
    if (parts & GLM_LEGACY_TEXENV)
        for (int t = 0; t < 8; ++t) copy4(b->texture_env_color[t], s->units[t].env.color);
    if (parts & GLM_LEGACY_TEXGEN)
        for (int t = 0; t < 8; ++t) {
            copy4(b->eye_plane_s[t], s->units[t].gen[0].eye_plane);
            copy4(b->eye_plane_t[t], s->units[t].gen[1].eye_plane);
            copy4(b->eye_plane_r[t], s->units[t].gen[2].eye_plane);
            copy4(b->eye_plane_q[t], s->units[t].gen[3].eye_plane);
            copy4(b->object_plane_s[t], s->units[t].gen[0].object_plane);
            copy4(b->object_plane_t[t], s->units[t].gen[1].object_plane);
            copy4(b->object_plane_r[t], s->units[t].gen[2].object_plane);
            copy4(b->object_plane_q[t], s->units[t].gen[3].object_plane);
        }
    if (parts & GLM_LEGACY_LIGHTS) {
        copy4(b->light_model_ambient, s->light_model_ambient);
        struct glm_legacy_material *materials[2] = {&b->front_material, &b->back_material};
        float *scene[2] = {b->front_scene_color, b->back_scene_color};
        for (int f = 0; f < 2; ++f) {
            const struct glm_material *m = &s->material[f];
            copy4(materials[f]->emission, m->emission);
            copy4(materials[f]->ambient, m->ambient);
            copy4(materials[f]->diffuse, m->diffuse);
            copy4(materials[f]->specular, m->specular);
            materials[f]->shininess = m->shininess;
            /* GL 2.1 2.20.3: ecm + acm * acs */
            for (int k = 0; k < 4; ++k) scene[f][k] = m->emission[k] + m->ambient[k] * s->light_model_ambient[k];
            scene[f][3] = m->diffuse[3];
        }
        for (int i = 0; i < 8; ++i) {
            const struct glm_light *l = &s->lights[i];
            struct glm_legacy_light *out = &b->lights[i];
            copy4(out->ambient, l->ambient);
            copy4(out->diffuse, l->diffuse);
            copy4(out->specular, l->specular);
            copy4(out->position, l->position);
            /* Half vector for an infinite viewer: normalize(P + (0,0,1)). */
            float h[3] = {l->position[0], l->position[1], l->position[2]};
            float len = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
            if (len > 0) for (int k = 0; k < 3; ++k) h[k] /= len;
            h[2] += 1.0f;
            len = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
            for (int k = 0; k < 3; ++k) out->half_vector[k] = len > 0 ? h[k] / len : 0;
            out->half_vector[3] = 0;
            memcpy(out->spot_direction, l->spot_direction, sizeof out->spot_direction);
            out->spot_exponent = l->spot_exponent;
            out->spot_cutoff = l->spot_cutoff;
            out->spot_cos_cutoff = l->spot_cutoff == 180 ? -1.0f : cosf(l->spot_cutoff * (float)M_PI / 180.0f);
            out->constant_attenuation = l->attenuation[0];
            out->linear_attenuation = l->attenuation[1];
            out->quadratic_attenuation = l->attenuation[2];
            for (int f = 0; f < 2; ++f) {
                struct glm_legacy_light_product *lp = f ? &b->back_products[i] : &b->front_products[i];
                product(lp->ambient, s->material[f].ambient, l->ambient);
                product(lp->diffuse, s->material[f].diffuse, l->diffuse);
                product(lp->specular, s->material[f].specular, l->specular);
            }
        }
    }
    if (parts & GLM_LEGACY_FOG) {
        copy4(b->fog_color, s->fog_color);
        b->fog_density = s->fog_density;
        b->fog_start = s->fog_start;
        b->fog_end = s->fog_end;
        b->fog_scale = s->fog_end != s->fog_start ? 1.0f / (s->fog_end - s->fog_start) : 0;
    }
    if (parts & GLM_LEGACY_DEPTH) {
        b->depth_near = (float)s->depth_range[0];
        b->depth_far = (float)s->depth_range[1];
        b->depth_diff = (float)(s->depth_range[1] - s->depth_range[0]);
    }
    if (parts & GLM_LEGACY_TWO_SIDE) b->two_side = s->vertex_program_two_side ? 1.0f : 0.0f;
    if (parts & GLM_LEGACY_POINT) {
        b->point_size = s->point_size;
        b->point_size_min = ctx->point_size_min;
        b->point_size_max = ctx->point_size_max;
        b->point_fade_threshold = ctx->point_fade_threshold;
        b->point_distance_constant = ctx->point_distance[0];
        b->point_distance_linear = ctx->point_distance[1];
        b->point_distance_quadratic = ctx->point_distance[2];
    }
}
