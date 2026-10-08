/* Transform feedback (GL 3.0, ARB_transform_feedback2/3 objects): captured
 * by a variant of the program's vertex stage that writes its feedback
 * varyings to storage buffers (shader_compiler.cpp), run on de-indexed,
 * primitive-ordered vertices (vertex.c) so the vertex index is the output
 * position. */
#include "glm_internal.h"
#include "programs.h"
#include "queries.h"

#include <stdlib.h>
#include <string.h>

/* GL 3/4 entry points gl.h does not declare. */
void glTransformFeedbackVaryings(GLuint program, GLsizei count, const GLchar *const *varyings, GLenum mode);
void glBindTransformFeedback(GLenum target, GLuint name);

/* A transform feedback object's buffer bindings and last capture. */
struct feedback_object {
    struct glm_indexed_buffer bindings[4];
    uint32_t vertices;
    uint32_t stream_vertices[4]; /* completed capture, independent of a later Begin */
    struct glm_feedback capture;
    bool bound;
};

static GLenum base_mode(GLenum mode)
{
    switch (mode) {
    case GL_POINTS: return GL_POINTS;
    case GL_LINES: case GL_LINE_STRIP: case GL_LINE_LOOP: case GL_LINES_ADJACENCY: case GL_LINE_STRIP_ADJACENCY: return GL_LINES;
    default: return GL_TRIANGLES;
    }
}

bool glm_feedback_draw_allowed(struct glm_context *ctx, GLenum mode)
{
    if (!ctx->xfb.active || ctx->xfb.paused) return true;
    struct glm_program *program = glm_current_program(ctx);
    GLenum output_mode = program && program->result.gs
        ? (program->result.gs->out_primitive == 3 ? GL_TRIANGLES : program->result.gs->out_primitive == 1 ? GL_LINES : GL_POINTS)
        : base_mode(mode);
    if (output_mode != ctx->xfb.mode) {
        glm_error(ctx, GL_INVALID_OPERATION);
        return false;
    }
    return true;
}

bool glm_feedback_capturing(struct glm_context *ctx)
{
    return ctx->xfb.active && !ctx->xfb.paused;
}

/* Reserves room for `vertices` output vertices (whole primitives only);
   returns how many are captured and their first index. */
uint32_t glm_feedback_reserve(struct glm_context *ctx, uint32_t vertices, uint32_t *first)
{
    uint32_t per = ctx->xfb.mode == GL_POINTS ? 1 : ctx->xfb.mode == GL_LINES ? 2 : 3;
    uint32_t room = ctx->xfb.capacity > ctx->xfb.vertices ? ctx->xfb.capacity - ctx->xfb.vertices : 0;
    uint32_t count = vertices < room ? vertices : room;
    count -= count % per;
    *first = ctx->xfb.vertices;
    ctx->xfb.vertices += count;
    ctx->xfb.stream_vertices[0] = ctx->xfb.vertices;
    struct glm_query *q = ctx->active_queries[GLM_QUERY_FEEDBACK];
    if (q) q->counter += count / per;
    return count;
}

GLM_EXPORT void glBeginTransformFeedback(GLenum mode)
{
    GLM_CONTEXT(ctx);
    if (mode != GL_POINTS && mode != GL_LINES && mode != GL_TRIANGLES) return glm_error(ctx, GL_INVALID_ENUM);
    if (ctx->xfb.active) return glm_error(ctx, GL_INVALID_OPERATION);
    struct glm_program *p = glm_current_program(ctx);
    if (!p || !p->result.xfb_count || (!p->functions_capture && !p->gs)) return glm_error(ctx, GL_INVALID_OPERATION);
    uint32_t capacity = UINT32_MAX;
    for (int b = 0; b < p->result.xfb_buffers; ++b) {
        const struct glm_indexed_buffer *binding = &ctx->feedback_bindings[b];
        struct glm_buffer *buffer = binding->buffer ? glm_buffer_get(ctx, binding->buffer) : NULL;
        if (!buffer) return glm_error(ctx, GL_INVALID_OPERATION);
        GLsizeiptr size = binding->size >= 0 ? binding->size : buffer->size - binding->offset;
        uint32_t stride = (uint32_t)p->result.xfb_stride[b] * 4;
        uint32_t fit = stride ? (uint32_t)(size > 0 ? (uint64_t)size / stride : 0) : UINT32_MAX;
        if (fit < capacity) capacity = fit;
    }
    ctx->xfb = (struct glm_feedback){true, false, mode, 0, capacity};
}
GLM_EXPORT void glBeginTransformFeedbackEXT(GLenum mode) { glBeginTransformFeedback(mode); }

GLM_EXPORT void glEndTransformFeedback(void)
{
    GLM_CONTEXT(ctx);
    if (!ctx->xfb.active) return glm_error(ctx, GL_INVALID_OPERATION);
    ctx->xfb.active = false;
    ctx->xfb.paused = false;
    struct feedback_object *o = ctx->feedback_object ? glm_table_get(&ctx->feedback_objects, ctx->feedback_object) : NULL;
    if (o) {
        o->vertices = ctx->xfb.vertices;
        memcpy(o->stream_vertices, ctx->xfb.stream_vertices, sizeof o->stream_vertices);
    } else {
        ctx->xfb_last_vertices = ctx->xfb.vertices;
        memcpy(ctx->xfb_last_stream_vertices, ctx->xfb.stream_vertices, sizeof ctx->xfb_last_stream_vertices);
    }
}
GLM_EXPORT void glEndTransformFeedbackEXT(void) { glEndTransformFeedback(); }

