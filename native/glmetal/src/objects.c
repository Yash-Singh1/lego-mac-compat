/* Buffer, renderbuffer, framebuffer, vertex array and texture objects. */
#include "glm_internal.h"
#include "glm_apple_gets.h"

#include <stdlib.h>
#include <string.h>

volatile uint64_t glm_object_generation;

/* ---- buffers ------------------------------------------------------------- */

struct glm_buffer *glm_buffer_get(struct glm_context *ctx, GLuint name)
{
    return glm_table_get(&ctx->share->buffers, name);
}

GLM_EXPORT void glGenBuffers(GLsizei n, GLuint *names)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    pthread_mutex_lock(&ctx->share->lock);
    for (GLsizei i = 0; i < n; ++i) {
        GLuint name = glm_table_reserve(&ctx->share->buffers);
        struct glm_buffer *buffer = calloc(1, sizeof *buffer);
        buffer->name = name;
        buffer->apple_flushing_unmap = buffer->apple_serialized_modify = true;
        glm_table_set(&ctx->share->buffers, name, buffer);
        names[i] = name;
    }
    pthread_mutex_unlock(&ctx->share->lock);
}
GLM_EXPORT void glGenBuffersARB(GLsizei n, GLuint *names) { glGenBuffers(n, names); }

static struct glm_buffer *buffer_for_bind(struct glm_context *ctx, GLuint name)
{
    if (!name) return NULL;
    struct glm_buffer *buffer = glm_buffer_get(ctx, name);
    if (!buffer) {
        if (ctx->profile == GLM_PROFILE_CORE) {
            glm_error(ctx, GL_INVALID_OPERATION);
            return NULL;
        }
        /* Legacy GL creates the object on first bind of an unused name. */
        buffer = calloc(1, sizeof *buffer);
        buffer->name = name;
        buffer->apple_flushing_unmap = buffer->apple_serialized_modify = true;
        pthread_mutex_lock(&ctx->share->lock);
        glm_table_set(&ctx->share->buffers, name, buffer);
        pthread_mutex_unlock(&ctx->share->lock);
    }
    buffer->bound = true;
    return buffer;
}

static GLuint *buffer_binding(struct glm_context *ctx, GLenum target)
{
    switch (target) {
    case GL_ARRAY_BUFFER: return &ctx->array_buffer;
    case GL_ELEMENT_ARRAY_BUFFER: return &ctx->vao->element_buffer;
    case GL_PIXEL_PACK_BUFFER: return &ctx->pixel_pack_buffer;
    case GL_PIXEL_UNPACK_BUFFER: return &ctx->pixel_unpack_buffer;
    case GL_UNIFORM_BUFFER: return &ctx->uniform_buffer;
    case GL_COPY_READ_BUFFER: return &ctx->copy_read_buffer;
    case GL_COPY_WRITE_BUFFER: return &ctx->copy_write_buffer;
    case GL_TEXTURE_BUFFER: return &ctx->texture_buffer;
    case GL_TRANSFORM_FEEDBACK_BUFFER: return &ctx->transform_feedback_buffer;
    case GL_DRAW_INDIRECT_BUFFER: return &ctx->draw_indirect_buffer;
    default: return NULL;
    }
}

GLM_EXPORT void glBindBuffer(GLenum target, GLuint name)
{
    GLM_CONTEXT(ctx);
    GLuint *binding = buffer_binding(ctx, target);
    if (!binding) return glm_error(ctx, GL_INVALID_ENUM);
    if (name && !buffer_for_bind(ctx, name)) return;
    *binding = name;
}
GLM_EXPORT void glBindBufferARB(GLenum target, GLuint name) { glBindBuffer(target, name); }

GLM_EXPORT void glDeleteBuffers(GLsizei n, const GLuint *names)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; ++i) {
        struct glm_buffer *buffer = glm_buffer_get(ctx, names[i]);
        if (!buffer) continue;
        if (ctx->array_buffer == names[i]) ctx->array_buffer = 0;
        GLuint *bindings[] = {&ctx->pixel_pack_buffer, &ctx->pixel_unpack_buffer,
            &ctx->uniform_buffer, &ctx->copy_read_buffer, &ctx->copy_write_buffer,
            &ctx->texture_buffer, &ctx->transform_feedback_buffer, &ctx->draw_indirect_buffer};
        for (size_t b = 0; b < sizeof bindings / sizeof *bindings; ++b)
            if (*bindings[b] == names[i]) *bindings[b] = 0;
        for (size_t b = 0; b < sizeof ctx->uniform_bindings / sizeof *ctx->uniform_bindings; ++b)
            if (ctx->uniform_bindings[b].buffer == names[i])
                memset(&ctx->uniform_bindings[b], 0, sizeof ctx->uniform_bindings[b]);
        for (size_t b = 0; b < sizeof ctx->feedback_bindings / sizeof *ctx->feedback_bindings; ++b)
            if (ctx->feedback_bindings[b].buffer == names[i])
                memset(&ctx->feedback_bindings[b], 0, sizeof ctx->feedback_bindings[b]);
        if (ctx->vao->element_buffer == names[i]) ctx->vao->element_buffer = 0;
        for (int a = 0; a < GLM_MAX_ATTRIBS; ++a)
            if (ctx->vao->arrays[a].buffer == names[i]) {
                ctx->vao->arrays[a].buffer = 0;
                glm_vao_changed(ctx->vao);
            }
        __atomic_add_fetch(&glm_object_generation, 1, __ATOMIC_RELEASE);
        pthread_mutex_lock(&ctx->share->lock);
        glm_table_set(&ctx->share->buffers, names[i], NULL);
        pthread_mutex_unlock(&ctx->share->lock);
        glm_backend_buffer_retire(ctx, buffer);
        free(buffer);
    }
}
GLM_EXPORT void glDeleteBuffersARB(GLsizei n, const GLuint *names) { glDeleteBuffers(n, names); }

/* A generated name is a buffer object once bound (GL 2.1 2.9). */
GLM_EXPORT GLboolean glIsBuffer(GLuint name)
{
    GLM_CONTEXT(ctx, GL_FALSE);
    struct glm_buffer *buffer = glm_buffer_get(ctx, name);
    return buffer && buffer->bound;
}
GLM_EXPORT GLboolean glIsBufferARB(GLuint name) { return glIsBuffer(name); }

static struct glm_buffer *bound_buffer(struct glm_context *ctx, GLenum target)
{
    GLuint *binding = buffer_binding(ctx, target);
    if (!binding) {
        glm_error(ctx, GL_INVALID_ENUM);
        return NULL;
    }
    struct glm_buffer *buffer = glm_buffer_get(ctx, *binding);
    if (!buffer) glm_error(ctx, GL_INVALID_OPERATION);
    return buffer;
}

GLM_HIDDEN bool glm_buffer_usage_valid(GLenum usage)
{
    switch (usage) {
    case GL_STREAM_DRAW: case GL_STREAM_READ: case GL_STREAM_COPY:
    case GL_STATIC_DRAW: case GL_STATIC_READ: case GL_STATIC_COPY:
    case GL_DYNAMIC_DRAW: case GL_DYNAMIC_READ: case GL_DYNAMIC_COPY: return true;
    default: return false;
    }
}

