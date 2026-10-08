/* libGLMetalInject: puts GLMetal under an unmodified application.
 *
 *   DYLD_INSERT_LIBRARIES=/path/libGLMetalInject.dylib App.app/Contents/MacOS/App
 *
 * dyld interposes every OpenGL.framework export with a trampoline into
 * GLMetal (inject_tables.c, generated), so both the application's direct
 * calls and AppKit's CGL calls reach GLMetal. Lookups that return function
 * pointers at run time (dlsym, CFBundleGetFunctionPointerForName, which
 * SDL and GLFW use) are mapped the same way, and NSOpenGLContext and
 * NSOpenGLPixelFormat allocate GLMetal's subclasses.
 *
 * libGLMetal.dylib is found next to this library or at GLMETAL_PATH.
 * GLMETAL_INJECT_DISABLE=1 leaves Apple's OpenGL in place. */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLM_INJECT_COUNT_FROM_TABLES
extern void *glm_inject_table[];
extern const char *const glm_inject_names[];
extern const void *const glm_inject_apple[];
extern const size_t glm_inject_count;

static bool active;

static int find(const char *name)
{
    size_t lo = 0, hi = glm_inject_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(glm_inject_names[mid], name);
        if (!c) return (int)mid;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

/* A pointer Apple's OpenGL handed out, swapped for GLMetal's. */
static void *map_pointer(const char *name, void *pointer)
{
    if (!active || !pointer || !name) return pointer;
    int i = find(name);
    if (i >= 0 && (pointer == glm_inject_apple[i] || pointer == glm_inject_table[i])) return glm_inject_table[i];
    return pointer;
}

static void *glm_dlsym(void *handle, const char *symbol)
{
    return map_pointer(symbol, dlsym(handle, symbol));
}

static void *glm_bundle_function(CFBundleRef bundle, CFStringRef name)
{
    void *pointer = CFBundleGetFunctionPointerForName(bundle, name);
    char buffer[256];
    if (!pointer || !name || !CFStringGetCString(name, buffer, sizeof buffer, kCFStringEncodingUTF8)) return pointer;
    return map_pointer(buffer, pointer);
}

static void glm_bundle_functions(CFBundleRef bundle, CFArrayRef names, void *pointers[])
{
    CFBundleGetFunctionPointersForNames(bundle, names, pointers);
    CFIndex count = names ? CFArrayGetCount(names) : 0;
    for (CFIndex i = 0; i < count; ++i) {
        char buffer[256];
        CFStringRef name = CFArrayGetValueAtIndex(names, i);
        if (name && CFStringGetCString(name, buffer, sizeof buffer, kCFStringEncodingUTF8))
            pointers[i] = map_pointer(buffer, pointers[i]);
    }
}

struct interpose { const void *replacement, *replacee; };
__attribute__((used, section("__DATA,__interpose"))) static const struct interpose lookups[] = {
    {(const void *)glm_dlsym, (const void *)dlsym},
    {(const void *)glm_bundle_function, (const void *)CFBundleGetFunctionPointerForName},
    {(const void *)glm_bundle_functions, (const void *)CFBundleGetFunctionPointersForNames},
};

static void *load_glmetal(void)
{
    const char *override = getenv("GLMETAL_PATH");
    if (override && override[0]) return dlopen(override, RTLD_NOW | RTLD_LOCAL);
    Dl_info info;
    if (!dladdr((const void *)load_glmetal, &info) || !info.dli_fname) return NULL;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s", info.dli_fname);
    char *slash = strrchr(path, '/');
    if (!slash) return NULL;
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "libGLMetal.dylib");
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

__attribute__((constructor)) static void inject(void)
{
    /* Until GLMetal is up, every trampoline goes to Apple's code. */
    for (size_t i = 0; i < glm_inject_count; ++i) glm_inject_table[i] = (void *)glm_inject_apple[i];
    const char *disable = getenv("GLMETAL_INJECT_DISABLE");
    if (disable && disable[0] == '1') return;
    void *glmetal = load_glmetal();
    if (!glmetal) {
        fprintf(stderr, "glmetal-inject: cannot load libGLMetal.dylib: %s\n", dlerror());
        return;
    }
    bool (*initialize)(void) = (bool (*)(void))dlsym(glmetal, "glmetal_initialize");
    void *(*lookup)(const char *) = (void *(*)(const char *))dlsym(glmetal, "glmetal_get_proc_address");
    void (*install)(void) = (void (*)(void))dlsym(glmetal, "glmetal_install_appkit_overrides");
    if (!initialize || !lookup || !install || !initialize()) {
        fprintf(stderr, "glmetal-inject: GLMetal did not start; using Apple's OpenGL\n");
        return;
    }
    size_t missing = 0;
    for (size_t i = 0; i < glm_inject_count; ++i) {
        void *function = lookup(glm_inject_names[i]);
        if (function) glm_inject_table[i] = function;
        else ++missing;
    }
    install();
    active = true;
    if (getenv("GLMETAL_DEBUG"))
        fprintf(stderr, "glmetal-inject: %zu entry points on GLMetal (%zu left on Apple's)\n", glm_inject_count - missing,
                missing);
}
