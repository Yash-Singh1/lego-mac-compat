/* CPU-only API state test using the real feedback.c and bounded mocks. */
#include "../../src/feedback.c"
#include <assert.h>
#include <stdio.h>

static struct glm_context context;
static struct glm_program program;
static struct glm_buffer buffer;
static void *objects[8];
static GLuint next_name = 1;
static GLenum error;
static unsigned drawn;
struct glm_context *glm_current(void) { return &context; }
void glm_error(struct glm_context *ctx, GLenum value) { (void)ctx; error = value; }
void *glm_table_get(struct glm_object_table *table, GLuint name) { (void)table; return name < 8 ? objects[name] : NULL; }
void glm_table_set(struct glm_object_table *table, GLuint name, void *value) { (void)table; assert(name < 8); objects[name] = value; }
GLuint glm_table_reserve(struct glm_object_table *table) { (void)table; return next_name++; }
struct glm_program *glm_current_program(struct glm_context *ctx) { (void)ctx; return &program; }
struct glm_program *glm_program_get(struct glm_context *ctx, GLuint name) { (void)ctx; (void)name; return &program; }
struct glm_buffer *glm_buffer_get(struct glm_context *ctx, GLuint name) { (void)ctx; return name ? &buffer : NULL; }
void glm_draw_arrays_internal(struct glm_context *ctx, GLenum mode, GLint first, GLsizei count, GLsizei instances)
{ (void)ctx; (void)mode; (void)first; drawn += count*instances; }
void glTransformFeedbackVaryings(GLuint p, GLsizei n, const GLchar *const *v, GLenum m) { (void)p; (void)n; (void)v; (void)m; }
int main(void)
{
    program.result.xfb_count = program.result.xfb_buffers = program.result.xfb_stride[0] = 1;
    program.functions_capture = (void *)1;
    GLuint names[3] = {0}; glGenTransformFeedbacks(2, names+1);
    struct glm_query query = {0}, nonzero[3] = {0};
    context.active_queries[GLM_QUERY_FEEDBACK] = &query;
    for (int stream = 1; stream < 4; ++stream)
        context.indexed_queries[GLM_QUERY_FEEDBACK][stream - 1] = &nonzero[stream - 1];
    for (int i = 0; i < 3; ++i) {
        glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, names[i]);
        assert(!context.xfb.active && !context.xfb.paused);
        context.feedback_bindings[0] = (struct glm_indexed_buffer){(GLuint)(i+1), 0, (i+3)*4};
        glBeginTransformFeedback(GL_POINTS); uint32_t first;
        assert(glm_feedback_reserve(&context, i+1, &first) == (uint32_t)(i+1)); assert(!first);
        glPauseTransformFeedback();
    }
    for (int i = 2; i >= 0; --i) {
        glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, names[i]);
        assert(context.xfb.active && context.xfb.paused);
        assert(context.xfb.capacity == (uint32_t)(i+3));
        assert(context.feedback_bindings[0].buffer == (GLuint)(i+1));
        glResumeTransformFeedback(); uint32_t first;
        assert(glm_feedback_reserve(&context, 1, &first) == 1); assert(first == (uint32_t)(i+1));
        glEndTransformFeedback(); assert(!context.xfb.active && !context.xfb.paused);
    }
    assert(query.counter == 9);
    for (int stream = 1; stream < 4; ++stream) assert(!nonzero[stream - 1].counter);
    for (int i = 0; i < 3; ++i) glDrawTransformFeedback(GL_POINTS, names[i]);
    assert(drawn == 9 && !error);
    glDeleteTransformFeedbacks(2, names+1);
    puts("CPU transform feedback object/default state, offsets, capacities and counts passed");
}
