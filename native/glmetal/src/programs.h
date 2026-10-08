/* Shader and program objects shared by programs.c and the Metal backend. */
#ifndef GLM_PROGRAMS_H
#define GLM_PROGRAMS_H

#include "glm_internal.h"
#include "shader_compiler.h"

struct glm_shader {
    GLuint name;
    GLenum type;
    char *source;
    char *log;
    bool compiled, delete_pending;
    int attached;
};

/* One GL uniform location: an element of a reflected uniform. */
struct glm_location {
    int uniform;
    int element;
};

struct glm_program;

struct glm_clip_state {
    void *vs_capture; /* retained MTLFunction */
    void *kernel;     /* retained MTLComputePipelineState */
    struct glm_program *pull;
};

/* A linked geometry stage's emulation (shader_compiler.h glm_gs_result). */
struct glm_gs_state {
    void *vs_capture;             /* id<MTLFunction>: vertex stage writing its outputs */
    void *kernel;                 /* id<MTLComputePipelineState>: the geometry shader */
    struct glm_program *pull;     /* draws the kernel's output with the fragment stage */
    uint8_t *kernel_globals;
};

/* Tessellation emulation (shader_compiler.h glm_tess_result). */
struct glm_tess_state {
    void *vs_capture;             /* id<MTLFunction> */
    void *kernel;                 /* id<MTLComputePipelineState>: the control shader */
    struct glm_program *eval;     /* post-tessellation vertex function and fragment stage */
    uint8_t *kernel_globals;
};

struct glm_program {
    GLuint name;
    GLuint shaders[8];
    int shader_count;
    struct glm_name_location *attribute_bindings, *output_bindings;
    int attribute_binding_count, output_binding_count;
    char **feedback_varyings;
    int feedback_count;
    GLenum feedback_mode;
    GLint ext_geometry[3];           /* EXT_geometry_shader4 vertices out, input and output types */
    bool linked, validated, delete_pending;
    char *log;
    struct glm_compile_result result;
    uint8_t *globals;                /* result.global_size bytes */
    struct glm_location *locations;
    int location_count;
    int sampler_units[64];           /* texture unit per sampler slot */
    GLuint block_bindings[GLM_SLOT_UBO_COUNT];
    void *functions[GLM_STAGE_COUNT]; /* id<MTLFunction> */
    void *functions_capture;          /* vertex stage writing feedback varyings */
    struct glm_gs_state *gs;          /* geometry stage emulation, or NULL */
    struct glm_tess_state *tess;      /* tessellation emulation, or NULL */
    struct glm_clip_state *clip;      /* bounded post-vertex user clipping, or NULL */
    bool borrowed_result;             /* result belongs to another program (gs->pull) */
    bool separable;                   /* GL_PROGRAM_SEPARABLE */
    char *linked_sources[GLM_STAGE_COUNT]; /* the stages' sources as last linked (program pipelines) */
    uint64_t uniform_serial;          /* bumped by every uniform change (program pipelines copy them) */
    GLuint subroutine_values[GLM_STAGE_COUNT][64]; /* glUniformSubroutinesuiv, by stage and location */
    /* Vertex functions for float inputs fed integer arrays (metal_backend.m). */
    struct glm_int_variant *int_variants[16]; /* integer input / border variants, compiled in the background (metal_backend.m) */
    int int_variant_count;
    /* The fragment stage reads gl_Color / gl_SecondaryColor: flat under
       glShadeModel(GL_FLAT) in the legacy profile, as on Apple's. */
    bool color_inputs;
    uint64_t link_serial;   /* unique per successful link */
    /* metal_backend.m vertex_inputs, memoized per link (0 = not yet). */
    uint64_t inputs_link;
    uint32_t inputs_used, inputs_integer, inputs_unsigned;
    void *vertex_descriptor;          /* MTLVertexDescriptor for this program's inputs */
    uint64_t generation;
    /* The globals' last upload (metal_backend.m), redone when dirty. */
    bool globals_dirty;
    void *globals_buffer, *globals_backend;
    size_t globals_offset;
    uint64_t globals_generation;
};

GLM_HIDDEN struct glm_program *glm_program_get(struct glm_context *ctx, GLuint name);
GLM_HIDDEN struct glm_program *glm_current_program(struct glm_context *ctx);
/* Links `p` from per-stage sources (NULL: absent), as glLinkProgram does. */
GLM_HIDDEN bool glm_program_link_sources(struct glm_context *ctx, struct glm_program *p, const char *const *sources);
GLM_HIDDEN void glm_program_free(struct glm_context *ctx, struct glm_program *p);
/* Program pipelines (pipelines.c): the program glUniform* changes without
   a current program, and the combined program a bound pipeline draws with. */
GLM_HIDDEN struct glm_program *glm_pipeline_active_program(struct glm_context *ctx);
GLM_HIDDEN struct glm_program *glm_pipeline_program(struct glm_context *ctx);
GLM_HIDDEN struct glm_program *glm_pipeline_stage_program(struct glm_context *ctx, int stage);
/* Creates the Metal functions for a linked program; false (with a log) on
   failure. */
GLM_HIDDEN bool glm_backend_program_link(struct glm_context *ctx, struct glm_program *program, char **log);
GLM_HIDDEN void glm_backend_program_release(struct glm_program *program);

/* Named creation and application-thread linking (marshal_custom.c). */
GLM_HIDDEN void glm_create_shader_named(struct glm_context *ctx, GLenum type, GLuint name);
GLM_HIDDEN void glm_create_program_named(struct glm_context *ctx, GLuint name);
GLM_HIDDEN void glm_shader_set_compiled(struct glm_context *ctx, GLuint name, bool ok, const char *log);
GLM_HIDDEN bool glm_program_query_iv(struct glm_program *p, GLenum pname, GLint *params);
GLM_HIDDEN GLint glm_program_attrib_location(const struct glm_program *p, const char *attribute);
GLM_HIDDEN GLint glm_program_uniform_location(const struct glm_program *p, const char *uniform);
GLM_HIDDEN struct glm_program *glm_program_uniform_target(struct glm_context *ctx);
GLM_HIDDEN bool glm_program_set_double(struct glm_context *ctx, struct glm_program *p, GLint location,
                                      GLsizei count, int columns, int rows, GLboolean transpose, const GLdouble *values);
GLM_HIDDEN bool glm_program_get_double(const struct glm_program *p, GLint location, GLdouble *values);
GLM_HIDDEN struct glm_program *glm_program_shadow_link(const struct glm_compile_request *request);
GLM_HIDDEN void glm_program_shadow_free(struct glm_program *p);
GLM_HIDDEN void glm_compile_pending_begin(const struct glm_compile_request *request);
GLM_HIDDEN void glm_compile_pending_end(const struct glm_compile_request *request);

#endif
