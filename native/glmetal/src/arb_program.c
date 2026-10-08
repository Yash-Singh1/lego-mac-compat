/* ARB_vertex_program / ARB_fragment_program objects. Programs are
 * translated to GLSL (arb_translate.c) and compiled like single-stage GLSL
 * programs; env and local parameters feed the GLMARB block. */
#include "arb_program.h"

#include "arb_translate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int target_index(GLenum target)
{
    if (target == GL_VERTEX_PROGRAM_ARB) return 0;
    if (target == GL_FRAGMENT_PROGRAM_ARB) return 1;
    return -1;
}

struct glm_arb_program *glm_arb_program_get(struct glm_context *ctx, GLuint name)
{
    return name ? glm_table_get(&ctx->share->arb_programs, name) : NULL;
}

struct glm_arb_program *glm_arb_current(struct glm_context *ctx, int index)
{
    bool enabled = index == 0 ? ctx->state.vertex_program : ctx->state.fragment_program;
    if (!enabled) return NULL;
    struct glm_arb_program *p = ctx->arb_bound[index] ? glm_arb_program_get(ctx, ctx->arb_bound[index])
                                                      : ctx->arb_default[index];
    return p && p->valid ? p : NULL;
}

GLM_EXPORT void glGenProgramsARB(GLsizei n, GLuint *names)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    pthread_mutex_lock(&ctx->share->lock);
    for (GLsizei i = 0; i < n; ++i) names[i] = glm_table_reserve(&ctx->share->arb_programs);
    pthread_mutex_unlock(&ctx->share->lock);
}

static struct glm_arb_program *create(struct glm_context *ctx, GLuint name, GLenum target)
{
    struct glm_arb_program *p = calloc(1, sizeof *p);
    p->name = name;
    p->target = target;
    pthread_mutex_lock(&ctx->share->lock);
    glm_table_set(&ctx->share->arb_programs, name, p);
    pthread_mutex_unlock(&ctx->share->lock);
    return p;
}

GLM_EXPORT void glBindProgramARB(GLenum target, GLuint name)
{
    GLM_CONTEXT(ctx);
    int index = target_index(target);
    if (index < 0) return glm_error(ctx, GL_INVALID_ENUM);
    if (name) {
        struct glm_arb_program *p = glm_arb_program_get(ctx, name);
        if (p && p->target != target) return glm_error(ctx, GL_INVALID_OPERATION);
        if (!p) create(ctx, name, target);
    }
    ctx->arb_bound[index] = name;
}

static void release(struct glm_arb_program *p)
{
    glm_compile_result_free(&p->linked.result);
    glm_backend_program_release(&p->linked);
    free(p->source);
    free(p->glsl);
    p->source = p->glsl = NULL;
}

GLM_EXPORT void glDeleteProgramsARB(GLsizei n, const GLuint *names)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; ++i) {
        struct glm_arb_program *p = glm_arb_program_get(ctx, names[i]);
        if (!p) continue;
        for (int t = 0; t < 2; ++t)
            if (ctx->arb_bound[t] == names[i]) ctx->arb_bound[t] = 0;
        pthread_mutex_lock(&ctx->share->lock);
        glm_table_set(&ctx->share->arb_programs, names[i], NULL);
        pthread_mutex_unlock(&ctx->share->lock);
        release(p);
        free(p);
    }
}

GLM_EXPORT GLboolean glIsProgramARB(GLuint name) { GLM_CONTEXT(ctx, GL_FALSE); return glm_arb_program_get(ctx, name) != NULL; }

