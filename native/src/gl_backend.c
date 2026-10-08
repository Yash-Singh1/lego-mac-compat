#include "gl_backend.h"

#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "gl_dispatch_names.h"

void *lp32_gl_dispatch_table[LP32_GL_DISPATCH_COUNT];

static enum lp32_gl_backend selected = LP32_GL_BACKEND_APPLE;
static void *apple_gl, *apple_agl, *lp32gl, *glmetal;
static void *(*lp32gl_lookup)(const char *);

static uintptr_t missing_entry_point(void)
{
    static int reported;
    if (!reported++) fprintf(stderr, "compat32: called an OpenGL entry point the selected implementation lacks\n");
    return 0;
}

static void fill_table(void *(*lookup)(const char *))
{
    for (unsigned i = 0; i < LP32_GL_DISPATCH_COUNT; ++i) {
        void *function = lookup(lp32_gl_dispatch_names[i]);
        lp32_gl_dispatch_table[i] = function ? function : (void *)missing_entry_point;
    }
}

static void *apple_lookup(const char *name) { return dlsym(apple_gl, name); }

/* Apple's OpenGL is loaded globally in either mode: GLU lives there, and
   AppKit's NSOpenGL classes need it for anything LP32GL does not replace. */
__attribute__((constructor)) static void load_apple(void)
{
    apple_gl = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_NOW | RTLD_GLOBAL);
    if (!apple_gl) {
        fprintf(stderr, "compat32: cannot load OpenGL.framework: %s\n", dlerror());
        abort();
    }
    fill_table(apple_lookup);
}

static bool lp32gl_path(char *path, size_t size)
{
    const char *override = getenv("LP32GL_PATH");
    if (override && override[0]) {
        snprintf(path, size, "%s", override);
        return true;
    }
    char executable[PATH_MAX];
    uint32_t length = sizeof executable;
    if (_NSGetExecutablePath(executable, &length) != 0) return false;
    char resolved[PATH_MAX];
    if (!realpath(executable, resolved)) return false;
    char *slash = strrchr(resolved, '/');
    if (!slash) return false;
    *slash = 0;
    /* A bundle keeps it in Contents/Frameworks; a bare build/game_loader
       finds build/lp32gl next to it. */
    snprintf(path, size, "%s/../Frameworks/LP32GL/libLP32GL.dylib", resolved);
    if (access(path, R_OK) == 0) return true;
    snprintf(path, size, "%s/lp32gl/libLP32GL.dylib", resolved);
    return access(path, R_OK) == 0;
}

static bool start_lp32gl(void)
{
    char path[PATH_MAX];
    if (!lp32gl_path(path, sizeof path)) {
        fprintf(stderr, "compat32: LP32GL is not installed in this bundle\n");
        return false;
    }
    lp32gl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lp32gl) {
        fprintf(stderr, "compat32: cannot load LP32GL: %s\n", dlerror());
        return false;
    }
    bool (*initialize)(void) = (bool (*)(void))dlsym(lp32gl, "lp32gl_initialize");
    const char *(*last_error)(void) = (const char *(*)(void))dlsym(lp32gl, "lp32gl_last_error");
    void (*install)(void) = (void (*)(void))dlsym(lp32gl, "lp32gl_install_appkit_overrides");
    lp32gl_lookup = (void *(*)(const char *))dlsym(lp32gl, "lp32gl_get_proc_address");
    if (!initialize || !install || !lp32gl_lookup) {
        fprintf(stderr, "compat32: %s is not LP32GL\n", path);
        return false;
    }
    if (!initialize()) {
        fprintf(stderr, "compat32: LP32GL could not start: %s\n", last_error ? last_error() : "unknown");
        return false;
    }
    install();
    fill_table(lp32gl_lookup);
    return true;
}

/* GLMetal: LP32_GLMETAL_PATH, the bundle's Frameworks/GLMetal, or
   build/glmetal next to a bare build/game_loader. */
static bool glmetal_path(char *path, size_t size)
{
    const char *override = getenv("LP32_GLMETAL_PATH");
    if (override && override[0]) {
        snprintf(path, size, "%s", override);
        return true;
    }
    char executable[PATH_MAX];
    uint32_t length = sizeof executable;
    if (_NSGetExecutablePath(executable, &length) != 0) return false;
    char resolved[PATH_MAX];
    if (!realpath(executable, resolved)) return false;
    char *slash = strrchr(resolved, '/');
    if (!slash) return false;
    *slash = 0;
    snprintf(path, size, "%s/../Frameworks/GLMetal/libGLMetal.dylib", resolved);
    if (access(path, R_OK) == 0) return true;
    snprintf(path, size, "%s/glmetal/libGLMetal.dylib", resolved);
    return access(path, R_OK) == 0;
}

/* Keep GPU storage out of the guest's 32-bit address space. This is a host
   allocation constraint, not OpenGL behavior, so the loader supplies it. */
