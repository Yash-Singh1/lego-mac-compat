/* APPLE_fence, APPLE object tests and ARB_sync on command buffer serials. */
#include "glm_internal.h"

#include <stdlib.h>

GLM_HIDDEN uint64_t glm_backend_fence_insert(struct glm_context *ctx);
GLM_HIDDEN bool glm_backend_fence_done(struct glm_context *ctx, uint64_t serial);
GLM_HIDDEN void glm_backend_fence_wait(struct glm_context *ctx, uint64_t serial);

/* APPLE_fence names are per context. 0: never set. */
struct apple_fence { uint64_t serial; bool set; };

static struct apple_fence *fence_slot(struct glm_context *ctx, GLuint name, bool create)
{
    struct apple_fence *f = glm_table_get(&ctx->fences, name);
    if (!f && create && name) {
        f = calloc(1, sizeof *f);
        glm_table_set(&ctx->fences, name, f);
    }
    return f;
}

GLM_EXPORT void glGenFencesAPPLE(GLsizei n, GLuint *fences)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; ++i) {
        fences[i] = glm_table_reserve(&ctx->fences);
        fence_slot(ctx, fences[i], true);
    }
}

GLM_EXPORT void glDeleteFencesAPPLE(GLsizei n, const GLuint *fences)
{
    GLM_CONTEXT(ctx);
    if (n < 0) return glm_error(ctx, GL_INVALID_VALUE);
    for (GLsizei i = 0; i < n; ++i) {
        struct apple_fence *f = fence_slot(ctx, fences[i], false);
        if (!f) continue;
        glm_table_set(&ctx->fences, fences[i], NULL);
        free(f);
    }
}

GLM_EXPORT GLboolean glIsFenceAPPLE(GLuint fence) { GLM_CONTEXT(ctx, GL_FALSE); return fence_slot(ctx, fence, false) != NULL; }

GLM_EXPORT void glSetFenceAPPLE(GLuint fence)
{
    GLM_CONTEXT(ctx);
    struct apple_fence *f = fence_slot(ctx, fence, true);
    f->serial = glm_backend_fence_insert(ctx);
    f->set = true;
}

GLM_EXPORT GLboolean glTestFenceAPPLE(GLuint fence)
{
    GLM_CONTEXT(ctx, GL_TRUE);
    struct apple_fence *f = fence_slot(ctx, fence, false);
    return !f || !f->set || glm_backend_fence_done(ctx, f->serial);
}

GLM_EXPORT void glFinishFenceAPPLE(GLuint fence)
{
    GLM_CONTEXT(ctx);
    struct apple_fence *f = fence_slot(ctx, fence, false);
    if (f && f->set) glm_backend_fence_wait(ctx, f->serial);
}

/* Object tests cover every use issued so far. */
GLM_EXPORT GLboolean glTestObjectAPPLE(GLenum object, GLuint name)
{
    GLM_CONTEXT(ctx, GL_TRUE);
    (void)object; (void)name;
    return glm_backend_fence_done(ctx, glm_backend_fence_insert(ctx));
}

GLM_EXPORT void glFinishObjectAPPLE(GLenum object, GLuint name)
{
    GLM_CONTEXT(ctx);
    (void)object; (void)name;
    glm_backend_fence_wait(ctx, glm_backend_fence_insert(ctx));
}

/* ---- ARB_sync ------------------------------------------------------------ */

struct glm_sync { uint32_t magic; uint64_t serial; struct glm_context *ctx; };
#define SYNC_MAGIC 0x53594e43u

GLM_EXPORT GLsync glFenceSync(GLenum condition, GLbitfield flags)
{
    GLM_CONTEXT(ctx, NULL);
    if (condition != GL_SYNC_GPU_COMMANDS_COMPLETE || flags) return glm_error(ctx, GL_INVALID_ENUM), NULL;
    struct glm_sync *sync = calloc(1, sizeof *sync);
    sync->magic = SYNC_MAGIC;
    sync->serial = glm_backend_fence_insert(ctx);
    sync->ctx = ctx;
    return (GLsync)sync;
}

/* The command stream (marshal_custom.c) creates the object when the call is
   recorded and inserts the fence when it executes. */
enum { SYNC_PENDING = UINT64_MAX };

GLM_HIDDEN GLsync glm_sync_new_pending(struct glm_context *ctx)
{
    struct glm_sync *sync = calloc(1, sizeof *sync);
    sync->magic = SYNC_MAGIC;
    sync->serial = SYNC_PENDING;
    sync->ctx = ctx;
    return (GLsync)sync;
}

GLM_HIDDEN void glm_sync_arm(GLsync handle)
{
    struct glm_sync *sync = (struct glm_sync *)handle;
    __atomic_store_n(&sync->serial, glm_backend_fence_insert(sync->ctx), __ATOMIC_RELEASE);
}

GLM_HIDDEN bool glm_sync_is_pending(GLsync handle)
{
    struct glm_sync *sync = (struct glm_sync *)handle;
    return sync && sync->magic == SYNC_MAGIC && __atomic_load_n(&sync->serial, __ATOMIC_ACQUIRE) == SYNC_PENDING;
}

static struct glm_sync *valid_sync(GLsync handle)
{
    struct glm_sync *sync = (struct glm_sync *)handle;
    return sync && sync->magic == SYNC_MAGIC ? sync : NULL;
}

GLM_EXPORT GLboolean glIsSync(GLsync handle) { return valid_sync(handle) != NULL; }

GLM_EXPORT void glDeleteSync(GLsync handle)
{
    struct glm_sync *sync = valid_sync(handle);
    if (!sync) return;
    sync->magic = 0;
    free(sync);
}

GLM_EXPORT GLenum glClientWaitSync(GLsync handle, GLbitfield flags, GLuint64 timeout)
{
    struct glm_sync *sync = valid_sync(handle);
    if (!sync) return GL_WAIT_FAILED;
    (void)flags;
    if (glm_backend_fence_done(sync->ctx, sync->serial)) return GL_ALREADY_SIGNALED;
    if (!timeout) return GL_TIMEOUT_EXPIRED;
    glm_backend_fence_wait(sync->ctx, sync->serial);
    return GL_CONDITION_SATISFIED;
}

/* One queue per share group already orders the GPU work. */
GLM_EXPORT void glWaitSync(GLsync handle, GLbitfield flags, GLuint64 timeout) { (void)handle; (void)flags; (void)timeout; }

GLM_EXPORT void glGetSynciv(GLsync handle, GLenum pname, GLsizei size, GLsizei *length, GLint *values)
{
    struct glm_sync *sync = valid_sync(handle);
    if (!sync || size < 1) return;
    if (length) *length = 1;
    switch (pname) {
    case GL_OBJECT_TYPE: values[0] = GL_SYNC_FENCE; break;
    case GL_SYNC_STATUS: values[0] = glm_backend_fence_done(sync->ctx, sync->serial) ? GL_SIGNALED : GL_UNSIGNALED; break;
    case GL_SYNC_CONDITION: values[0] = GL_SYNC_GPU_COMMANDS_COMPLETE; break;
    case GL_SYNC_FLAGS: values[0] = 0; break;
    default: if (length) *length = 0;
    }
}