GLM_EXPORT void glProgramStringARB(GLenum target, GLenum format, GLsizei length, const void *string)
{
    GLM_CONTEXT(ctx);
    int index = target_index(target);
    if (index < 0) return glm_error(ctx, GL_INVALID_ENUM);
    if (format != GL_PROGRAM_FORMAT_ASCII_ARB) return glm_error(ctx, GL_INVALID_ENUM);
    struct glm_arb_program *p = glm_arb_program_get(ctx, ctx->arb_bound[index]);
    if (!p) {
        /* Program 0 is the default program object; give it storage. */
        GLuint name = ctx->arb_bound[index];
        if (name) return glm_error(ctx, GL_INVALID_OPERATION);
        p = ctx->arb_default[index];
        if (!p) {
            p = ctx->arb_default[index] = calloc(1, sizeof *p);
            p->target = target;
        }
    }
    release(p);
    p->valid = false;
    p->source = malloc((size_t)length + 1);
    memcpy(p->source, string, (size_t)length);
    p->source[length] = 0;
    p->length = (size_t)length;

    free(ctx->arb_error_string);
    ctx->arb_error_string = NULL;
    ctx->arb_error_position = -1;
    struct glm_arb_translation translation;
    if (!glm_arb_translate(p->source, p->length, index == 0, &translation)) {
        ctx->arb_error_position = translation.error_position;
        ctx->arb_error_string = strdup(translation.error);
        glm_log("ARB program rejected at %d: %s", translation.error_position, translation.error);
        return glm_error(ctx, GL_INVALID_OPERATION);
    }
    p->glsl = translation.glsl;
    p->env_count = translation.env_count;
    p->local_count = translation.local_count;

    struct glm_name_location attributes[16];
    int attribute_count = 0;
    char attribute_names[16][16];
    for (int i = 0; i < 16; ++i) {
        if (!(translation.attribs_used & (1u << i))) continue;
        snprintf(attribute_names[i], sizeof attribute_names[i], "arb_attrib%d", i);
        attributes[attribute_count++] = (struct glm_name_location){attribute_names[i], i};
    }
    struct glm_compile_request request = {0};
    request.sources[index == 0 ? GLM_STAGE_VERTEX : GLM_STAGE_FRAGMENT] = p->glsl;
    request.attributes = attributes;
    request.attribute_count = attribute_count;
    glm_program_compile(&request, &p->linked.result);
    char *log = NULL;
    bool ok = p->linked.result.ok && glm_backend_program_link(ctx, &p->linked, &log);
    if (!ok) {
        /* Translation succeeded, so this is our bug, not the program's. */
        glm_log("ARB program failed to compile:\n%s\n%s%s", p->glsl, p->linked.result.log ? p->linked.result.log : "",
                log ? log : "");
        ctx->arb_error_position = 0;
        ctx->arb_error_string = strdup("internal compiler error");
        free(log);
        return glm_error(ctx, GL_INVALID_OPERATION);
    }
    p->valid = true;
    ++p->generation;
}

/* ---- parameters ---------------------------------------------------------- */

static float *env_slot(struct glm_context *ctx, GLenum target, GLuint index)
{
    int t = target_index(target);
    if (t < 0) {
        glm_error(ctx, GL_INVALID_ENUM);
        return NULL;
    }
    if (index >= GLM_ARB_PARAMETERS) {
        glm_error(ctx, GL_INVALID_VALUE);
        return NULL;
    }
    return ctx->arb_env[t][index];
}

static float *local_slot(struct glm_context *ctx, GLenum target, GLuint index)
{
    int t = target_index(target);
    if (t < 0) {
        glm_error(ctx, GL_INVALID_ENUM);
        return NULL;
    }
    if (index >= GLM_ARB_PARAMETERS) {
        glm_error(ctx, GL_INVALID_VALUE);
        return NULL;
    }
    struct glm_arb_program *p = glm_arb_program_get(ctx, ctx->arb_bound[t]);
    if (!p) p = ctx->arb_default[t];
    if (!p) {
        p = ctx->arb_default[t] = calloc(1, sizeof *p);
        p->target = target;
    }
    return p->local[index];
}

static void set4(float *dst, float x, float y, float z, float w)
{
    if (!dst) return;
    dst[0] = x;
    dst[1] = y;
    dst[2] = z;
    dst[3] = w;
}

GLM_EXPORT void glProgramEnvParameter4fARB(GLenum target, GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    GLM_CONTEXT(ctx);
    set4(env_slot(ctx, target, index), x, y, z, w);
}
GLM_EXPORT void glProgramEnvParameter4fvARB(GLenum target, GLuint index, const GLfloat *v)
{ glProgramEnvParameter4fARB(target, index, v[0], v[1], v[2], v[3]); }
GLM_EXPORT void glProgramEnvParameter4dARB(GLenum target, GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w)
{ glProgramEnvParameter4fARB(target, index, (float)x, (float)y, (float)z, (float)w); }
GLM_EXPORT void glProgramEnvParameter4dvARB(GLenum target, GLuint index, const GLdouble *v)
{ glProgramEnvParameter4fARB(target, index, (float)v[0], (float)v[1], (float)v[2], (float)v[3]); }
GLM_EXPORT void glProgramEnvParameters4fvEXT(GLenum target, GLuint index, GLsizei count, const GLfloat *v)
{
    GLM_CONTEXT(ctx);
    if (count < 0 || index + (GLuint)count > GLM_ARB_PARAMETERS) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < count; ++i) {
        float *dst = env_slot(ctx, target, index + (GLuint)i);
        if (!dst) return;
        memcpy(dst, v + i * 4, 4 * sizeof(float));
    }
}