static void *glmetal_guest_safe_allocate(size_t size)
{
    void *memory = mmap((void *)((uintptr_t)1 << 36), size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) return NULL;
    if ((uintptr_t)memory < ((uintptr_t)1 << 32)) {
        munmap(memory, size);
        return NULL;
    }
    return memory;
}

static void glmetal_guest_safe_deallocate(void *memory, size_t size)
{
    munmap(memory, size);
}

static bool start_glmetal(void)
{
    char path[PATH_MAX];
    if (!glmetal_path(path, sizeof path)) {
        fprintf(stderr, "compat32: GLMetal is not installed in this bundle\n");
        return false;
    }
    glmetal = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!glmetal) {
        fprintf(stderr, "compat32: cannot load GLMetal: %s\n", dlerror());
        return false;
    }
    bool (*initialize)(void) = (bool (*)(void))dlsym(glmetal, "glmetal_initialize");
    const char *(*last_error)(void) = (const char *(*)(void))dlsym(glmetal, "glmetal_last_error");
    void (*install)(void) = (void (*)(void))dlsym(glmetal, "glmetal_install_appkit_overrides");
    void *(*lookup)(const char *) = (void *(*)(const char *))dlsym(glmetal, "glmetal_get_proc_address");
    bool (*set_allocator)(void *(*)(size_t), void (*)(void *, size_t)) =
        (bool (*)(void *(*)(size_t), void (*)(void *, size_t)))dlsym(glmetal, "glmetal_set_shared_buffer_allocator");
    bool (*prewarm)(unsigned, unsigned) =
        (bool (*)(unsigned, unsigned))dlsym(glmetal, "glmetal_prewarm_command_queues");
    if (!initialize || !install || !lookup) {
        fprintf(stderr, "compat32: %s is not GLMetal\n", path);
        return false;
    }
    if (set_allocator && !set_allocator(glmetal_guest_safe_allocate, glmetal_guest_safe_deallocate)) {
        fprintf(stderr, "compat32: GLMetal rejected the guest-safe buffer allocator\n");
        return false;
    }
    if (!initialize()) {
        fprintf(stderr, "compat32: GLMetal could not start: %s\n", last_error ? last_error() : "unknown");
        return false;
    }
    /* Exercise Metal's command-memory pools before guest level allocations
       claim the remaining low pages needed by IOGPUDeviceShmem. */
    if (prewarm && !prewarm(4, 6)) {
        fprintf(stderr, "compat32: GLMetal command queue preparation failed\n");
        return false;
    }
    install();
    lp32gl_lookup = lookup;
    fill_table(lookup);
    return true;
}

void lp32_gl_backend_select(enum lp32_gl_backend preferred)
{
    const char *override = getenv("LP32_GL_BACKEND");
    enum lp32_gl_backend wanted = preferred;
    CFBundleRef bundle = CFBundleGetMainBundle();
    if (bundle && CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("LP32GLMetal")) == kCFBooleanTrue)
        wanted = LP32_GL_BACKEND_METAL;
    if (override && !strcmp(override, "mesa")) wanted = LP32_GL_BACKEND_MESA;
    else if (override && !strcmp(override, "metal")) wanted = LP32_GL_BACKEND_METAL;
    else if (override && !strcmp(override, "apple")) wanted = LP32_GL_BACKEND_APPLE;
    if (wanted == selected) return;
    if (wanted == LP32_GL_BACKEND_MESA) {
        if (start_lp32gl()) {
            selected = LP32_GL_BACKEND_MESA;
            fprintf(stderr, "compat32: OpenGL through LP32GL (Mesa Zink on KosmicKrisp)\n");
        } else {
            fprintf(stderr, "compat32: falling back to Apple's OpenGL\n");
        }
    } else if (wanted == LP32_GL_BACKEND_METAL) {
        if (start_glmetal()) {
            selected = LP32_GL_BACKEND_METAL;
            fprintf(stderr, "compat32: OpenGL through GLMetal\n");
        } else {
            fprintf(stderr, "compat32: falling back to Apple's OpenGL\n");
        }
    }
}

const char *lp32_gl_backend_context_class(void)
{
    switch (selected) {
    case LP32_GL_BACKEND_MESA: return "LP32OpenGLContext";
    case LP32_GL_BACKEND_METAL: return "GLMOpenGLContext";
    default: return NULL;
    }
}

enum lp32_gl_backend lp32_gl_backend(void) { return selected; }

void *lp32_gl_backend_agl_symbol(const char *name)
{
    if (selected != LP32_GL_BACKEND_APPLE) return lp32gl_lookup(name);
    if (!apple_agl) apple_agl = dlopen("/System/Library/Frameworks/AGL.framework/AGL", RTLD_NOW | RTLD_LOCAL);
    return apple_agl ? dlsym(apple_agl, name) : NULL;
}
