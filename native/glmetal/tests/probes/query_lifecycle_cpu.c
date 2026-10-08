/* Real queries.c with CPU backend mocks. No OpenGL context or GPU execution. */
#include "../../src/queries.c"
#include <assert.h>
#include <stdio.h>

static struct glm_context context;
static GLenum error;
static unsigned released, occlusion_ends;
struct glm_context *glm_current(void) { return &context; }
void glm_error(struct glm_context *ctx, GLenum value) { (void)ctx; error = value; }
void *glm_table_get(struct glm_object_table *table, GLuint name)
{ return name && name < table->capacity ? table->items[name] : NULL; }
void glm_table_set(struct glm_object_table *table, GLuint name, void *value)
{
    if (!table->items) {
        table->capacity = 32;
        table->items = calloc(table->capacity, sizeof *table->items);
    }
    assert(name < table->capacity);
    table->items[name] = value;
}
GLuint glm_table_reserve(struct glm_object_table *table)
{
    GLuint name = table->next_name ? table->next_name : 1;
    while (glm_table_get(table, name)) ++name;
    table->next_name = name + 1;
    return name;
}
void glm_backend_release(void *buffer) { if (buffer) ++released; }
void glm_backend_occlusion_end(struct glm_context *ctx) { (void)ctx; ++occlusion_ends; }
uint64_t glm_backend_pending_serial(struct glm_context *ctx) { (void)ctx; return 1; }
uint64_t glm_backend_fence_insert(struct glm_context *ctx) { (void)ctx; return 1; }
bool glm_backend_fence_done(struct glm_context *ctx, uint64_t serial) { (void)ctx; (void)serial; return true; }
void glm_backend_fence_wait(struct glm_context *ctx, uint64_t serial) { (void)ctx; (void)serial; }
uint64_t glm_backend_buffer_read_u64(void *buffer, uint32_t offset) { (void)buffer; (void)offset; return 0; }

int main(void)
{
    GLuint name;
    glGenQueries(1, &name);
    glBeginQueryIndexed(GL_PRIMITIVES_GENERATED, 1, name);
    struct glm_query *old = context.indexed_queries[GLM_QUERY_PRIMITIVES][0];
    glm_query_add_segment(old, (void *)1, 0);
    glQueryCounter(name, GL_TIMESTAMP);
    assert(error == GL_INVALID_OPERATION && old->target == GL_PRIMITIVES_GENERATED);
    error = 0;
    glDeleteQueries(1, &name);
    assert(!glIsQuery(name) && old->active && old->deleted && !released);
    GLint current = 0;
    glGetQueryIndexediv(GL_PRIMITIVES_GENERATED, 1, GL_CURRENT_QUERY, &current);
    assert((GLuint)current == name);

    /* Reuse the deleted name for another object while the old object is active. */
    context.queries.next_name = name;
    GLuint reused;
    glGenQueries(1, &reused);
    assert(reused == name);
    glBeginQueryIndexed(GL_PRIMITIVES_GENERATED, 2, reused);
    struct glm_query *fresh = query_get(&context, reused);
    assert(fresh && fresh != old && context.indexed_queries[GLM_QUERY_PRIMITIVES][1] == fresh);
    glm_query_count_primitives(&context, 3, 6, 3);
    assert(!old->counter && !fresh->counter); /* No GS means nonzero streams stay empty. */
    glEndQueryIndexed(GL_PRIMITIVES_GENERATED, 1);
    assert(released == 1 && query_get(&context, reused) == fresh && fresh->active);
    glEndQueryIndexed(GL_PRIMITIVES_GENERATED, 2);
    glDeleteQueries(1, &reused);
    assert(!query_get(&context, reused));

    GLuint other;
    glGenQueries(1, &other);
    glBeginQuery(GL_SAMPLES_PASSED, other);
    struct glm_query *occlusion = context.active_queries[GLM_QUERY_OCCLUSION];
    glm_query_add_segment(occlusion, (void *)2, 0);
    glDeleteQueries(1, &other);
    assert(!occlusion_ends && released == 1);
    glEndQuery(GL_SAMPLES_PASSED);
    assert(occlusion_ends == 1 && released == 2);

    /* Context cleanup owns both named queries and deleted active queries. */
    glGenQueries(1, &other);
    glBeginQueryIndexed(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, 3, other);
    glm_query_add_segment(query_get(&context, other), (void *)3, 0);
    glDeleteQueries(1, &other);
    glGenQueries(1, &other);
    glBeginQuery(GL_PRIMITIVES_GENERATED, other);
    glm_query_add_segment(query_get(&context, other), (void *)4, 0);
    glm_queries_destroy(&context);
    assert(released == 4 && !context.queries.items);
    assert(!context.active_queries[GLM_QUERY_PRIMITIVES]);
    assert(!context.indexed_queries[GLM_QUERY_FEEDBACK][2]);
    assert(!error);
    puts("CPU query lifecycle: deferred deletion, name reuse, active timestamp rejection, stream isolation and cleanup passed");
}