GLM_EXPORT void glProgramLocalParameter4fARB(GLenum target, GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    GLM_CONTEXT(ctx);
    set4(local_slot(ctx, target, index), x, y, z, w);
}
GLM_EXPORT void glProgramLocalParameter4fvARB(GLenum target, GLuint index, const GLfloat *v)
{ glProgramLocalParameter4fARB(target, index, v[0], v[1], v[2], v[3]); }
GLM_EXPORT void glProgramLocalParameter4dARB(GLenum target, GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w)
{ glProgramLocalParameter4fARB(target, index, (float)x, (float)y, (float)z, (float)w); }
GLM_EXPORT void glProgramLocalParameter4dvARB(GLenum target, GLuint index, const GLdouble *v)
{ glProgramLocalParameter4fARB(target, index, (float)v[0], (float)v[1], (float)v[2], (float)v[3]); }
GLM_EXPORT void glProgramLocalParameters4fvEXT(GLenum target, GLuint index, GLsizei count, const GLfloat *v)
{
    GLM_CONTEXT(ctx);
    if (count < 0 || index + (GLuint)count > GLM_ARB_PARAMETERS) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < count; ++i) {
        float *dst = local_slot(ctx, target, index + (GLuint)i);
        if (!dst) return;
        memcpy(dst, v + i * 4, 4 * sizeof(float));
    }
}

GLM_EXPORT void glGetProgramEnvParameterfvARB(GLenum target, GLuint index, GLfloat *v)
{
    GLM_CONTEXT(ctx);
    float *src = env_slot(ctx, target, index);
    if (src) memcpy(v, src, 4 * sizeof(float));
}
GLM_EXPORT void glGetProgramEnvParameterdvARB(GLenum target, GLuint index, GLdouble *v)
{
    GLfloat f[4] = {0};
    glGetProgramEnvParameterfvARB(target, index, f);
    for (int i = 0; i < 4; ++i) v[i] = f[i];
}
GLM_EXPORT void glGetProgramLocalParameterfvARB(GLenum target, GLuint index, GLfloat *v)
{
    GLM_CONTEXT(ctx);
    float *src = local_slot(ctx, target, index);
    if (src) memcpy(v, src, 4 * sizeof(float));
}
GLM_EXPORT void glGetProgramLocalParameterdvARB(GLenum target, GLuint index, GLdouble *v)
{
    GLfloat f[4] = {0};
    glGetProgramLocalParameterfvARB(target, index, f);
    for (int i = 0; i < 4; ++i) v[i] = f[i];
}

/* ---- queries ------------------------------------------------------------- */