GLM_EXPORT void glPauseTransformFeedback(void)
{
    GLM_CONTEXT(ctx);
    if (!ctx->xfb.active || ctx->xfb.paused) return glm_error(ctx, GL_INVALID_OPERATION);
    ctx->xfb.paused = true;
}

GLM_EXPORT void glResumeTransformFeedback(void)
{
    GLM_CONTEXT(ctx);
    if (!ctx->xfb.active || !ctx->xfb.paused) return glm_error(ctx, GL_INVALID_OPERATION);
    ctx->xfb.paused = false;
}

GLM_EXPORT void glTransformFeedbackVaryingsEXT(GLuint program, GLsizei count, const GLchar *const *varyings, GLenum mode)
{
    glTransformFeedbackVaryings(program, count, varyings, mode);
}

GLM_EXPORT void glGetTransformFeedbackVarying(GLuint name, GLuint index, GLsizei size, GLsizei *length, GLsizei *array,
                                              GLenum *type, GLchar *out)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = glm_program_get(ctx, name);
    if (!p) return glm_error(ctx, GL_INVALID_VALUE);
    if (index >= (GLuint)p->result.xfb_count) return glm_error(ctx, GL_INVALID_VALUE);
    const struct glm_xfb_varying *v = &p->result.xfb[index];
    if (size > 0) {
        size_t n = strlen(v->name);
        if (n > (size_t)size - 1) n = (size_t)size - 1;
        memcpy(out, v->name, n);
        out[n] = 0;
        if (length) *length = (GLsizei)n;
    } else if (length) {
        *length = 0;
    }
    if (array) *array = v->size;
    if (type) *type = v->type ? v->type : GL_NONE;
}
GLM_EXPORT void glGetTransformFeedbackVaryingEXT(GLuint name, GLuint index, GLsizei size, GLsizei *length, GLsizei *array,
                                                 GLenum *type, GLchar *out)
{
    glGetTransformFeedbackVarying(name, index, size, length, array, type, out);
}

/* ---- objects (ARB_transform_feedback2) ---------------------------------- */

GLM_EXPORT void glGenTransformFeedbacks(GLsizei n, GLuint *names)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; ++i) {
        names[i] = glm_table_reserve(&ctx->feedback_objects);
        glm_table_set(&ctx->feedback_objects, names[i], calloc(1, sizeof(struct feedback_object)));
    }
}

GLM_EXPORT void glDeleteTransformFeedbacks(GLsizei n, const GLuint *names)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; ++i) {
        struct feedback_object *o = names[i] ? glm_table_get(&ctx->feedback_objects, names[i]) : NULL;
        if (!o) continue;
        if ((ctx->feedback_object == names[i] ? ctx->xfb.active : o->capture.active))
            return glm_error(ctx, GL_INVALID_OPERATION);
        if (ctx->feedback_object == names[i]) glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, 0);
        glm_table_set(&ctx->feedback_objects, names[i], NULL);
        free(o);
    }
}

GLM_EXPORT GLboolean glIsTransformFeedback(GLuint name)
{
    GLM_CONTEXT(ctx, GL_FALSE);
    struct feedback_object *o = glm_table_get(&ctx->feedback_objects, name);
    return o && o->bound;
}

/* Bindings live in the object: swap them into the context's slots. */
GLM_EXPORT void glBindTransformFeedback(GLenum target, GLuint name)
{
    GLM_CONTEXT(ctx);
    if (target != GL_TRANSFORM_FEEDBACK) return glm_error(ctx, GL_INVALID_ENUM);
    if (ctx->xfb.active && !ctx->xfb.paused) return glm_error(ctx, GL_INVALID_OPERATION);
    struct feedback_object *next = name ? glm_table_get(&ctx->feedback_objects, name) : NULL;
    if (name && !next) return glm_error(ctx, GL_INVALID_OPERATION);
    if (next) next->bound = true;
    struct feedback_object *current = ctx->feedback_object ? glm_table_get(&ctx->feedback_objects, ctx->feedback_object) : NULL;
    memcpy(current ? current->bindings : ctx->feedback_default_bindings, ctx->feedback_bindings, sizeof ctx->feedback_bindings);
    if (current) current->capture = ctx->xfb;
    else ctx->feedback_default_capture = ctx->xfb;
    memcpy(ctx->feedback_bindings, next ? next->bindings : ctx->feedback_default_bindings, sizeof ctx->feedback_bindings);
    ctx->xfb = next ? next->capture : ctx->feedback_default_capture;
    ctx->feedback_object = name;
}

/* Draws the vertices the object's last capture wrote, from whatever arrays
   are bound (the application points them at the feedback buffers). */
GLM_EXPORT void glDrawTransformFeedback(GLenum mode, GLuint name)
{
    GLM_CONTEXT(ctx);
    struct feedback_object *o = name ? glm_table_get(&ctx->feedback_objects, name) : NULL;
    if (name && !o) return glm_error(ctx, GL_INVALID_VALUE);
    uint32_t count = o ? o->vertices : ctx->xfb_last_vertices;
    glm_draw_arrays_internal(ctx, mode, 0, (GLsizei)count, 1);
}

GLM_EXPORT void glDrawTransformFeedbackStream(GLenum mode, GLuint name, GLuint stream)
{
    GLM_CONTEXT(ctx);
    if (stream >= 4) return glm_error(ctx, GL_INVALID_VALUE);
    struct feedback_object *o = name ? glm_table_get(&ctx->feedback_objects, name) : NULL;
    if (name && !o) return glm_error(ctx, GL_INVALID_VALUE);
    uint32_t count = o ? o->stream_vertices[stream] : ctx->xfb_last_stream_vertices[stream];
    glm_draw_arrays_internal(ctx, mode, 0, (GLsizei)count, 1);
}