GLM_EXPORT void glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage)
{
    GLM_CONTEXT(ctx);
    if (size < 0) return glm_error(ctx, GL_INVALID_VALUE);
    if (!glm_buffer_usage_valid(usage)) return glm_error(ctx, GL_INVALID_ENUM);
    struct glm_buffer *buffer = bound_buffer(ctx, target);
    if (!buffer) return;
    buffer->size = size;
    buffer->usage = usage;
    buffer->mapped = NULL;
    glm_backend_buffer_data(ctx, buffer, data);
}
GLM_EXPORT void glBufferDataARB(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage) { glBufferData(target, size, data, usage); }

/* glBufferData with storage the command stream filled on the calling thread. */
GLM_HIDDEN void glm_buffer_data_with_storage(struct glm_context *ctx, GLenum target, GLsizeiptr size, GLenum usage,
                                             void *storage)
{
    if (!glm_buffer_usage_valid(usage)) {
        glm_backend_release(storage);
        return glm_error(ctx, GL_INVALID_ENUM);
    }
    struct glm_buffer *buffer = bound_buffer(ctx, target);
    if (!buffer) {
        glm_backend_release(storage);
        return;
    }
    buffer->size = size;
    buffer->usage = usage;
    buffer->mapped = NULL;
    glm_backend_buffer_adopt(ctx, buffer, storage);
}

GLM_EXPORT void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data)
{
    GLM_CONTEXT(ctx);
    struct glm_buffer *buffer = bound_buffer(ctx, target);
    if (!buffer) return;
    if (offset < 0 || size < 0 || offset + size > buffer->size) return glm_error(ctx, GL_INVALID_VALUE);
    if (size && data) glm_backend_buffer_sub_data(ctx, buffer, offset, size, data);
}
GLM_EXPORT void glBufferSubDataARB(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data)
{ glBufferSubData(target, offset, size, data); }

GLM_EXPORT void glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, GLvoid *data)
{
    GLM_CONTEXT(ctx);
    struct glm_buffer *buffer = bound_buffer(ctx, target);
    if (!buffer) return;
    if (offset < 0 || size < 0 || offset + size > buffer->size) return glm_error(ctx, GL_INVALID_VALUE);
    glm_backend_flush(ctx, true);
    const uint8_t *contents = glm_backend_buffer_contents(ctx, buffer, false);
    if (contents) memcpy(data, contents + offset, (size_t)size);
}

static void *map_range(struct glm_context *ctx, struct glm_buffer *buffer, GLintptr offset, GLsizeiptr length,
                       GLbitfield access)
{
    if (buffer->mapped) {
        glm_error(ctx, GL_INVALID_OPERATION);
        return NULL;
    }
    if (offset < 0 || length < 0 || offset + length > buffer->size) {
        glm_error(ctx, GL_INVALID_VALUE);
        return NULL;
    }
    /* Unsynchronized maps (and Apple's non-serialized buffers) write into
       storage the GPU may still read. Invalidating maps get fresh storage
       when the GPU is busy with the range; the rest wait for it. */
    bool unsynchronized = (access & GL_MAP_UNSYNCHRONIZED_BIT) || !buffer->apple_serialized_modify;
    if (!unsynchronized && (access & GL_MAP_WRITE_BIT)) {
        bool invalidate = (access & (GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_INVALIDATE_RANGE_BIT)) != 0;
        glm_backend_buffer_prepare_write(ctx, buffer, offset, length, (access & GL_MAP_INVALIDATE_BUFFER_BIT) != 0,
                                         !invalidate);
    }
    uint8_t *contents = glm_backend_buffer_contents(ctx, buffer, (access & GL_MAP_WRITE_BIT) != 0);
    if (!contents) {
        glm_error(ctx, GL_OUT_OF_MEMORY);
        return NULL;
    }
    buffer->mapped = contents + offset;
    buffer->map_access = access;
    buffer->map_offset = offset;
    buffer->map_length = length;
    return buffer->mapped;
}

GLM_EXPORT GLvoid *glMapBuffer(GLenum target, GLenum access)
{
    GLM_CONTEXT(ctx, NULL);
    struct glm_buffer *buffer = bound_buffer(ctx, target);
    if (!buffer) return NULL;
    GLbitfield bits = access == GL_READ_ONLY ? GL_MAP_READ_BIT
                      : access == GL_WRITE_ONLY ? GL_MAP_WRITE_BIT
                      : GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
    if (access == GL_READ_ONLY || access == GL_READ_WRITE) glm_backend_flush(ctx, true);
    return map_range(ctx, buffer, 0, buffer->size, bits);
}
GLM_EXPORT GLvoid *glMapBufferARB(GLenum target, GLenum access) { return glMapBuffer(target, access); }

GLM_EXPORT GLvoid *glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access)
{
    GLM_CONTEXT(ctx, NULL);
    struct glm_buffer *buffer = bound_buffer(ctx, target);
    if (!buffer) return NULL;
    if (access & GL_MAP_READ_BIT) glm_backend_flush(ctx, true);
    return map_range(ctx, buffer, offset, length, access);
}

GLM_EXPORT GLboolean glUnmapBuffer(GLenum target)
{
    GLM_CONTEXT(ctx, GL_FALSE);
    struct glm_buffer *buffer = bound_buffer(ctx, target);
    if (!buffer) return GL_FALSE;
    if (!buffer->mapped) {
        glm_error(ctx, GL_INVALID_OPERATION);
        return GL_FALSE;
    }
    /* Storage is shared memory, so writes are already in place. */
    buffer->mapped = NULL;
    return GL_TRUE;
}
GLM_EXPORT GLboolean glUnmapBufferARB(GLenum target) { return glUnmapBuffer(target); }

GLM_EXPORT void glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) { (void)target; (void)offset; (void)length; }
GLM_EXPORT void glFlushMappedBufferRangeAPPLE(GLenum target, GLintptr offset, GLsizeiptr size) { (void)target; (void)offset; (void)size; }

GLM_EXPORT void glBufferParameteriAPPLE(GLenum target, GLenum pname, GLint param)
{
    GLM_CONTEXT(ctx);
    struct glm_buffer *buffer = bound_buffer(ctx, target);
    if (!buffer) return;
    if (pname == GL_BUFFER_SERIALIZED_MODIFY_APPLE) buffer->apple_serialized_modify = param != 0;
    else if (pname == GL_BUFFER_FLUSHING_UNMAP_APPLE) buffer->apple_flushing_unmap = param != 0;
    else glm_error(ctx, GL_INVALID_ENUM);
}

GLM_EXPORT void glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params)
{
    GLM_CONTEXT(ctx);
    struct glm_buffer *buffer = bound_buffer(ctx, target);
    if (!buffer) return;
    switch (pname) {
    case GL_BUFFER_SIZE: *params = (GLint)buffer->size; break;
    case GL_BUFFER_USAGE: *params = (GLint)buffer->usage; break;
    case GL_BUFFER_MAPPED: *params = buffer->mapped != NULL; break;
    case GL_BUFFER_ACCESS: {
        /* The glMapBuffer access of the current map (GL_READ_WRITE unmapped). */
        GLbitfield rw = buffer->mapped ? buffer->map_access & (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT) : 0;
        *params = rw == GL_MAP_READ_BIT ? GL_READ_ONLY : rw == GL_MAP_WRITE_BIT ? GL_WRITE_ONLY : GL_READ_WRITE;
        break;
    }
    case GL_BUFFER_ACCESS_FLAGS: *params = buffer->mapped ? (GLint)buffer->map_access : 0; break;
    case GL_BUFFER_MAP_OFFSET: *params = buffer->mapped ? (GLint)buffer->map_offset : 0; break;
    case GL_BUFFER_MAP_LENGTH: *params = buffer->mapped ? (GLint)buffer->map_length : 0; break;
    default: glm_error(ctx, GL_INVALID_ENUM);
    }
}
GLM_EXPORT void glGetBufferParameterivARB(GLenum target, GLenum pname, GLint *params) { glGetBufferParameteriv(target, pname, params); }

