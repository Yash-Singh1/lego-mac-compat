/* Fixed-function pipeline as generated Metal shaders. */
#ifndef GLM_FFGEN_H
#define GLM_FFGEN_H

#include "glm_internal.h"
#include "shader_compiler.h"

enum { GLM_FF_UNITS = 8 };

/* Everything the generated code depends on. Two draws with equal keys use
   the same MTLFunctions; values that vary without changing the code (colours,
   matrices, positions) live in struct glm_ff_uniforms. */
struct glm_ff_key {
    uint8_t lighting, two_side, local_viewer, separate_specular, normalize, rescale_normal;
    uint8_t color_material;           /* 0 none, else GL_* mode */
    uint8_t color_material_face;      /* 1 front, 2 back, 3 both */
    uint8_t light_enabled;            /* bitmask */
    uint8_t light_positional;         /* bitmask: w != 0 */
    uint8_t light_spot;               /* bitmask: cutoff != 180 */
    uint8_t fog;                      /* 0 off, 1 linear, 2 exp, 3 exp2 */
    uint8_t fog_from_coord;
    uint8_t alpha_func;               /* GL_NEVER..GL_ALWAYS - GL_NEVER, 7 = always */
    uint8_t clip_planes;              /* bitmask */
    uint8_t color_attachments;        /* number of fragment outputs */
    uint8_t texgen_needs_eye, texgen_needs_normal;
    uint8_t all_outputs;              /* vertex writes every legacy varying (non-FF fragment stage) */
    uint8_t front_only;               /* fragment ignores back colours (non-FF vertex stage) */
    uint8_t wide_line_expansion;      /* streamed fog.yz contain a window-space line offset */
    uint8_t logic_op;                 /* GL_COLOR_LOGIC_OP: op - GL_CLEAR + 1, applied to the fetched framebuffer */
    struct {
        uint8_t target;               /* 0 none, 1 1D, 2 2D, 3 3D, 4 cube, 5 rect */
        uint8_t gen[4];               /* 0 off, 1 object, 2 eye, 3 sphere, 4 reflection, 5 normal */
        uint8_t shadow;               /* depth compare */
        uint16_t env_mode;            /* GL_MODULATE ... GL_COMBINE */
        uint16_t combine_rgb, combine_alpha;
        uint16_t src_rgb[3], src_alpha[3], op_rgb[3], op_alpha[3];
        uint8_t rgb_scale, alpha_scale; /* 1, 2, 4 */
        uint8_t format_base;          /* 0 rgba, 1 rgb, 2 alpha, 3 luminance, 4 lum_alpha, 5 intensity */
        uint8_t border;               /* border colour / GL_CLAMP emulated (white sampler at unit + 8, GLMBorder) */
    } unit[GLM_FF_UNITS];
};

/* Matches `struct FF` in the generated shaders (all float4 / float4x4). */
struct glm_ff_uniforms {
    float mvp[16], modelview[16], projection[16], normal_matrix[16];
    float texture_matrix[GLM_FF_UNITS][16];
    float clip_planes[GLM_MAX_CLIP_PLANES][4];
    float light_ambient[GLM_MAX_LIGHTS][4], light_diffuse[GLM_MAX_LIGHTS][4], light_specular[GLM_MAX_LIGHTS][4];
    float light_position[GLM_MAX_LIGHTS][4];
    float light_spot[GLM_MAX_LIGHTS][4];        /* xyz direction, w cos(cutoff) */
    float light_attenuation[GLM_MAX_LIGHTS][4]; /* constant, linear, quadratic, spot exponent */
    float material_ambient[2][4], material_diffuse[2][4], material_specular[2][4], material_emission[2][4];
    float material_shininess[4]; /* x front, y back */
    float scene_ambient[4];
    float texgen_object[GLM_FF_UNITS][4][4], texgen_eye[GLM_FF_UNITS][4][4];
    float texenv_color[GLM_FF_UNITS][4];
    float fog_color[4];
    float fog_params[4];  /* density, start, end, 1/(end-start) */
    float alpha_ref[4];
    float point_size[4];
    float rescale[4];     /* x: rescale-normal factor */
};

/* `ff_vertex`/`ff_fragment`: which stages the fixed-function code
   provides; the other comes from a program. */
GLM_HIDDEN void glm_ff_key_build(struct glm_context *ctx, int color_attachments, bool ff_vertex, bool ff_fragment,
                                 struct glm_ff_key *key);
/* Packs the uniforms `key`'s shaders read (at most sizeof(struct
   glm_ff_uniforms)); returns the byte count. */
GLM_HIDDEN size_t glm_ff_uniforms_pack(struct glm_context *ctx, const struct glm_ff_key *key, float *out);
/* Returns malloc'd MSL defining `ff_vertex` and `ff_fragment`. */
GLM_HIDDEN char *glm_ff_generate(const struct glm_ff_key *key);

#endif
