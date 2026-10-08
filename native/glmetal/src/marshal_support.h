/* What the generated marshal.c needs, without GL headers (its prototypes
 * come from the generator) or the implementation's internals. */
#ifndef GLM_MARSHAL_SUPPORT_H
#define GLM_MARSHAL_SUPPORT_H

#include <stddef.h>
#include <stdint.h>

#define GLM_EXPORT __attribute__((visibility("default")))
#define GLM_HIDDEN __attribute__((visibility("hidden")))

#include "thread.h"

struct glm_context;
GLM_HIDDEN struct glm_context *glm_current(void);
/* Reject an unavailable entry point after earlier commands have executed. */
GLM_HIDDEN int glm_profile_rejected(struct glm_context *ctx, int core);
/* The context's command stream: its first member (glm_internal.h). */
#define GLM_STREAM(ctx) (*(void *const *)(ctx))
/* The context's state serial: its second member. */
#define GLM_STATE_CHANGED(ctx) __atomic_add_fetch((volatile uint64_t *)((char *)(ctx) + 8), 1, __ATOMIC_RELAXED)
/* The display list being compiled: its third member (glm_internal.h). */
#define GLM_LISTING(ctx) (*(void *const *)((char *)(ctx) + 16))
/* Room for a command of `size` payload bytes in the list being compiled
   (marshal_custom.c); replayed by glCallList through `exec`. */
GLM_HIDDEN void *glm_list_alloc(struct glm_context *ctx, size_t size, glm_exec_fn exec);
/* GL_COMPILE_AND_EXECUTE: the call runs as well. */
GLM_HIDDEN int glm_list_executes(struct glm_context *ctx);
/* The list changes state the client-side shadow follows. */
GLM_HIDDEN void glm_list_note_shadowed(struct glm_context *ctx);
GLM_HIDDEN void glm_list_unsupported(struct glm_context *ctx, const char *name);
/* Client bytes a glTex(Sub)Image upload reads; 0 with an unpack buffer,
   (size_t)-1 when unknown (marshal_custom.c). */
GLM_HIDDEN size_t glm_marshal_image_size(struct glm_context *ctx, int w, int h, int d, unsigned format, unsigned type);
/* Values glLightfv, glTexParameterfv... read for `pname`. */
GLM_HIDDEN size_t glm_param_count(unsigned pname);
/* pnames of state the cheap (non-serial) calls change: queries must sync. */
GLM_HIDDEN int glm_cheap_pname(unsigned pname);
/* Re-reads the client-side shadow from the (drained) implementation. */
GLM_HIDDEN void glm_shadow_refresh(struct glm_context *ctx);

#endif
