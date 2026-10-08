/* Windowed smoke test: an NSOpenGLContext on an NSView, drawn and
 * presented for a number of frames, the way applications use Apple's GL.
 *
 *   glm_window [--provider apple|PATH] [--frames N] [--capture out.ppm]
 *
 * With a PATH the provider is loaded and its AppKit overrides installed
 * before any NSOpenGL object exists, which is what an injector does. The
 * capture is the back buffer read just before the last flush. */
#define GL_SILENCE_DEPRECATION 1
#import <Cocoa/Cocoa.h>
#include <OpenGL/gl.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GL calls go through the provider's table when one is loaded. */
static void *provider;
#define GLFN(ret, name, params)                                                           \
    static ret(*p_##name) params;                                                         \
    static void resolve_##name(void)                                                      \
    {                                                                                     \
        p_##name = provider ? dlsym(provider, #name) : dlsym(RTLD_DEFAULT, #name);        \
        if (!p_##name) { fprintf(stderr, "missing %s\n", #name); exit(1); }               \
    }
GLFN(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))
GLFN(void, glClear, (GLbitfield))
GLFN(void, glBegin, (GLenum))
GLFN(void, glEnd, (void))
GLFN(void, glColor3f, (GLfloat, GLfloat, GLfloat))
GLFN(void, glVertex2f, (GLfloat, GLfloat))
GLFN(void, glViewport, (GLint, GLint, GLsizei, GLsizei))
GLFN(void, glLoadIdentity, (void))
GLFN(void, glRotatef, (GLfloat, GLfloat, GLfloat, GLfloat))
GLFN(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))
GLFN(void, glGetIntegerv, (GLenum, GLint *))
GLFN(const GLubyte *, glGetString, (GLenum))
GLFN(void, glFinish, (void))

static void resolve_all(void)
{
    resolve_glClearColor(); resolve_glClear(); resolve_glBegin(); resolve_glEnd(); resolve_glColor3f();
    resolve_glVertex2f(); resolve_glViewport(); resolve_glLoadIdentity(); resolve_glRotatef();
    resolve_glReadPixels(); resolve_glGetIntegerv(); resolve_glGetString(); resolve_glFinish();
}

static void write_ppm(const char *path, const unsigned char *rgba, int width, int height)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int y = height - 1; y >= 0; --y)
        for (int x = 0; x < width; ++x) fwrite(rgba + ((size_t)y * width + x) * 4, 3, 1, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *provider_path = NULL, *capture = NULL;
    int frames = 120;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--provider") && i + 1 < argc) provider_path = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc) capture = argv[++i];
    }
    if (provider_path && strcmp(provider_path, "apple")) {
        provider = dlopen(provider_path, RTLD_NOW | RTLD_LOCAL);
        if (!provider) { fprintf(stderr, "%s\n", dlerror()); return 1; }
        bool (*initialize)(void) = dlsym(provider, "glmetal_initialize");
        void (*install)(void) = dlsym(provider, "glmetal_install_appkit_overrides");
        if (!initialize || !install || !initialize()) { fprintf(stderr, "provider did not start\n"); return 1; }
        install();
    }
    resolve_all();
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(80, 80, 400, 300)
                                                       styleMask:NSWindowStyleMaskTitled
                                                         backing:NSBackingStoreBuffered defer:NO];
        window.title = provider ? @"GLMetal window test" : @"Apple GL window test";
        [window orderFront:nil];
        NSOpenGLPixelFormatAttribute attributes[] = {NSOpenGLPFADoubleBuffer, NSOpenGLPFAColorSize, 24,
                                                     NSOpenGLPFADepthSize, 24, 0};
        NSOpenGLPixelFormat *format = [[NSOpenGLPixelFormat alloc] initWithAttributes:attributes];
        NSOpenGLContext *context = [[NSOpenGLContext alloc] initWithFormat:format shareContext:nil];
        printf("context class: %s\n", object_getClassName(context));
        [context setView:window.contentView];
        [context makeCurrentContext];
        GLint one = 1;
        [context setValues:&one forParameter:NSOpenGLContextParameterSwapInterval];
        printf("renderer: %s / %s\n", p_glGetString(GL_RENDERER), p_glGetString(GL_VERSION));
        uint64_t start = mach_absolute_time();
        for (int frame = 0; frame < frames; ++frame) {
            @autoreleasepool {
                NSEvent *event;
                while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode
                                                     dequeue:YES]))
                    [NSApp sendEvent:event];
                GLint viewport[4];
                p_glGetIntegerv(GL_VIEWPORT, viewport);
                p_glClearColor(0.1f, 0.2f, 0.3f, 1);
                p_glClear(GL_COLOR_BUFFER_BIT);
                p_glLoadIdentity();
                p_glRotatef((float)frame * 3, 0, 0, 1);
                p_glBegin(GL_TRIANGLES);
                p_glColor3f(1, 0, 0); p_glVertex2f(-0.6f, -0.5f);
                p_glColor3f(0, 1, 0); p_glVertex2f(0.6f, -0.5f);
                p_glColor3f(0, 0, 1); p_glVertex2f(0, 0.6f);
                p_glEnd();
                if (capture && frame == frames - 1) {
                    unsigned char *pixels = malloc((size_t)viewport[2] * viewport[3] * 4);
                    p_glReadPixels(0, 0, viewport[2], viewport[3], GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                    write_ppm(capture, pixels, viewport[2], viewport[3]);
                    free(pixels);
                    printf("captured %dx%d\n", viewport[2], viewport[3]);
                }
                [context flushBuffer];
            }
        }
        mach_timebase_info_data_t base;
        mach_timebase_info(&base);
        double seconds = (double)(mach_absolute_time() - start) * base.numer / base.denom / 1e9;
        printf("%d frames in %.2f s (%.1f fps)\n", frames, seconds, frames / seconds);
        [NSOpenGLContext clearCurrentContext];
    }
    return 0;
}
