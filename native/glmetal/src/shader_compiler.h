/* GLSL (and ARB programs translated to GLSL) to Metal Shading Language
 * through glslang and SPIRV-Cross. C interface to shader_compiler.cpp. */
#ifndef GLM_SHADER_COMPILER_H
#define GLM_SHADER_COMPILER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum glm_stage { GLM_STAGE_VERTEX, GLM_STAGE_FRAGMENT, GLM_STAGE_GEOMETRY, GLM_STAGE_TESS_CONTROL,
                 GLM_STAGE_TESS_EVALUATION, GLM_STAGE_COMPUTE, GLM_STAGE_COUNT };

/* Fixed locations of the legacy varyings, shared by the fixed-function,
   ARB and legacy GLSL paths so any vertex stage pairs with any fragment
   stage. */
enum glm_varying_location {
    GLM_VARYING_COLOR0 = 0, GLM_VARYING_COLOR1 = 1, GLM_VARYING_BACK_COLOR0 = 2, GLM_VARYING_BACK_COLOR1 = 3,
    GLM_VARYING_FOG = 4, GLM_VARYING_TEXCOORD0 = 5, /* 8 slots */ GLM_VARYING_USER = 13,
};

/* Parts of the GLMLegacy block (legacy_block.h) a program reads. */
enum glm_legacy_part {
    GLM_LEGACY_MATRICES = 1 << 0,   /* modelview, projection, mvp and their inverse/transposes */
    GLM_LEGACY_TEXTURE = 1 << 1,    /* texture matrices */
    GLM_LEGACY_NORMAL = 1 << 2,     /* normal matrix, normal scale */
    GLM_LEGACY_CLIP = 1 << 3,
    GLM_LEGACY_TEXENV = 1 << 4,
    GLM_LEGACY_TEXGEN = 1 << 5,
    GLM_LEGACY_LIGHTS = 1 << 6,     /* light model, materials, lights, products */
    GLM_LEGACY_FOG = 1 << 7,
    GLM_LEGACY_DEPTH = 1 << 8,
    GLM_LEGACY_POINT = 1 << 9,
    GLM_LEGACY_TWO_SIDE = 1 << 10,  /* GL_VERTEX_PROGRAM_TWO_SIDE, for gl_Color in fragment shaders */
    GLM_LEGACY_ALL = (1 << 11) - 1,
};

/* Metal argument slots. */
enum { GLM_SLOT_XFB = 17 /* 17..20 buffers, 21 GLMXfbInfo; transform feedback draws use the CPU vertex path */,
       GLM_SLOT_GLOBALS = 0, GLM_SLOT_FIRST_UBO = 1, GLM_SLOT_UBO_COUNT = 12, GLM_SLOT_ARB = 13, GLM_SLOT_LEGACY = 14,
       GLM_SLOT_POINT = 15, GLM_SLOT_FF = 16, GLM_SLOT_BORDER = 22 /* GLMBorder */, GLM_SLOT_STREAM = 30 };
/* Programmable stages do not use the fixed-function uniform block. */
/* Bias/cube flags, sampling LOD metadata, then fixed-depth reference flags. */
enum { GLM_SLOT_LOD_BIAS = GLM_SLOT_FF, GLM_LOD_ROW_COUNT = 192, GLM_LOD_METADATA_BASE = 64, GLM_DEPTH_METADATA_BASE = 128 };
/* Fragment-only; vertex/compute slot 25 retains its geometry meaning. */
enum { GLM_SLOT_FF_CUBE_LOD = 25 };

struct glm_name_location {
    const char *name;
    int location;
};

struct glm_compile_request {
    const char *sources[GLM_STAGE_COUNT]; /* NULL for absent stages */
    const struct glm_name_location *attributes;   /* glBindAttribLocation */
    int attribute_count;
    const struct glm_name_location *frag_outputs; /* glBindFragDataLocation */
    int frag_output_count;
    const char *const *feedback_varyings;         /* transform feedback, may be NULL */
    int feedback_count;
    bool feedback_interleaved;
    int tess_output_vertices;                     /* control points per patch, for evaluation-only compiles */
    /* Float vertex inputs (bit per location) fed unnormalized unsigned /
       signed integer arrays: declared as uvec/ivec and converted, so Metal
       can fetch the integer formats. */
    uint32_t uint_inputs, int_inputs;
    /* Fragment-stage samplers (by base sampler slot, NULL for none) whose
       border colour or GL_CLAMP is emulated: each sample is repeated with a
       glm_bw_<name> sampler (the same texture with an opaque white border,
       at slot sampler_count + rank) and combined with GLMBorder's colour. */
    const char *border_samplers[32];
};

