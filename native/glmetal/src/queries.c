/* Query objects: occlusion through Metal visibility results, elapsed time
 * and timestamps from the host clock, primitive counts on the CPU. */
#include "queries.h"

#include <mach/mach_time.h>
#include <stdlib.h>
#include <string.h>

/* Newer than Apple's headers. */
#ifndef GL_ANY_SAMPLES_PASSED_CONSERVATIVE
#define GL_ANY_SAMPLES_PASSED_CONSERVATIVE 0x8D6A
#endif
#ifndef GL_QUERY_RESULT_NO_WAIT
#define GL_QUERY_RESULT_NO_WAIT 0x9194
#endif

GLM_HIDDEN uint64_t glm_backend_fence_insert(struct glm_context *ctx);
GLM_HIDDEN bool glm_backend_fence_done(struct glm_context *ctx, uint64_t serial);
GLM_HIDDEN void glm_backend_fence_wait(struct glm_context *ctx, uint64_t serial);

uint64_t glm_now_ns(void)
{
    static mach_timebase_info_data_t base;
    if (!base.denom) mach_timebase_info(&base);
    return mach_absolute_time() * base.numer / base.denom;
}

static int query_class(GLenum target)
{
    switch (target) {
    case GL_SAMPLES_PASSED: case GL_ANY_SAMPLES_PASSED: case GL_ANY_SAMPLES_PASSED_CONSERVATIVE: return GLM_QUERY_OCCLUSION;
    case GL_TIME_ELAPSED: return GLM_QUERY_TIME;
    case GL_PRIMITIVES_GENERATED: return GLM_QUERY_PRIMITIVES;
    case GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN: return GLM_QUERY_FEEDBACK;
    default: return -1;
    }
}

static struct glm_query *query_get(struct glm_context *ctx, GLuint name)
{
    return name ? glm_table_get(&ctx->queries, name) : NULL;
}

void glm_query_add_segment(struct glm_query *q, void *buffer, uint32_t offset)
{
    if (q->segment_count == q->segment_capacity) {
        q->segment_capacity = q->segment_capacity ? q->segment_capacity * 2 : 4;
        q->buffers = realloc(q->buffers, (size_t)q->segment_capacity * sizeof *q->buffers);
        q->offsets = realloc(q->offsets, (size_t)q->segment_capacity * sizeof *q->offsets);
    }
    q->buffers[q->segment_count] = buffer;
    q->offsets[q->segment_count] = offset;
    ++q->segment_count;
}

static void clear_segments(struct glm_query *q)
{
    for (int i = 0; i < q->segment_count; ++i) glm_backend_release(q->buffers[i]);
    q->segment_count = 0;
}

static void query_free(struct glm_query *q)
{
    clear_segments(q);
    free(q->buffers);
    free(q->offsets);
    free(q);
}

/* Called after the backend has drained pending work. Deleted active queries
   no longer belong to the name table, so release them from their active slots. */
void glm_queries_destroy(struct glm_context *ctx)
{
    for (int c = 0; c < GLM_QUERY_CLASSES; ++c) {
        for (int index = 0; index < 4; ++index) {
            struct glm_query **slot = index ? &ctx->indexed_queries[c][index - 1] : &ctx->active_queries[c];
            struct glm_query *q = *slot;
            *slot = NULL;
            if (q && q->deleted) query_free(q);
        }
    }
    for (GLuint i = 0; i < ctx->queries.capacity; ++i)
        if (ctx->queries.items[i]) query_free(ctx->queries.items[i]);
    free(ctx->queries.items);
    memset(&ctx->queries, 0, sizeof ctx->queries);
}

GLM_EXPORT void glGenQueries(GLsizei n, GLuint *names)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; ++i) names[i] = glm_table_reserve(&ctx->queries);
}

GLM_EXPORT void glDeleteQueries(GLsizei n, const GLuint *names)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; ++i) {
        struct glm_query *q = query_get(ctx, names[i]);
        if (!q) continue;
        glm_table_set(&ctx->queries, names[i], NULL);
        if (q->active) q->deleted = true;
        else query_free(q);
    }
}