GLM_EXPORT void glGetBufferPointerv(GLenum target, GLenum pname, GLvoid **params)
{
    GLM_CONTEXT(ctx);
    struct glm_buffer *buffer = bound_buffer(ctx, target);
    if (!buffer) return;
    if (pname != GL_BUFFER_MAP_POINTER) return glm_error(ctx, GL_INVALID_ENUM);
    *params = buffer->mapped;
}

GLM_EXPORT void glCopyBufferSubData(GLenum read, GLenum write, GLintptr read_offset, GLintptr write_offset, GLsizeiptr size)
{
    GLM_CONTEXT(ctx);
    struct glm_buffer *src = bound_buffer(ctx, read), *dst = bound_buffer(ctx, write);
    if (!src || !dst) return;
    if (read_offset < 0 || write_offset < 0 || size < 0 || read_offset + size > src->size || write_offset + size > dst->size)
        return glm_error(ctx, GL_INVALID_VALUE);
    glm_backend_flush(ctx, true);
    const uint8_t *from = glm_backend_buffer_contents(ctx, src, false);
    uint8_t *copy = malloc((size_t)size);
    if (!from || !copy) return free(copy);
    memcpy(copy, from + read_offset, (size_t)size);
    glm_backend_buffer_sub_data(ctx, dst, write_offset, size, copy);
    free(copy);
}

/* ---- renderbuffers ------------------------------------------------------- */

struct glm_renderbuffer *glm_renderbuffer_get(struct glm_context *ctx, GLuint name)
{
    return glm_table_get(&ctx->share->renderbuffers, name);
}

GLM_EXPORT void glGenRenderbuffers(GLsizei n, GLuint *names)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    pthread_mutex_lock(&ctx->share->lock);
    for (GLsizei i = 0; i < n; ++i) {
        GLuint name = glm_table_reserve(&ctx->share->renderbuffers);
        struct glm_renderbuffer *rb = calloc(1, sizeof *rb);
        rb->name = name;
        glm_table_set(&ctx->share->renderbuffers, name, rb);
        names[i] = name;
    }
    pthread_mutex_unlock(&ctx->share->lock);
}

GLM_EXPORT void glBindRenderbuffer(GLenum target, GLuint name)
{
    GLM_CONTEXT(ctx);
    if (target != GL_RENDERBUFFER) return glm_error(ctx, GL_INVALID_ENUM);
    if (name && !glm_renderbuffer_get(ctx, name)) {
        struct glm_renderbuffer *rb = calloc(1, sizeof *rb);
        rb->name = name;
        pthread_mutex_lock(&ctx->share->lock);
        glm_table_set(&ctx->share->renderbuffers, name, rb);
        pthread_mutex_unlock(&ctx->share->lock);
    }
    ctx->renderbuffer_binding = name;
}

GLM_EXPORT void glDeleteRenderbuffers(GLsizei n, const GLuint *names)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; ++i) {
        struct glm_renderbuffer *rb = glm_renderbuffer_get(ctx, names[i]);
        if (!rb) continue;
        if (ctx->renderbuffer_binding == names[i]) ctx->renderbuffer_binding = 0;
        pthread_mutex_lock(&ctx->share->lock);
        glm_table_set(&ctx->share->renderbuffers, names[i], NULL);
        pthread_mutex_unlock(&ctx->share->lock);
        glm_detach_deleted(ctx, GL_RENDERBUFFER, names[i]);
        glm_backend_release(rb->backend);
        free(rb);
    }
}

/* Deleting an image detaches it from the bound framebuffers (GL 3.0
   section 4.4.2); other framebuffers keep the (now dangling) name. */
GLM_HIDDEN void glm_detach_deleted(struct glm_context *ctx, GLenum type, GLuint name)
{
    GLuint bound[2] = {ctx->draw_framebuffer, ctx->read_framebuffer};
    for (int k = 0; k < 2; ++k) {
        if (!bound[k] || (k == 1 && bound[1] == bound[0])) continue;
        struct glm_framebuffer *fb = glm_table_get(&ctx->framebuffers, bound[k]);
        if (!fb) continue;
        for (int i = 0; i < GLM_MAX_DRAW_BUFFERS + 2; ++i) {
            struct glm_attachment *a = i < GLM_MAX_DRAW_BUFFERS ? &fb->color[i] : i == GLM_MAX_DRAW_BUFFERS ? &fb->depth : &fb->stencil;
            if (a->type == type && a->name == name) *a = (struct glm_attachment){0};
        }
        glm_backend_framebuffer_changed(ctx);
    }
}

GLM_EXPORT GLboolean glIsRenderbuffer(GLuint name) { GLM_CONTEXT(ctx, GL_FALSE); return glm_renderbuffer_get(ctx, name) != NULL; }

/* The renderbuffer formats Apple's implementation accepts in this profile,
   with the sized format and bits it reports (tools/apple_get_sweep.c). */
static const struct glm_apple_renderbuffer *apple_renderbuffer(const struct glm_context *ctx, GLenum format)
{
    bool core = ctx->profile == GLM_PROFILE_CORE;
    const struct glm_apple_renderbuffer *t = core ? glm_apple_renderbuffers_core : glm_apple_renderbuffers_legacy;
    size_t lo = 0, hi = core ? sizeof glm_apple_renderbuffers_core / sizeof *t : sizeof glm_apple_renderbuffers_legacy / sizeof *t;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (t[mid].format == format) return &t[mid];
        if (t[mid].format < format) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

GLM_EXPORT void glRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internal_format, GLsizei width,
                                                 GLsizei height)
{
    GLM_CONTEXT(ctx);
    if (target != GL_RENDERBUFFER) return glm_error(ctx, GL_INVALID_ENUM);
    struct glm_renderbuffer *rb = glm_renderbuffer_get(ctx, ctx->renderbuffer_binding);
    if (!rb) return glm_error(ctx, GL_INVALID_OPERATION);
    const struct glm_apple_renderbuffer *reference = apple_renderbuffer(ctx, internal_format);
    if (!reference) return glm_error(ctx, GL_INVALID_ENUM);
    /* Storage: the reported format, else the closest GLMetal has (stencil
       indices of any size are 8 bits deep, as Apple reports). */
    struct glm_format_info info;
    bool depth = reference->bits[4] != 0, stencil = reference->bits[5] != 0;
    GLenum storage = reference->reported;
    if (!glm_format_lookup(storage, &info)) {
        storage = depth && stencil ? GL_DEPTH24_STENCIL8 : depth ? GL_DEPTH_COMPONENT32 : stencil ? GL_STENCIL_INDEX8 : internal_format;
        if (!glm_format_lookup(storage, &info)) return glm_error(ctx, GL_INVALID_ENUM);
    }
    if (width < 0 || height < 0 || width > 16384 || height > 16384) return glm_error(ctx, GL_INVALID_VALUE);
    rb->internal_format = reference->reported;
    memcpy(rb->bits, reference->bits, sizeof rb->bits);
    rb->width = width;
    rb->height = height;
    rb->samples = samples > 1 ? (samples <= 2 ? 2 : samples <= 4 ? 4 : 8) : 0;
    rb->metal_format = info.metal_format;
    glm_backend_renderbuffer_storage(ctx, rb);
    glm_backend_framebuffer_changed(ctx);
}