struct glm_uniform_info {
    char *name;           /* "tint", "lights[0].color", "w[0]" for arrays */
    uint32_t type;        /* GL_FLOAT_VEC3 ... GL_SAMPLER_2D */
    int array_size;       /* 1 for non-arrays */
    int offset;           /* in the global block, -1 for samplers */
    int array_stride, matrix_stride;
    int sampler_slot;     /* Metal texture/sampler slot for samplers, else -1 */
    int legacy;           /* index of a glm_* legacy state uniform, else -1 */
    int block;            /* block_uniforms only: index in blocks */
    bool row_major;
    bool is_array;        /* declared as an array (named "x[0]" even with one element) */
};

struct glm_block_info {
    char *name;
    int size;
    int slot;             /* Metal buffer slot */
    uint32_t stages;      /* bit per glm_stage referencing it */
};

/* glBindFragDataLocationIndexed: an output binding's location with this
   bit set is index 1 (dual-source blending). */
enum { GLM_OUTPUT_INDEX1 = 0x10000 };

enum { GLM_INPUT_INACTIVE = -1, GLM_INPUT_BUILTIN = -2 };

struct glm_io_info {
    char *name;
    uint32_t type;
    int location;
    int index;            /* fragment outputs: dual-source blend index; vertex inputs: 0, or
                             GLM_INPUT_INACTIVE (unused: not listed by GL) or GLM_INPUT_BUILTIN
                             (gl_VertexID / gl_InstanceID: listed only, not fetched) */
    int array_size;
    bool integer;
};

/* A captured transform feedback varying (glGetTransformFeedbackVarying). */
struct glm_xfb_varying {
    char *name;
    uint32_t type;        /* GL type, 0 for gl_SkipComponents / gl_NextBuffer */
    int size;             /* array elements */
    int components;       /* floats written */
    int buffer, offset;   /* buffer index and offset in floats */
};

struct glm_compile_result;

/* ARB_shader_subroutine, lowered to a uint uniform per subroutine uniform
   (glm_sub_<name>) and a switch over the compatible functions. */
struct glm_subroutine {
    int stage, index;
    char *name;
};
struct glm_subroutine_uniform {
    int stage, location, array_size;
    char *name;
    int compatible_count;
    int compatible[32];   /* subroutine indices */
};

/* Geometry shaders on Metal (no geometry stage): the vertex stage writes
   its outputs per input vertex (vs_capture, like transform feedback), a
   compute kernel runs the geometry shader once per input primitive and
   invocation, writing up to max_vertices output vertices into a fixed slot
   and primitive indices (unused ones point at the off-screen vertex 0),
   and `pull` (a generated vertex stage reading those vertices, linked
   with the program's fragment stage) draws them. */
struct glm_gs_result {
    char *vs_capture;              /* MSL: vertex stage writing GLMXfb0 */
    int vs_stride;                 /* floats per captured vertex */
    int in_vertices;               /* per input primitive */
    int invocations, max_vertices, out_stride, max_indices;
    int out_primitive;             /* MTLPrimitiveType: 0 point, 1 line, 3 triangle */
    bool writes_viewport;          /* gl_ViewportIndex: every viewport is set */
    int feedback_offsets[64], feedback_streams[64];
    bool flat_outputs;
    struct glm_compile_result *kernel, *pull;
};

enum { GLM_SLOT_GS_INPUT = 24, GLM_SLOT_GS_VERTICES = 25, GLM_SLOT_GS_INDICES = 26, GLM_SLOT_GS_INFO = 27 };

/* Optional post-vertex user clipping. Temporary records contain Position4,
   ClipDistance[clip_count], then scalar/vector varying words. Output adds one
   primitive-ID word. One triangle reserves11vertices and27indices; record0
   is the degenerate sentinel. Info atslot27 is {triangles,mask,primitive_base,shifts}.
   shifts packs even/odd cyclic vertex offsets in bits0..1/2..3; flat varying
   values always come from captured vertex0, independently of these offsets. */
struct glm_clip_varying {
    char *name;
    int components, kind, interpolation, offset; /* kind0float,1int,2uint;
                                                   interpolation0smooth,1flat,2noperspective */
};
struct glm_clip_result {
    char *vs_capture;
    int vs_stride, out_stride, clip_count;
    bool flat_outputs, primitive_id;
    struct glm_clip_varying *varyings;
    int varying_count;
    struct glm_compile_result *kernel, *pull;
};
enum { GLM_CLIP_MAX_VERTICES=11, GLM_CLIP_MAX_INDICES=27 };