GLM_EXPORT GLboolean glIsQuery(GLuint name) { GLM_CONTEXT(ctx, GL_FALSE); return query_get(ctx, name) != NULL; }

static void begin_query(struct glm_context *ctx, GLenum target, GLuint index, GLuint name)
{
    int c = query_class(target);
    if (c < 0) return glm_error(ctx, GL_INVALID_ENUM);
    if (index >= 4 || (index && c != GLM_QUERY_PRIMITIVES && c != GLM_QUERY_FEEDBACK))
        return glm_error(ctx, GL_INVALID_VALUE);
    struct glm_query **slot = index ? &ctx->indexed_queries[c][index - 1] : &ctx->active_queries[c];
    if (!name || *slot) return glm_error(ctx, GL_INVALID_OPERATION);
    struct glm_query *q = query_get(ctx, name);
    if (!q) {
        q = calloc(1, sizeof *q);
        q->name = name;
        glm_table_set(&ctx->queries, name, q);
    } else if (q->active || (q->target && q->target != target)) {
        return glm_error(ctx, GL_INVALID_OPERATION);
    }
    q->target = target;
    q->index = index;
    q->active = true;
    q->counter = c == GLM_QUERY_TIME ? glm_now_ns() : 0;
    q->pass_id = 0;
    clear_segments(q);
    *slot = q;
}

static void end_query(struct glm_context *ctx, GLenum target, GLuint index)
{
    int c = query_class(target);
    if (c < 0) return glm_error(ctx, GL_INVALID_ENUM);
    if (index >= 4 || (index && c != GLM_QUERY_PRIMITIVES && c != GLM_QUERY_FEEDBACK))
        return glm_error(ctx, GL_INVALID_VALUE);
    struct glm_query **slot = index ? &ctx->indexed_queries[c][index - 1] : &ctx->active_queries[c];
    struct glm_query *q = *slot;
    if (!q || q->target != target) return glm_error(ctx, GL_INVALID_OPERATION);
    if (c == GLM_QUERY_OCCLUSION) glm_backend_occlusion_end(ctx);
    if (c == GLM_QUERY_TIME) q->counter = glm_now_ns() - q->counter;
    q->active = false;
    q->serial = glm_backend_pending_serial(ctx);
    *slot = NULL;
    if (q->deleted) query_free(q);
}

GLM_EXPORT void glBeginQuery(GLenum target, GLuint name)
{
    GLM_CONTEXT(ctx);
    begin_query(ctx, target, 0, name);
}

GLM_EXPORT void glEndQuery(GLenum target)
{
    GLM_CONTEXT(ctx);
    end_query(ctx, target, 0);
}

GLM_EXPORT void glQueryCounter(GLuint name, GLenum target)
{
    GLM_CONTEXT(ctx);
    if (target != GL_TIMESTAMP) return glm_error(ctx, GL_INVALID_ENUM);
    struct glm_query *q = query_get(ctx, name);
    if (q && q->active) return glm_error(ctx, GL_INVALID_OPERATION);
    if (!q) {
        if (!name) return glm_error(ctx, GL_INVALID_OPERATION);
        q = calloc(1, sizeof *q);
        q->name = name;
        glm_table_set(&ctx->queries, name, q);
    }
    q->target = GL_TIMESTAMP;
    q->counter = 0; /* Apple's timestamps are always 0 (GL_TIMESTAMP too) */
    q->serial = glm_backend_pending_serial(ctx);
    clear_segments(q);
}

void glm_query_count_primitives(struct glm_context *ctx, uint32_t metal_primitive, uint32_t n, uint32_t instances)
{
    struct glm_query *q = ctx->active_queries[GLM_QUERY_PRIMITIVES];
    if (!q) return;
    uint64_t count;
    switch (metal_primitive) { /* MTLPrimitiveType after GL primitive conversion */
    case 0: count = n; break;
    case 1: count = n / 2; break;
    case 2: count = n > 1 ? n - 1 : 0; break;
    case 3: count = n / 3; break;
    case 4: count = n > 2 ? n - 2 : 0; break;
    default: count = 0;
    }
    q->counter += count * (instances ? instances : 1);
}

