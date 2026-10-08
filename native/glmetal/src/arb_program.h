/* ARB program objects shared by arb_program.c and the Metal backend. */
#ifndef GLM_ARB_PROGRAM_H
#define GLM_ARB_PROGRAM_H

#include "legacy_block.h"
#include "programs.h"

struct glm_arb_program {
    GLuint name;
    GLenum target;
    char *source, *glsl;
    size_t length;
    bool valid;
    uint64_t generation;
    int env_count, local_count;  /* sizes of the GLMARB arrays */
    float local[GLM_ARB_PARAMETERS][4];
    struct glm_program linked;   /* compiled translation (one stage) */
};

GLM_HIDDEN struct glm_arb_program *glm_arb_program_get(struct glm_context *ctx, GLuint name);
/* The enabled, valid program of target 0 (vertex) or 1 (fragment). */
GLM_HIDDEN struct glm_arb_program *glm_arb_current(struct glm_context *ctx, int index);
GLM_HIDDEN int glm_arb_query(struct glm_context *ctx, GLenum pname, double *v);
GLM_HIDDEN const char *glm_arb_error_string(struct glm_context *ctx);
GLM_HIDDEN void glm_arb_context_destroy(struct glm_context *ctx);

#endif