GLM_EXPORT void glRenderbufferStorage(GLenum target, GLenum internal_format, GLsizei width, GLsizei height)
{
    glRenderbufferStorageMultisample(target, 0, internal_format, width, height);
}

GLM_EXPORT void glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint *params)
{
    GLM_CONTEXT(ctx);
    struct glm_renderbuffer *rb = glm_renderbuffer_get(ctx, ctx->renderbuffer_binding);
    if (target != GL_RENDERBUFFER || !rb) return glm_error(ctx, GL_INVALID_OPERATION);
    switch (pname) {
    case GL_RENDERBUFFER_WIDTH: *params = rb->width; break;
    case GL_RENDERBUFFER_HEIGHT: *params = rb->height; break;
    case GL_RENDERBUFFER_INTERNAL_FORMAT: *params = rb->internal_format ? (GLint)rb->internal_format : GL_RGBA; break;
    case GL_RENDERBUFFER_SAMPLES: *params = rb->samples; break;
    case GL_RENDERBUFFER_RED_SIZE: *params = rb->bits[0]; break;
    case GL_RENDERBUFFER_GREEN_SIZE: *params = rb->bits[1]; break;
    case GL_RENDERBUFFER_BLUE_SIZE: *params = rb->bits[2]; break;
    case GL_RENDERBUFFER_ALPHA_SIZE: *params = rb->bits[3]; break;
    case GL_RENDERBUFFER_DEPTH_SIZE: *params = rb->bits[4]; break;
    case GL_RENDERBUFFER_STENCIL_SIZE: *params = rb->bits[5]; break;
    default: glm_error(ctx, GL_INVALID_ENUM); break;
    }
}

/* ---- framebuffers -------------------------------------------------------- */

struct glm_framebuffer *glm_framebuffer_get(struct glm_context *ctx, GLuint name)
{
    return name ? glm_table_get(&ctx->framebuffers, name) : &ctx->default_framebuffer;
}

GLM_EXPORT void glGenFramebuffers(GLsizei n, GLuint *names)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; ++i) {
        GLuint name = glm_table_reserve(&ctx->framebuffers);
        struct glm_framebuffer *fb = calloc(1, sizeof *fb);
        fb->name = name;
        fb->draw_buffers[0] = GL_COLOR_ATTACHMENT0;
        fb->read_buffer = GL_COLOR_ATTACHMENT0;
        glm_table_set(&ctx->framebuffers, name, fb);
        names[i] = name;
    }
}

GLM_EXPORT void glBindFramebuffer(GLenum target, GLuint name)
{
    GLM_CONTEXT(ctx);
    if (name && !glm_table_get(&ctx->framebuffers, name)) {
        struct glm_framebuffer *fb = calloc(1, sizeof *fb);
        fb->name = name;
        fb->draw_buffers[0] = GL_COLOR_ATTACHMENT0;
        fb->read_buffer = GL_COLOR_ATTACHMENT0;
        glm_table_set(&ctx->framebuffers, name, fb);
    }
    switch (target) {
    case GL_FRAMEBUFFER: ctx->draw_framebuffer = ctx->read_framebuffer = name; break;
    case GL_DRAW_FRAMEBUFFER: ctx->draw_framebuffer = name; break;
    case GL_READ_FRAMEBUFFER: ctx->read_framebuffer = name; break;
    default: return glm_error(ctx, GL_INVALID_ENUM);
    }
    glm_backend_framebuffer_changed(ctx);
}

GLM_EXPORT void glDeleteFramebuffers(GLsizei n, const GLuint *names)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; ++i) {
        struct glm_framebuffer *fb = glm_table_get(&ctx->framebuffers, names[i]);
        if (!fb) continue;
        if (ctx->draw_framebuffer == names[i]) ctx->draw_framebuffer = 0;
        if (ctx->read_framebuffer == names[i]) ctx->read_framebuffer = 0;
        glm_table_set(&ctx->framebuffers, names[i], NULL);
        free(fb);
    }
    glm_backend_framebuffer_changed(ctx);
}

GLM_EXPORT GLboolean glIsFramebuffer(GLuint name) { GLM_CONTEXT(ctx, GL_FALSE); return name && glm_table_get(&ctx->framebuffers, name); }

static struct glm_framebuffer *target_framebuffer(struct glm_context *ctx, GLenum target)
{
    GLuint name = target == GL_READ_FRAMEBUFFER ? ctx->read_framebuffer : ctx->draw_framebuffer;
    if (!name) {
        glm_error(ctx, GL_INVALID_OPERATION);
        return NULL;
    }
    return glm_table_get(&ctx->framebuffers, name);
}

static struct glm_attachment *attachment_slot(struct glm_context *ctx, struct glm_framebuffer *fb, GLenum attachment,
                                              struct glm_attachment **second)
{
    *second = NULL;
    if (attachment >= GL_COLOR_ATTACHMENT0 && attachment < GL_COLOR_ATTACHMENT0 + GLM_MAX_DRAW_BUFFERS)
        return &fb->color[attachment - GL_COLOR_ATTACHMENT0];
    if (attachment == GL_DEPTH_ATTACHMENT) return &fb->depth;
    if (attachment == GL_STENCIL_ATTACHMENT) return &fb->stencil;
    if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
        *second = &fb->stencil;
        return &fb->depth;
    }
    glm_error(ctx, GL_INVALID_ENUM);
    return NULL;
}

static void attach(struct glm_context *ctx, GLenum target, GLenum attachment, GLenum type, GLuint name, GLint level,
                   GLint layer, GLenum face)
{
    struct glm_framebuffer *fb = target_framebuffer(ctx, target);
    if (!fb) return;
    struct glm_attachment *second, *slot = attachment_slot(ctx, fb, attachment, &second);
    if (!slot) return;
    struct glm_attachment value = {name ? type : GL_NONE, name, level, layer, face};
    *slot = value;
    if (second) *second = value;
    glm_backend_framebuffer_changed(ctx);
}

GLM_EXPORT void glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum rb_target, GLuint name)
{
    GLM_CONTEXT(ctx);
    (void)rb_target;
    attach(ctx, target, attachment, GL_RENDERBUFFER, name, 0, 0, 0);
}

GLM_EXPORT void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint name, GLint level)
{
    GLM_CONTEXT(ctx);
    GLenum face = textarget >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && textarget <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? textarget : 0;
    attach(ctx, target, attachment, GL_TEXTURE, name, level, 0, face);
}