static bool available(struct glm_context *ctx, struct glm_query *q)
{
    if (!q->segment_count) return true;
    if (glm_backend_fence_done(ctx, q->serial)) return true;
    /* The work must reach the GPU for the result to become available. */
    if (glm_backend_pending_serial(ctx) == q->serial) glm_backend_fence_insert(ctx);
    return glm_backend_fence_done(ctx, q->serial);
}

static uint64_t result(struct glm_context *ctx, struct glm_query *q)
{
    if (q->segment_count && !glm_backend_fence_done(ctx, q->serial)) {
        if (glm_backend_pending_serial(ctx) == q->serial) glm_backend_fence_insert(ctx);
        glm_backend_fence_wait(ctx, q->serial);
    }
    uint64_t value = q->counter;
    if (query_class(q->target) == GLM_QUERY_OCCLUSION) {
        value = 0;
        for (int i = 0; i < q->segment_count; ++i) value += glm_backend_buffer_read_u64(q->buffers[i], q->offsets[i]);
        if (q->target != GL_SAMPLES_PASSED) value = value != 0;
    }
    return value;
}

static bool object_value(GLuint name, GLenum pname, uint64_t *out)
{
    GLM_CONTEXT(ctx, false);
    struct glm_query *q = query_get(ctx, name);
    if (!q || q->active) {
        glm_error(ctx, GL_INVALID_OPERATION);
        return false;
    }
    switch (pname) {
    case GL_QUERY_RESULT: *out = result(ctx, q); return true;
    case GL_QUERY_RESULT_AVAILABLE: *out = available(ctx, q); return true;
    case GL_QUERY_RESULT_NO_WAIT:
        if (available(ctx, q)) *out = result(ctx, q);
        return true;
    }
    glm_error(ctx, GL_INVALID_ENUM);
    return false;
}

GLM_EXPORT void glGetQueryObjectiv(GLuint name, GLenum pname, GLint *params)
{
    uint64_t v = 0;
    if (object_value(name, pname, &v)) *params = v > 0x7fffffff ? 0x7fffffff : (GLint)v;
}
GLM_EXPORT void glGetQueryObjectuiv(GLuint name, GLenum pname, GLuint *params)
{
    uint64_t v = 0;
    if (object_value(name, pname, &v)) *params = v > 0xffffffffu ? 0xffffffffu : (GLuint)v;
}
GLM_EXPORT void glGetQueryObjecti64v(GLuint name, GLenum pname, GLint64 *params)
{
    uint64_t v = 0;
    if (object_value(name, pname, &v)) *params = (GLint64)v;
}
GLM_EXPORT void glGetQueryObjectui64v(GLuint name, GLenum pname, GLuint64 *params)
{
    uint64_t v = 0;
    if (object_value(name, pname, &v)) *params = v;
}

GLM_EXPORT void glGetQueryiv(GLenum target, GLenum pname, GLint *params)
{
    GLM_CONTEXT(ctx);
    if (target == GL_TIMESTAMP) {
        if (pname == GL_QUERY_COUNTER_BITS) *params = 64;
        else glm_error(ctx, GL_INVALID_ENUM);
        return;
    }
    int c = query_class(target);
    if (c < 0) return glm_error(ctx, GL_INVALID_ENUM);
    switch (pname) {
    case GL_CURRENT_QUERY: *params = ctx->active_queries[c] && ctx->active_queries[c]->target == target ? (GLint)ctx->active_queries[c]->name : 0; return;
    case GL_QUERY_COUNTER_BITS: *params = c == GLM_QUERY_OCCLUSION || c == GLM_QUERY_TIME ? 64 : 32; return;
    }
    glm_error(ctx, GL_INVALID_ENUM);
}

