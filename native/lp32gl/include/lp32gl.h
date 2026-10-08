/* LP32GL: an OpenGL implementation for macOS built on Mesa (Zink) and
 * KosmicKrisp (Vulkan on Metal).
 *
 * The library exports the public API of Apple's OpenGL.framework (CGL and
 * the gl* entry points) and AGL.framework under their usual names, so code
 * can use it as a drop-in replacement.  A host that loads Apple's
 * frameworks as well (for example to choose a backend at runtime) should
 * dlopen LP32GL with RTLD_LOCAL and resolve every entry point through
 * lp32gl_get_proc_address() or dlsym() on that handle.
 *
 * Profiles follow Apple's: a legacy (OpenGL 2.1) profile, and core profiles
 * requested as 3.2 or 4.1 that both report OpenGL 4.1.  Version strings,
 * extension lists and limits match Apple's OpenGL on Apple silicon except
 * where the implementation cannot back an extension (see README.md). */
#ifndef LP32GL_H
#define LP32GL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LP32GL_EXPORT __attribute__((visibility("default")))

/* Starts Mesa if needed.  Returns false (and a reason via
   lp32gl_last_error) when no usable Vulkan device exists. */
LP32GL_EXPORT bool lp32gl_initialize(void);
LP32GL_EXPORT const char *lp32gl_last_error(void);

/* Any gl*, CGL* or agl* entry point this library implements, with or without
   Mach-O's leading underscore.  NULL for names it does not implement. */
LP32GL_EXPORT void *lp32gl_get_proc_address(const char *name);

/* Makes +[NSOpenGLContext alloc] and +[NSOpenGLPixelFormat alloc] return
   LP32GL-backed subclasses, so AppKit code (including NSOpenGLView) renders
   through this library.  Idempotent.  Must run before any NSOpenGL object
   the host wants on LP32GL is created. */
LP32GL_EXPORT void lp32gl_install_appkit_overrides(void);

/* CGL has no public call to attach a context to a view; NSOpenGLContext
   uses private SPI.  These are LP32GL's equivalent.  `view` is an NSView *. */
typedef struct _CGLContextObject *LP32CGLContextObj;
LP32GL_EXPORT int LP32CGLSetView(LP32CGLContextObj context, void *view);
LP32GL_EXPORT void *LP32CGLGetView(LP32CGLContextObj context);

#ifdef __cplusplus
}
#endif

#endif