GLM_EXPORT void glGetProgramivARB(GLenum target, GLenum pname, GLint *params)
{
    GLM_CONTEXT(ctx);
    int t = target_index(target);
    if (t < 0) return glm_error(ctx, GL_INVALID_ENUM);
    struct glm_arb_program *p = glm_arb_program_get(ctx, ctx->arb_bound[t]);
    if (!p) p = ctx->arb_default[t];
    bool vertex = t == 0;
    switch (pname) {
    case GL_PROGRAM_LENGTH_ARB: *params = p ? (GLint)p->length : 0; return;
    case GL_PROGRAM_FORMAT_ARB: *params = GL_PROGRAM_FORMAT_ASCII_ARB; return;
    case GL_PROGRAM_BINDING_ARB: *params = (GLint)ctx->arb_bound[t]; return;
    case GL_PROGRAM_UNDER_NATIVE_LIMITS_ARB: *params = p && p->valid; return;
    case GL_MAX_PROGRAM_ENV_PARAMETERS_ARB: case GL_MAX_PROGRAM_LOCAL_PARAMETERS_ARB: *params = GLM_ARB_PARAMETERS; return;
    case GL_MAX_PROGRAM_INSTRUCTIONS_ARB: case GL_MAX_PROGRAM_NATIVE_INSTRUCTIONS_ARB:
    case GL_MAX_PROGRAM_ALU_INSTRUCTIONS_ARB: case GL_MAX_PROGRAM_NATIVE_ALU_INSTRUCTIONS_ARB:
    case GL_MAX_PROGRAM_TEX_INSTRUCTIONS_ARB: case GL_MAX_PROGRAM_NATIVE_TEX_INSTRUCTIONS_ARB:
    case GL_MAX_PROGRAM_TEX_INDIRECTIONS_ARB: case GL_MAX_PROGRAM_NATIVE_TEX_INDIRECTIONS_ARB:
        *params = vertex ? 65536 : 65536; return;
    case GL_MAX_PROGRAM_TEMPORARIES_ARB: case GL_MAX_PROGRAM_NATIVE_TEMPORARIES_ARB: *params = 256; return;
    case GL_MAX_PROGRAM_PARAMETERS_ARB: case GL_MAX_PROGRAM_NATIVE_PARAMETERS_ARB: *params = GLM_ARB_PARAMETERS; return;
    case GL_MAX_PROGRAM_ATTRIBS_ARB: case GL_MAX_PROGRAM_NATIVE_ATTRIBS_ARB: *params = vertex ? 16 : 10; return;
    case GL_MAX_PROGRAM_ADDRESS_REGISTERS_ARB: case GL_MAX_PROGRAM_NATIVE_ADDRESS_REGISTERS_ARB: *params = vertex ? 1 : 0; return;
    case GL_PROGRAM_INSTRUCTIONS_ARB: case GL_PROGRAM_NATIVE_INSTRUCTIONS_ARB: case GL_PROGRAM_TEMPORARIES_ARB:
    case GL_PROGRAM_NATIVE_TEMPORARIES_ARB: case GL_PROGRAM_PARAMETERS_ARB: case GL_PROGRAM_NATIVE_PARAMETERS_ARB:
    case GL_PROGRAM_ATTRIBS_ARB: case GL_PROGRAM_NATIVE_ATTRIBS_ARB: case GL_PROGRAM_ADDRESS_REGISTERS_ARB:
    case GL_PROGRAM_NATIVE_ADDRESS_REGISTERS_ARB: case GL_PROGRAM_ALU_INSTRUCTIONS_ARB:
    case GL_PROGRAM_NATIVE_ALU_INSTRUCTIONS_ARB: case GL_PROGRAM_TEX_INSTRUCTIONS_ARB:
    case GL_PROGRAM_NATIVE_TEX_INSTRUCTIONS_ARB: case GL_PROGRAM_TEX_INDIRECTIONS_ARB:
    case GL_PROGRAM_NATIVE_TEX_INDIRECTIONS_ARB:
        *params = 0; return;
    case GL_MAX_PROGRAM_MATRICES_ARB: *params = 8; return;
    case GL_MAX_PROGRAM_MATRIX_STACK_DEPTH_ARB: *params = 1; return;
    }
    glm_error(ctx, GL_INVALID_ENUM);
}

GLM_EXPORT void glGetProgramStringARB(GLenum target, GLenum pname, void *string)
{
    GLM_CONTEXT(ctx);
    int t = target_index(target);
    if (t < 0 || pname != GL_PROGRAM_STRING_ARB) return glm_error(ctx, GL_INVALID_ENUM);
    struct glm_arb_program *p = glm_arb_program_get(ctx, ctx->arb_bound[t]);
    if (!p) p = ctx->arb_default[t];
    if (p && p->source) memcpy(string, p->source, p->length);
}

int glm_arb_query(struct glm_context *ctx, GLenum pname, double *v)
{
    switch (pname) {
    case GL_PROGRAM_ERROR_POSITION_ARB: v[0] = ctx->arb_error_position; return 1;
    case GL_MAX_PROGRAM_MATRICES_ARB: v[0] = 8; return 1;
    case GL_MAX_PROGRAM_MATRIX_STACK_DEPTH_ARB: v[0] = 1; return 1;
    case GL_CURRENT_MATRIX_STACK_DEPTH_ARB: v[0] = 1; return 1;
    case GL_MAX_VERTEX_ATTRIBS_ARB: v[0] = 16; return 1;
    case GL_MAX_TEXTURE_COORDS_ARB: v[0] = 8; return 1;
    case GL_MAX_TEXTURE_IMAGE_UNITS_ARB: v[0] = 16; return 1;
    default: return 0;
    }
}

const char *glm_arb_error_string(struct glm_context *ctx)
{
    return ctx->arb_error_string ? ctx->arb_error_string : "";
}

void glm_arb_context_destroy(struct glm_context *ctx)
{
    for (int t = 0; t < 2; ++t) {
        if (!ctx->arb_default[t]) continue;
        release(ctx->arb_default[t]);
        free(ctx->arb_default[t]);
        ctx->arb_default[t] = NULL;
    }
    free(ctx->arb_error_string);
    ctx->arb_error_string = NULL;
}