GLM_EXPORT void glGenQueriesARB(GLsizei n, GLuint *names) { glGenQueries(n, names); }
GLM_EXPORT void glDeleteQueriesARB(GLsizei n, const GLuint *names) { glDeleteQueries(n, names); }
GLM_EXPORT GLboolean glIsQueryARB(GLuint name) { return glIsQuery(name); }
GLM_EXPORT void glBeginQueryARB(GLenum target, GLuint name) { glBeginQuery(target, name); }
GLM_EXPORT void glEndQueryARB(GLenum target) { glEndQuery(target); }
GLM_EXPORT void glGetQueryivARB(GLenum target, GLenum pname, GLint *params) { glGetQueryiv(target, pname, params); }
GLM_EXPORT void glGetQueryObjectivARB(GLuint name, GLenum pname, GLint *params) { glGetQueryObjectiv(name, pname, params); }
GLM_EXPORT void glGetQueryObjectuivARB(GLuint name, GLenum pname, GLuint *params) { glGetQueryObjectuiv(name, pname, params); }
GLM_EXPORT void glGetQueryObjecti64vEXT(GLuint name, GLenum pname, GLint64 *params) { glGetQueryObjecti64v(name, pname, params); }
GLM_EXPORT void glGetQueryObjectui64vEXT(GLuint name, GLenum pname, GLuint64 *params) { glGetQueryObjectui64v(name, pname, params); }

/* ---- conditional rendering (GL 3.0) and indexed queries (GL 4.0) -------- */

GLM_EXPORT void glBeginConditionalRender(GLuint name, GLenum mode)
{
    GLM_CONTEXT(ctx);
    struct glm_query *q = query_get(ctx, name);
    if (!q || q->active || query_class(q->target) != GLM_QUERY_OCCLUSION) return glm_error(ctx, GL_INVALID_OPERATION);
    if (ctx->conditional_render_active) return glm_error(ctx, GL_INVALID_OPERATION);
    ctx->conditional_render_active = true;
    /* The NO_WAIT modes may draw when the result is not known yet. */
    bool wait = mode == GL_QUERY_WAIT || mode == GL_QUERY_BY_REGION_WAIT;
    ctx->conditional_discard = (wait || available(ctx, q)) && result(ctx, q) == 0;
}

GLM_EXPORT void glEndConditionalRender(void)
{
    GLM_CONTEXT(ctx);
    if (!ctx->conditional_render_active) return glm_error(ctx, GL_INVALID_OPERATION);
    ctx->conditional_render_active = false;
    ctx->conditional_discard = false;
}

/* Indexed primitive queries keep independent active objects for each stream. */
GLM_EXPORT void glBeginQueryIndexed(GLenum target, GLuint index, GLuint name)
{
    GLM_CONTEXT(ctx);
    begin_query(ctx, target, index, name);
}

GLM_EXPORT void glEndQueryIndexed(GLenum target, GLuint index)
{
    GLM_CONTEXT(ctx);
    end_query(ctx, target, index);
}

GLM_EXPORT void glGetQueryIndexediv(GLenum target, GLuint index, GLenum pname, GLint *params)
{
    GLM_CONTEXT(ctx);
    int c = query_class(target);
    if (c < 0 && target != GL_TIMESTAMP) return glm_error(ctx, GL_INVALID_ENUM);
    if (index >= 4 || (index && c != GLM_QUERY_PRIMITIVES && c != GLM_QUERY_FEEDBACK))
        return glm_error(ctx, GL_INVALID_VALUE);
    if (!index) return glGetQueryiv(target, pname, params);
    struct glm_query *q = ctx->indexed_queries[c][index - 1];
    if (pname == GL_QUERY_COUNTER_BITS) *params = 32;
    else if (pname == GL_CURRENT_QUERY) *params = q ? (GLint)q->name : 0;
    else glm_error(ctx, GL_INVALID_ENUM);
}