GLM_EXPORT void glFramebufferTexture1D(GLenum target, GLenum attachment, GLenum textarget, GLuint name, GLint level)
{ (void)textarget; GLM_CONTEXT(ctx); attach(ctx, target, attachment, GL_TEXTURE, name, level, 0, 0); }
GLM_EXPORT void glFramebufferTexture3D(GLenum target, GLenum attachment, GLenum textarget, GLuint name, GLint level, GLint layer)
{ (void)textarget; GLM_CONTEXT(ctx); attach(ctx, target, attachment, GL_TEXTURE, name, level, layer, 0); }
GLM_EXPORT void glFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint name, GLint level, GLint layer)
{ GLM_CONTEXT(ctx); attach(ctx, target, attachment, GL_TEXTURE, name, level, layer, 0); }
GLM_EXPORT void glFramebufferTexture(GLenum target, GLenum attachment, GLuint name, GLint level)
{
    GLM_CONTEXT(ctx);
    attach(ctx, target, attachment, GL_TEXTURE, name, level, 0, 0);
    struct glm_texture *t = name ? glm_texture_get(ctx, name) : NULL;
    GLenum type = t ? t->target : 0;
    if (!(type == GL_TEXTURE_2D_ARRAY || type == GL_TEXTURE_CUBE_MAP || type == GL_TEXTURE_CUBE_MAP_ARRAY ||
          type == GL_TEXTURE_3D || type == GL_TEXTURE_1D_ARRAY || type == GL_TEXTURE_2D_MULTISAMPLE_ARRAY))
        return;
    struct glm_framebuffer *fb = target_framebuffer(ctx, target);
    struct glm_attachment *second, *slot = fb ? attachment_slot(ctx, fb, attachment, &second) : NULL;
    if (slot) slot->layered = true;
    if (slot && second) second->layered = true;
}

GLM_HIDDEN GLenum glm_framebuffer_status(struct glm_context *ctx, struct glm_framebuffer *fb);

enum { KIND_NONE, KIND_COLOR, KIND_COLOR_UNSUPPORTED, KIND_DEPTH, KIND_STENCIL, KIND_DEPTH_STENCIL };

/* What an attachment's format can be attached as. Alpha, luminance and
   intensity colour formats attach but Apple's framebuffers do not support
   them. */
static int format_kind(GLenum format)
{
    switch (format) {
    case GL_ALPHA: case GL_ALPHA4: case GL_ALPHA8: case GL_ALPHA12: case GL_ALPHA16: case GL_LUMINANCE: case GL_LUMINANCE4:
    case GL_LUMINANCE8: case GL_LUMINANCE12: case GL_LUMINANCE16: case GL_LUMINANCE_ALPHA: case GL_LUMINANCE4_ALPHA4:
    case GL_LUMINANCE6_ALPHA2: case GL_LUMINANCE8_ALPHA8: case GL_LUMINANCE12_ALPHA4: case GL_LUMINANCE12_ALPHA12:
    case GL_LUMINANCE16_ALPHA16: case GL_INTENSITY: case GL_INTENSITY4: case GL_INTENSITY8: case GL_INTENSITY12:
    case GL_INTENSITY16: case GL_SLUMINANCE: case GL_SLUMINANCE8: case GL_SLUMINANCE_ALPHA: case GL_SLUMINANCE8_ALPHA8:
    case GL_ALPHA16F_ARB: case GL_ALPHA32F_ARB: case GL_LUMINANCE16F_ARB: case GL_LUMINANCE32F_ARB:
    case GL_LUMINANCE_ALPHA16F_ARB: case GL_LUMINANCE_ALPHA32F_ARB: case GL_INTENSITY16F_ARB: case GL_INTENSITY32F_ARB:
    case GL_RGB9_E5:
        return KIND_COLOR_UNSUPPORTED;
    case GL_STENCIL_INDEX: case GL_STENCIL_INDEX1: case GL_STENCIL_INDEX4: case GL_STENCIL_INDEX8: case GL_STENCIL_INDEX16:
        return KIND_STENCIL;
    }
    /* EXT_texture_integer's alpha, intensity, luminance and luminance-alpha
       formats: 0x8D70 + 6n + 2..5 for each of 32UI, 16UI, 8UI, 32I, 16I, 8I. */
    if (format >= 0x8D70 && format <= 0x8D93 && (format - 0x8D70) % 6 >= 2) return KIND_COLOR_UNSUPPORTED;
    struct glm_format_info info;
    if (!glm_format_lookup(format, &info)) return KIND_NONE;
    if (info.depth && info.stencil) return KIND_DEPTH_STENCIL;
    if (info.depth) return KIND_DEPTH;
    if (info.stencil) return KIND_STENCIL;
    return KIND_COLOR;
}

