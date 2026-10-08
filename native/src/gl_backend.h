/* Which OpenGL implementation the loader and its guest talk to.
 *
 * The loader does not link OpenGL.framework.  Every gl* and CGL* symbol it
 * uses (and every one a guest can import) is an exported trampoline through a
 * dispatch table, filled either from Apple's OpenGL.framework (the default)
 * from LP32GL, the Mesa-based implementation in native/lp32gl, or from
 * GLMetal, the from-scratch implementation on Metal in native/glmetal.
 *
 * LP32_GL_BACKEND=apple|mesa|metal overrides the title's default. */
#ifndef LP32_GL_BACKEND_H
#define LP32_GL_BACKEND_H

#include <stdbool.h>

enum lp32_gl_backend {
    LP32_GL_BACKEND_APPLE,
    LP32_GL_BACKEND_MESA,
    LP32_GL_BACKEND_METAL,
};

/* Switches to `preferred` unless LP32_GL_BACKEND says otherwise.  Must run
   before the guest or the loader creates a GL context.  Falls back to Apple's
   OpenGL (and says so) if LP32GL cannot start. */
void lp32_gl_backend_select(enum lp32_gl_backend preferred);
enum lp32_gl_backend lp32_gl_backend(void);
static inline bool lp32_gl_backend_is_mesa(void) { return lp32_gl_backend() == LP32_GL_BACKEND_MESA; }
/* LP32GL or GLMetal: an implementation with its own CGL, AGL and NSOpenGL
   classes rather than Apple's. */
static inline bool lp32_gl_backend_is_replacement(void) { return lp32_gl_backend() != LP32_GL_BACKEND_APPLE; }
/* The replacement's NSOpenGLContext subclass name, or NULL for Apple's. */
const char *lp32_gl_backend_context_class(void);

/* An AGL entry point ("aglCreateContext", no underscore) in the selected
   implementation, or NULL. */
void *lp32_gl_backend_agl_symbol(const char *name);

#endif