/* One input of the post-tessellation vertex function: Metal vertex
   attribute `attribute` reads float4 slot `slot` of the per-control-point
   buffer, or of the per-patch buffer when `patch`. */
struct glm_tess_input {
    int attribute, slot;
    bool patch, integer, is_unsigned;
};

/* Tessellation on Metal's tessellator: the vertex stage is captured per
   patch vertex (as for geometry shaders), the control shader runs as a
   compute kernel (one threadgroup per patch, one thread per output vertex)
   writing control points, per-patch data and tessellation factors, and the
   evaluation shader is the post-tessellation vertex function of `eval`
   (linked with the program's fragment stage). */
struct glm_tess_result {
    char *vs_capture;              /* MSL: vertex stage writing GLMXfb0 */
    int vs_stride;                 /* floats per captured vertex */
    int out_vertices;              /* control points per output patch */
    int cp_slots, patch_slots;     /* float4s per control point / per patch */
    bool quads;                    /* else triangles */
    int partition;                 /* MTLTessellationPartitionMode */
    bool cw;                       /* GL vertex order */
    bool point_mode;
    struct glm_tess_input *inputs;
    int input_count;
    struct glm_compile_result *kernel, *eval;
};

enum { GLM_SLOT_TESS_POINTS = 28, GLM_SLOT_TESS_PATCH = 29, GLM_SLOT_TESS_FACTORS = 28 /* kernel only */ };

struct glm_compile_result {
    bool ok;
    char *log;
    char *msl[GLM_STAGE_COUNT];
    char *msl_capture;    /* vertex stage writing the feedback varyings (transform feedback) */
    struct glm_gs_result *gs; /* the program has a geometry stage */
    struct glm_clip_result *clip; /* eligible VS-only post-transform clipping */
    struct glm_tess_result *tess; /* the program has tessellation stages */
    char *tess_inputs;    /* eval compiles: the MSL stage_in members, "patch type name attribute\n" */
    struct glm_xfb_varying *xfb;
    int xfb_count, xfb_buffers;
    int xfb_stride[4];    /* floats per vertex in each buffer */
    int global_size;      /* bytes of the global uniform block */
    unsigned char *initial_globals; /* global_size bytes the block starts with (uniform initializers), or NULL */
    struct glm_uniform_info *uniforms;
    int uniform_count;
    struct glm_block_info *blocks;
    int block_count;
    struct glm_uniform_info *block_uniforms; /* members of blocks: active uniforms after `uniforms` */
    int block_uniform_count;
    struct glm_subroutine *subroutines;
    int subroutine_count;
    struct glm_subroutine_uniform *subroutine_uniforms;
    int subroutine_uniform_count;
    struct glm_io_info *attributes;
    int attribute_count;
    struct glm_io_info *outputs;
    int output_count;
    int sampler_count;
    bool writes_frag_color;      /* legacy gl_FragColor: broadcast to all targets */
    bool flat_inputs;            /* the fragment stage has flat-interpolated inputs */
    bool uses_legacy;            /* some stage reads the GLMLegacy block */
    uint8_t globals_stages, legacy_stages; /* Metal stages reading GLMGlobals / GLMLegacy: 1 vertex, 2 fragment */
    uint32_t legacy_parts;       /* enum glm_legacy_part: which parts of it */
    uint32_t legacy_uniforms;    /* bitmask of glm legacy state blocks used */
};

/* `source` with identifiers from[i] renamed to[i] (members after '.' kept). malloc'd. */
char *glm_glsl_rename(const char *source, const char *const *from, const char *const *to, int count);
/* Apple's info log when `source`'s #version is not one its compiler takes
   in this profile, else NULL. malloc'd. */
char *glm_shader_version_error(const char *source, bool core);
/* Checks one shader's source for glCompileShader. Returns malloc'd log. */
bool glm_shader_check(enum glm_stage stage, const char *source, char **log);
/* Compiles and links a program. */
void glm_program_compile(const struct glm_compile_request *request, struct glm_compile_result *result);
void glm_compile_result_free(struct glm_compile_result *result);
/* Shared fixed-function/program cube sampling implementation. Static lifetime. */
const char *glm_cube_msl_helpers(void);

#ifdef __cplusplus
}
#endif

#endif