/* The attachment's internal format and samples, or 0 without an image. */
static GLenum attachment_format(struct glm_context *ctx, const struct glm_attachment *a, int *samples)
{
    *samples = 0;
    if (a->type == GL_RENDERBUFFER) {
        struct glm_renderbuffer *rb = glm_renderbuffer_get(ctx, a->name);
        if (!rb || !rb->internal_format || !rb->width || !rb->height) return 0;
        *samples = rb->samples;
        return rb->internal_format;
    }
    if (a->type == GL_TEXTURE) {
        struct glm_texture *t = glm_texture_get(ctx, a->name);
        if (!t || a->level < 0 || a->level >= GLM_MAX_LEVELS) return 0;
        int face = a->face >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && a->face <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z
                       ? (int)(a->face - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
        const struct glm_texture_level *l = &t->levels[face][a->level];
        if (!l->defined || !l->width) return 0;
        /* A layer past the image's layers attaches nothing. */
        if (!a->layered) {
            GLint layers = t->target == GL_TEXTURE_3D || t->target == GL_TEXTURE_2D_ARRAY || t->target == GL_TEXTURE_CUBE_MAP_ARRAY
                               ? l->depth
                           : t->target == GL_TEXTURE_1D_ARRAY ? l->height : 1;
            if (a->layer < 0 || a->layer >= (layers > 1 ? layers : 1)) return 0;
        }
        *samples = t->samples;
        return l->internal_format;
    }
    return 0;
}

/* Completeness as Apple's implementation judges it: attachments of the
   right kind, then (legacy profile only) the draw and read buffers, then
   formats it cannot render to, then sizes and images (the backend). */
GLM_HIDDEN GLenum glm_framebuffer_check(struct glm_context *ctx, struct glm_framebuffer *fb)
{
    bool any = false, unsupported = false;
    int samples = -1;
    bool sample_mismatch = false;
    for (int i = 0; i < GLM_MAX_DRAW_BUFFERS + 2; ++i) {
        const struct glm_attachment *a = i < GLM_MAX_DRAW_BUFFERS ? &fb->color[i] : i == GLM_MAX_DRAW_BUFFERS ? &fb->depth : &fb->stencil;
        if (a->type == GL_NONE) continue;
        int s;
        GLenum format = attachment_format(ctx, a, &s);
        if (!format) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        /* Textures: what Apple reports for the format at this point. */
        const struct glm_apple_texture *ref = a->type == GL_TEXTURE ? glm_apple_texture_format(ctx, format) : NULL;
        if (ref) {
            GLenum alone = ref->status[i < GLM_MAX_DRAW_BUFFERS ? 0 : i == GLM_MAX_DRAW_BUFFERS ? 1 : 2];
            if (alone == GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT) return alone;
            unsupported |= alone == GL_FRAMEBUFFER_UNSUPPORTED;
            if (samples >= 0 && s != samples) sample_mismatch = true;
            samples = s;
            any = true;
            continue;
        }
        int kind = format_kind(format);
        if (i < GLM_MAX_DRAW_BUFFERS) {
            if (kind != KIND_COLOR && kind != KIND_COLOR_UNSUPPORTED) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
            unsupported |= kind == KIND_COLOR_UNSUPPORTED;
        } else if (i == GLM_MAX_DRAW_BUFFERS) {
            if (kind != KIND_DEPTH && kind != KIND_DEPTH_STENCIL) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        } else if (kind != KIND_STENCIL && kind != KIND_DEPTH_STENCIL) {
            return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        }
        if (samples >= 0 && s != samples) sample_mismatch = true;
        samples = s;
        any = true;
    }
    if (!any) return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
    if (ctx->profile != GLM_PROFILE_CORE) {
        for (int i = 0; i < GLM_MAX_DRAW_BUFFERS; ++i) {
            GLenum buffer = fb->draw_buffers[i];
            if (buffer >= GL_COLOR_ATTACHMENT0 && buffer < GL_COLOR_ATTACHMENT0 + GLM_MAX_DRAW_BUFFERS &&
                fb->color[buffer - GL_COLOR_ATTACHMENT0].type == GL_NONE)
                return GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER;
        }
        GLenum read = fb->read_buffer;
        if (read >= GL_COLOR_ATTACHMENT0 && read < GL_COLOR_ATTACHMENT0 + GLM_MAX_DRAW_BUFFERS &&
            fb->color[read - GL_COLOR_ATTACHMENT0].type == GL_NONE)
            return GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER;
    }
    if (unsupported) return GL_FRAMEBUFFER_UNSUPPORTED;
    if (sample_mismatch) return GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE;
    return glm_framebuffer_status(ctx, fb);
}

GLM_HIDDEN GLenum glm_read_buffer_format(struct glm_context *ctx, bool *depth, bool *stencil)
{
    if (!ctx->read_framebuffer) {
        *depth = !ctx->default_bits_known || ctx->default_depth_bits > 0;
        *stencil = !ctx->default_bits_known || ctx->default_stencil_bits > 0;
        struct glm_framebuffer *fb0 = glm_framebuffer_get(ctx, 0);
        return fb0 && fb0->read_buffer == GL_NONE ? 0 : GL_RGBA8;
    }
    struct glm_framebuffer *fb = glm_table_get(&ctx->framebuffers, ctx->read_framebuffer);
    if (!fb) return 0;
    int samples;
    *depth = fb->depth.type != GL_NONE && attachment_format(ctx, &fb->depth, &samples);
    *stencil = fb->stencil.type != GL_NONE && attachment_format(ctx, &fb->stencil, &samples);
    GLenum read = fb->read_buffer;
    if (read < GL_COLOR_ATTACHMENT0 || read >= GL_COLOR_ATTACHMENT0 + GLM_MAX_DRAW_BUFFERS) return 0;
    return attachment_format(ctx, &fb->color[read - GL_COLOR_ATTACHMENT0], &samples);
}

/* Framebuffer 0 exists once a drawable is attached (cgl.m); before that
   Apple reports it undefined and draws to it fail. */
GLM_HIDDEN GLenum glm_framebuffer_status_of(struct glm_context *ctx, GLuint name)
{
    if (!name) return ctx->presents ? GL_FRAMEBUFFER_COMPLETE : GL_FRAMEBUFFER_UNDEFINED;
    return glm_framebuffer_check(ctx, glm_table_get(&ctx->framebuffers, name));
}

/* Which colour buffer names glDrawBuffer, glDrawBuffers and glReadBuffer
   take, as Apple's implementation answers (build/probes/drawbuf.c): the
   window-system buffers its pixel format has (never stereo or aux) on
   framebuffer 0, attachments below GL_MAX_COLOR_ATTACHMENTS on objects. */
GLM_HIDDEN GLenum glm_color_buffer_error(const struct glm_context *ctx, bool fbo, GLenum buffer, int call)
{
    if (buffer == GL_NONE) return GL_NO_ERROR;
    if (buffer >= GL_COLOR_ATTACHMENT0 && buffer <= GL_COLOR_ATTACHMENT15) {
        if (buffer >= GL_COLOR_ATTACHMENT0 + GLM_MAX_DRAW_BUFFERS) return GL_INVALID_ENUM;
        return fbo ? GL_NO_ERROR : GL_INVALID_OPERATION;
    }
    if (buffer >= 0x0409 && buffer <= 0x040C) return GL_INVALID_OPERATION; /* GL_AUX0..3 */
    if (buffer < GL_FRONT_LEFT || buffer > GL_FRONT_AND_BACK) return GL_INVALID_ENUM;
    /* GL_FRONT, GL_BACK, GL_LEFT, GL_RIGHT, GL_FRONT_AND_BACK name several buffers. */
    if (call == GLM_CALL_DRAW_BUFFERS && buffer >= GL_FRONT)
        return ctx->profile == GLM_PROFILE_CORE ? GL_INVALID_ENUM : GL_INVALID_OPERATION;
    if (call == GLM_CALL_READ_BUFFER && buffer == GL_FRONT_AND_BACK) return GL_INVALID_ENUM;
    if (fbo) return GL_INVALID_OPERATION;
    switch (buffer) {
    case GL_FRONT_LEFT: case GL_FRONT: case GL_LEFT: case GL_FRONT_AND_BACK: return GL_NO_ERROR;
    case GL_BACK_LEFT: case GL_BACK: return ctx->default_single_buffered ? GL_INVALID_OPERATION : GL_NO_ERROR;
    default: return GL_INVALID_OPERATION;
    }
}

GLM_HIDDEN GLenum glm_draw_buffers_error(const struct glm_context *ctx, bool fbo, GLsizei n, const GLenum *buffers)
{
    if (n < 0 || n > GLM_MAX_DRAW_BUFFERS) return GL_INVALID_VALUE;
    for (GLsizei i = 0; i < n; ++i) {
        GLenum error = glm_color_buffer_error(ctx, fbo, buffers[i], GLM_CALL_DRAW_BUFFERS);
        if (error) return error;
        for (GLsizei j = 0; j < i; ++j)
            if (buffers[i] != GL_NONE && buffers[j] == buffers[i]) return GL_INVALID_OPERATION;
    }
    return GL_NO_ERROR;
}

GLM_EXPORT GLenum glCheckFramebufferStatus(GLenum target)
{
    GLM_CONTEXT(ctx, 0);
    if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER && target != GL_READ_FRAMEBUFFER) {
        glm_error(ctx, GL_INVALID_ENUM);
        return 0;
    }
    return glm_framebuffer_status_of(ctx, target == GL_READ_FRAMEBUFFER ? ctx->read_framebuffer : ctx->draw_framebuffer);
}

GLM_EXPORT void glDrawBuffers(GLsizei n, const GLenum *buffers)
{
    GLM_CONTEXT(ctx);
    GLenum error = glm_draw_buffers_error(ctx, ctx->draw_framebuffer != 0, n, buffers);
    if (error) return glm_error(ctx, error);
    struct glm_framebuffer *fb = glm_framebuffer_get(ctx, ctx->draw_framebuffer);
    for (int i = 0; i < GLM_MAX_DRAW_BUFFERS; ++i) fb->draw_buffers[i] = i < n ? buffers[i] : GL_NONE;
    glm_backend_framebuffer_changed(ctx);
}
GLM_EXPORT void glDrawBuffersARB(GLsizei n, const GLenum *buffers) { glDrawBuffers(n, buffers); }

GLM_EXPORT void glDrawBuffer(GLenum buffer)
{
    GLM_CONTEXT(ctx);
    GLenum error = glm_color_buffer_error(ctx, ctx->draw_framebuffer != 0, buffer, GLM_CALL_DRAW_BUFFER);
    if (error) return glm_error(ctx, error);
    struct glm_framebuffer *fb = glm_framebuffer_get(ctx, ctx->draw_framebuffer);
    for (int i = 0; i < GLM_MAX_DRAW_BUFFERS; ++i) fb->draw_buffers[i] = GL_NONE;
    fb->draw_buffers[0] = buffer;
    glm_backend_framebuffer_changed(ctx);
}

