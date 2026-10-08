/* Legacy GL state as seen by GLSL 1.x built-ins and ARB program state
 * bindings: one std140 uniform block. The GLSL declaration lives in
 * shader_compiler.cpp (glm_legacy_block_glsl); both must stay in step, and
 * shader_compiler.cpp checks the offsets against glslang's reflection. */
#ifndef GLM_LEGACY_BLOCK_H
#define GLM_LEGACY_BLOCK_H

struct glm_legacy_light {
    float ambient[4], diffuse[4], specular[4], position[4], half_vector[4];
    float spot_direction[3], spot_exponent;
    float spot_cutoff, spot_cos_cutoff, constant_attenuation, linear_attenuation;
    float quadratic_attenuation, pad[3];
};

struct glm_legacy_material {
    float emission[4], ambient[4], diffuse[4], specular[4];
    float shininess, pad[3];
};

struct glm_legacy_light_product {
    float ambient[4], diffuse[4], specular[4];
};

/* Order: all 16-byte-aligned members, so std140 adds no hidden padding
   except the explicit pads. */
struct glm_legacy_block {
    float modelview[16], projection[16], mvp[16];
    float modelview_inverse[16], projection_inverse[16], mvp_inverse[16];
    float modelview_transpose[16], projection_transpose[16], mvp_transpose[16];
    float modelview_inverse_transpose[16], projection_inverse_transpose[16], mvp_inverse_transpose[16];
    float texture[8][16], texture_inverse[8][16], texture_transpose[8][16], texture_inverse_transpose[8][16];
    float normal_matrix[3][4]; /* mat3: three vec4 columns */
    float clip_plane[8][4];
    float clip_plane_enabled[2][4]; /* vec4[2], independent of the stored equations */
    float texture_env_color[8][4];
    float eye_plane_s[8][4], eye_plane_t[8][4], eye_plane_r[8][4], eye_plane_q[8][4];
    float object_plane_s[8][4], object_plane_t[8][4], object_plane_r[8][4], object_plane_q[8][4];
    float light_model_ambient[4];
    float front_scene_color[4], back_scene_color[4];
    struct glm_legacy_material front_material, back_material;
    struct glm_legacy_light lights[8];
    struct glm_legacy_light_product front_products[8], back_products[8];
    float fog_color[4];                                   /* glm_Fog (32 bytes) */
    float fog_density, fog_start, fog_end, fog_scale;
    float depth_near, depth_far, depth_diff, pad0;        /* glm_DepthRange (16) */
    float point_size, point_size_min, point_size_max, point_fade_threshold; /* glm_Point (32) */
    float point_distance_constant, point_distance_linear, point_distance_quadratic, pad1;
    float normal_scale, alpha_ref, two_side, pad2;        /* glm_NormalScale, glm_AlphaRef, glm_TwoSide */
};

enum { GLM_ARB_PARAMETERS = 256 };

struct glm_arb_block {
    float env[GLM_ARB_PARAMETERS][4];
    float local[GLM_ARB_PARAMETERS][4];
};

#endif