GLM_EXPORT void glReadBuffer(GLenum buffer)
{
    GLM_CONTEXT(ctx);
    GLenum error = glm_color_buffer_error(ctx, ctx->read_framebuffer != 0, buffer, GLM_CALL_READ_BUFFER);
    if (error) return glm_error(ctx, error);
    glm_framebuffer_get(ctx, ctx->read_framebuffer)->read_buffer = buffer;
}

GLM_EXPORT void glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                                  GLbitfield mask, GLenum filter)
{
    GLM_CONTEXT(ctx);
    if (glm_framebuffer_status_of(ctx, ctx->read_framebuffer) != GL_FRAMEBUFFER_COMPLETE ||
        glm_framebuffer_status_of(ctx, ctx->draw_framebuffer) != GL_FRAMEBUFFER_COMPLETE)
        return glm_error(ctx, GL_INVALID_FRAMEBUFFER_OPERATION);
    GLint src[4] = {sx0, sy0, sx1, sy1}, dst[4] = {dx0, dy0, dx1, dy1};
    glm_backend_blit_framebuffer(ctx, src, dst, mask, filter);
}

static GLenum attachment_format(struct glm_context *ctx, const struct glm_attachment *a, int *samples);

GLM_HIDDEN const struct glm_apple_texture *glm_apple_texture_format(const struct glm_context *ctx, GLenum format)
{
    bool core = ctx->profile == GLM_PROFILE_CORE;
    const struct glm_apple_texture *t = core ? glm_apple_textures_core : glm_apple_textures_legacy;
    size_t lo = 0, hi = core ? sizeof glm_apple_textures_core / sizeof *t : sizeof glm_apple_textures_legacy / sizeof *t;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (t[mid].format == format) return &t[mid];
        if (t[mid].format < format) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

/* The component type GL reports for a renderbuffer of `format`. */
static GLenum renderbuffer_component_type(const struct glm_context *ctx, GLenum format)
{
    const struct glm_apple_texture *t = glm_apple_texture_format(ctx, format);
    if (t) return t->component;
    struct glm_format_info info;
    if (glm_format_lookup(format, &info) && info.integer) return GL_INT;
    return GL_UNSIGNED_NORMALIZED;
}

/* Default framebuffer attachments, as Apple's implementation reports them:
   the colour buffer that exists (back, or front when single-buffered), depth
   and stencil when the pixel format has them. */
static void default_attachment_parameter(struct glm_context *ctx, GLenum attachment, GLenum pname, GLint *params)
{
    bool single = ctx->default_single_buffered;
    int depth = ctx->default_bits_known ? ctx->default_depth_bits : 32;
    int stencil = ctx->default_bits_known ? ctx->default_stencil_bits : 0;
    bool exists, color = false;
    switch (attachment) {
    case GL_FRONT_LEFT: exists = single; color = true; break;
    case GL_BACK_LEFT: exists = !single; color = true; break;
    case GL_FRONT_RIGHT: case GL_BACK_RIGHT: exists = false; break;
    case GL_DEPTH: exists = depth > 0; break;
    case GL_STENCIL: exists = stencil > 0; break;
    case GL_AUX0: case GL_AUX1: case GL_AUX2: case GL_AUX3:
        if (ctx->profile == GLM_PROFILE_CORE) return glm_error(ctx, GL_INVALID_ENUM);
        exists = false;
        break;
    default: return glm_error(ctx, GL_INVALID_ENUM);
    }
    switch (pname) {
    case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE: *params = exists ? GL_FRAMEBUFFER_DEFAULT : GL_NONE; return;
    case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME: case GL_FRAMEBUFFER_ATTACHMENT_LAYERED: *params = 0; return;
    case GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING: *params = exists ? GL_LINEAR : 0; return;
    case GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE: *params = exists ? GL_UNSIGNED_NORMALIZED : 0; return;
    case GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE: case GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE:
    case GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE: case GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE:
        *params = exists && color ? 8 : 0; return;
    case GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE: *params = exists && attachment == GL_DEPTH ? depth : 0; return;
    case GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE: *params = exists && attachment == GL_STENCIL ? stencil : 0; return;
    }
    glm_error(ctx, GL_INVALID_ENUM);
}

GLM_EXPORT void glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint *params)
{
    GLM_CONTEXT(ctx);
    if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER && target != GL_READ_FRAMEBUFFER)
        return glm_error(ctx, GL_INVALID_ENUM);
    GLuint name = target == GL_READ_FRAMEBUFFER ? ctx->read_framebuffer : ctx->draw_framebuffer;
    if (!name) return default_attachment_parameter(ctx, attachment, pname, params);
    struct glm_framebuffer *fb = glm_table_get(&ctx->framebuffers, name);
    if (!fb) return glm_error(ctx, GL_INVALID_OPERATION);
    struct glm_attachment *second, *slot = attachment_slot(ctx, fb, attachment, &second);
    if (!slot) return;
    /* Depth-stencil: one image in both, or the query is ambiguous. */
    if (second && (second->type != slot->type || second->name != slot->name)) return glm_error(ctx, GL_INVALID_OPERATION);
    if (slot->type == GL_NONE) {
        switch (pname) {
        case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL: case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:
        case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER:
            return glm_error(ctx, GL_INVALID_ENUM);
        }
        *params = 0;
        return;
    }
    bool texture = slot->type == GL_TEXTURE;
    switch (pname) {
    case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE: *params = (GLint)slot->type; return;
    case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME: *params = (GLint)slot->name; return;
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL: case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER:
        if (!texture) return glm_error(ctx, GL_INVALID_ENUM);
        *params = pname == GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL ? slot->level
                  : pname == GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER ? slot->layer
                  : slot->face >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && slot->face <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? (GLint)slot->face : 0;
        return;
    case GL_FRAMEBUFFER_ATTACHMENT_LAYERED: *params = slot->layered; return;
    }
    /* Format properties: the attached image's, as Apple reports them. */
    GLenum format = 0;
    const uint8_t *rb_bits = NULL;
    if (texture) {
        int samples;
        format = attachment_format(ctx, slot, &samples);
    } else {
        struct glm_renderbuffer *rb = glm_renderbuffer_get(ctx, slot->name);
        if (rb) {
            format = rb->internal_format;
            rb_bits = rb->bits;
        }
    }
    const struct glm_apple_texture *t = format ? glm_apple_texture_format(ctx, format) : NULL;
    int channel = -1;
    switch (pname) {
    case GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING:
        *params = t ? t->encoding : GL_LINEAR;
        return;
    case GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE:
        *params = (GLint)(t ? t->component : renderbuffer_component_type(ctx, format));
        return;
    case GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE: channel = 0; break;
    case GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE: channel = 1; break;
    case GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE: channel = 2; break;
    case GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE: channel = 3; break;
    case GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE: channel = 4; break;
    case GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE: channel = 5; break;
    default: return glm_error(ctx, GL_INVALID_ENUM);
    }
    *params = rb_bits ? rb_bits[channel] : t ? t->attachment[channel] : 0;
}

/* EXT_framebuffer_object names: the same objects. */
GLM_EXPORT void glGenFramebuffersEXT(GLsizei n, GLuint *names) { glGenFramebuffers(n, names); }
GLM_EXPORT void glBindFramebufferEXT(GLenum target, GLuint name) { glBindFramebuffer(target, name); }
GLM_EXPORT void glDeleteFramebuffersEXT(GLsizei n, const GLuint *names) { glDeleteFramebuffers(n, names); }
GLM_EXPORT GLboolean glIsFramebufferEXT(GLuint name) { return glIsFramebuffer(name); }
GLM_EXPORT GLenum glCheckFramebufferStatusEXT(GLenum target) { return glCheckFramebufferStatus(target); }
GLM_EXPORT void glFramebufferTexture2DEXT(GLenum t, GLenum a, GLenum tt, GLuint n, GLint l) { glFramebufferTexture2D(t, a, tt, n, l); }
GLM_EXPORT void glFramebufferTexture1DEXT(GLenum t, GLenum a, GLenum tt, GLuint n, GLint l) { glFramebufferTexture1D(t, a, tt, n, l); }
GLM_EXPORT void glFramebufferTexture3DEXT(GLenum t, GLenum a, GLenum tt, GLuint n, GLint l, GLint z) { glFramebufferTexture3D(t, a, tt, n, l, z); }
GLM_EXPORT void glFramebufferTextureLayerEXT(GLenum t, GLenum a, GLuint n, GLint l, GLint z) { glFramebufferTextureLayer(t, a, n, l, z); }
GLM_EXPORT void glFramebufferRenderbufferEXT(GLenum t, GLenum a, GLenum rt, GLuint n) { glFramebufferRenderbuffer(t, a, rt, n); }
GLM_EXPORT void glGenRenderbuffersEXT(GLsizei n, GLuint *names) { glGenRenderbuffers(n, names); }
GLM_EXPORT void glBindRenderbufferEXT(GLenum target, GLuint name) { glBindRenderbuffer(target, name); }
GLM_EXPORT void glDeleteRenderbuffersEXT(GLsizei n, const GLuint *names) { glDeleteRenderbuffers(n, names); }
GLM_EXPORT GLboolean glIsRenderbufferEXT(GLuint name) { return glIsRenderbuffer(name); }
GLM_EXPORT void glRenderbufferStorageEXT(GLenum t, GLenum f, GLsizei w, GLsizei h) { glRenderbufferStorage(t, f, w, h); }
GLM_EXPORT void glRenderbufferStorageMultisampleEXT(GLenum t, GLsizei s, GLenum f, GLsizei w, GLsizei h) { glRenderbufferStorageMultisample(t, s, f, w, h); }
GLM_EXPORT void glGetRenderbufferParameterivEXT(GLenum t, GLenum p, GLint *v) { glGetRenderbufferParameteriv(t, p, v); }
GLM_EXPORT void glGetFramebufferAttachmentParameterivEXT(GLenum t, GLenum a, GLenum p, GLint *v) { glGetFramebufferAttachmentParameteriv(t, a, p, v); }
GLM_EXPORT void glBlitFramebufferEXT(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, GLbitfield m, GLenum fl)
{ glBlitFramebuffer(a, b, c, d, e, f, g, h, m, fl); }

/* ---- vertex array objects ------------------------------------------------ */

GLM_EXPORT void glGenVertexArrays(GLsizei n, GLuint *names)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; ++i) {
        GLuint name = glm_table_reserve(&ctx->vertex_arrays);
        struct glm_vertex_array *vao = calloc(1, sizeof *vao);
        vao->name = name;
        vao->client_active_texture = GL_TEXTURE0;
        glm_vao_changed(vao);
        glm_table_set(&ctx->vertex_arrays, name, vao);
        names[i] = name;
    }
}
GLM_EXPORT void glGenVertexArraysAPPLE(GLsizei n, GLuint *names) { glGenVertexArrays(n, names); }

GLM_EXPORT void glBindVertexArray(GLuint name)
{
    GLM_CONTEXT(ctx);
    if (!name) {
        ctx->vao = &ctx->default_vao;
        return;
    }
    struct glm_vertex_array *vao = glm_table_get(&ctx->vertex_arrays, name);
    if (!vao) {
        if (ctx->profile == GLM_PROFILE_CORE) return glm_error(ctx, GL_INVALID_OPERATION);
        /* APPLE_vertex_array_object binds unused names. */
        vao = calloc(1, sizeof *vao);
        vao->name = name;
        vao->client_active_texture = GL_TEXTURE0;
        glm_vao_changed(vao);
        glm_table_set(&ctx->vertex_arrays, name, vao);
    }
    ctx->vao = vao;
}
GLM_EXPORT void glBindVertexArrayAPPLE(GLuint name) { glBindVertexArray(name); }

GLM_EXPORT void glDeleteVertexArrays(GLsizei n, const GLuint *names)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; ++i) {
        struct glm_vertex_array *vao = glm_table_get(&ctx->vertex_arrays, names[i]);
        if (!vao) continue;
        if (ctx->vao == vao) ctx->vao = &ctx->default_vao;
        glm_table_set(&ctx->vertex_arrays, names[i], NULL);
        free(vao);
    }
}
GLM_EXPORT void glDeleteVertexArraysAPPLE(GLsizei n, const GLuint *names) { glDeleteVertexArrays(n, names); }
GLM_EXPORT GLboolean glIsVertexArray(GLuint name) { GLM_CONTEXT(ctx, GL_FALSE); return name && glm_table_get(&ctx->vertex_arrays, name); }
GLM_EXPORT GLboolean glIsVertexArrayAPPLE(GLuint name) { return glIsVertexArray(name); }

/* ---- indexed bindings (uniform and transform feedback buffers) ----------- */

static struct glm_indexed_buffer *indexed_binding(struct glm_context *ctx, GLenum target, GLuint index)
{
    if (target == GL_UNIFORM_BUFFER && index < 80) return &ctx->uniform_bindings[index];
    if (target == GL_TRANSFORM_FEEDBACK_BUFFER && index < 4) return &ctx->feedback_bindings[index];
    return NULL;
}

GLM_EXPORT void glBindBufferRange(GLenum target, GLuint index, GLuint name, GLintptr offset, GLsizeiptr size)
{
    GLM_CONTEXT(ctx);
    struct glm_indexed_buffer *b = indexed_binding(ctx, target, index);
    if (!b) return glm_error(ctx, target == GL_UNIFORM_BUFFER || target == GL_TRANSFORM_FEEDBACK_BUFFER ? GL_INVALID_VALUE : GL_INVALID_ENUM);
    if (name && (size <= 0 || offset < 0)) return glm_error(ctx, GL_INVALID_VALUE);
    if (name && !buffer_for_bind(ctx, name)) return;
    *b = (struct glm_indexed_buffer){name, offset, size};
    *buffer_binding(ctx, target) = name;
}

GLM_EXPORT void glBindBufferBase(GLenum target, GLuint index, GLuint name)
{
    GLM_CONTEXT(ctx);
    struct glm_indexed_buffer *b = indexed_binding(ctx, target, index);
    if (!b) return glm_error(ctx, target == GL_UNIFORM_BUFFER || target == GL_TRANSFORM_FEEDBACK_BUFFER ? GL_INVALID_VALUE : GL_INVALID_ENUM);
    if (name && !buffer_for_bind(ctx, name)) return;
    *b = (struct glm_indexed_buffer){name, 0, -1};
    *buffer_binding(ctx, target) = name;
}
GLM_EXPORT void glBindBufferRangeEXT(GLenum t, GLuint i, GLuint n, GLintptr o, GLsizeiptr s) { glBindBufferRange(t, i, n, o, s); }
GLM_EXPORT void glBindBufferBaseEXT(GLenum t, GLuint i, GLuint n) { glBindBufferBase(t, i, n); }
