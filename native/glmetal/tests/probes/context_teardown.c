/* Standalone, bounded teardown probe. No glFinish before context destruction.
 * Build: xcrun clang tests/probes/context_teardown.c -o build/context_teardown
 * Run: build/context_teardown /absolute/path/to/libGLMetal.dylib
 * Defaults to 12 iterations, with 50 ms idle time between iterations. */
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define API_LIST(X) \
    X(CGLError, CGLChoosePixelFormat, (const CGLPixelFormatAttribute *, CGLPixelFormatObj *, GLint *)) \
    X(CGLError, CGLCreateContext, (CGLPixelFormatObj, CGLContextObj, CGLContextObj *)) \
    X(CGLError, CGLSetCurrentContext, (CGLContextObj)) \
    X(CGLError, CGLDestroyContext, (CGLContextObj)) \
    X(CGLError, CGLDestroyPixelFormat, (CGLPixelFormatObj)) \
    X(void, glGenTextures, (GLsizei, GLuint *)) \
    X(void, glBindTexture, (GLenum, GLuint)) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, glGetTexImage, (GLenum, GLint, GLenum, GLenum, void *)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint *)) \
    X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum)) \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glClear, (GLbitfield)) \
    X(void, glGenBuffers, (GLsizei, GLuint *)) \
    X(void, glBindBuffer, (GLenum, GLuint)) \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *)) \
    X(void, glEnableClientState, (GLenum)) \
    X(void, glVertexPointer, (GLint, GLenum, GLsizei, const void *)) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glColor3f, (GLfloat, GLfloat, GLfloat)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(void, glFlush, (void)) \
    X(GLenum, glGetError, (void))
#define DECLARE(result, name, arguments) static result (*p_##name) arguments;
API_LIST(DECLARE)

static void require(bool okay, const char *message)
{
    if (!okay) {
        fprintf(stderr, "context teardown: %s\n", message);
        exit(1);
    }
}

static void framebuffer(void)
{
    GLuint texture, target;
    p_glGenTextures(1, &texture);
    p_glBindTexture(GL_TEXTURE_2D, texture);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    p_glGenFramebuffers(1, &target);
    p_glBindFramebuffer(GL_FRAMEBUFFER, target);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    require(p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "mixed-read framebuffer incomplete");
    p_glViewport(0, 0, 64, 64);
    p_glClearColor(0, 0, 0, 1);
    p_glClear(GL_COLOR_BUFFER_BIT);
}

static void mixed_reads(CGLPixelFormatObj format)
{
    CGLContextObj first = NULL, second = NULL;
    require(p_CGLCreateContext(format, NULL, &first) == kCGLNoError, "mixed-read first creation failed");
    require(p_CGLCreateContext(format, first, &second) == kCGLNoError, "mixed-read second creation failed");
    require(p_CGLSetCurrentContext(first) == kCGLNoError, "mixed-read first activation failed");
    framebuffer();
    const GLfloat original[] = {-1, -1, 0, 1, -1, 0, 0, 1, 0};
    const GLfloat replacement[] = {3, -1, 0, 5, -1, 0, 4, 1, 0};
    GLuint buffer;
    p_glGenBuffers(1, &buffer);
    p_glBindBuffer(GL_ARRAY_BUFFER, buffer);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof original, original, GL_DYNAMIC_DRAW);
    p_glEnableClientState(GL_VERTEX_ARRAY);
    p_glVertexPointer(3, GL_FLOAT, 0, NULL);
    p_glColor3f(1, 0, 0);
    p_glDrawArrays(GL_TRIANGLES, 0, 3); /* Leave this old-storage read unsubmitted. */
    require(p_CGLSetCurrentContext(second) == kCGLNoError, "mixed-read second activation failed");
    framebuffer();
    p_glBindBuffer(GL_ARRAY_BUFFER, buffer);
    p_glEnableClientState(GL_VERTEX_ARRAY);
    p_glVertexPointer(3, GL_FLOAT, 0, NULL);
    p_glDrawArrays(GL_TRIANGLES, 0, 3);
    require(p_glGetError() == GL_NO_ERROR, "mixed-read second draw failed");
    require(p_CGLDestroyContext(second) == kCGLNoError, "mixed-read second destruction failed");
    require(p_CGLSetCurrentContext(first) == kCGLNoError, "mixed-read survivor activation failed");
    p_glBindBuffer(GL_ARRAY_BUFFER, buffer);
    p_glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof replacement, replacement);
    p_glColor3f(0, 1, 0);
    p_glDrawArrays(GL_TRIANGLES, 0, 3); /* New storage is entirely outside the viewport. */
    unsigned char pixel[4] = {0};
    p_glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    require(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0,
            "orphaned shared VBO read lost its original storage");
    require(p_glGetError() == GL_NO_ERROR, "mixed-read survivor error");
    require(p_CGLDestroyContext(first) == kCGLNoError, "mixed-read survivor destruction failed");
}

int main(int argc, char **argv)
{
    require(argc == 2 || argc == 3, "usage: context_teardown PROVIDER [ITERATIONS]");
    int iterations = argc == 3 ? atoi(argv[2]) : 12;
    require(iterations > 0 && iterations <= 64, "iterations must be 1..64");
    void *provider = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    require(provider != NULL, "provider could not load");
    bool (*initialize)(void) = dlsym(provider, "glmetal_initialize");
    if (initialize) require(initialize(), "provider initialization failed");
#define LOAD(result, name, arguments) p_##name = dlsym(provider, #name); require(p_##name != NULL, #name " missing");
    API_LIST(LOAD)
    CGLPixelFormatAttribute attributes[] = {kCGLPFAAccelerated, 0};
    CGLPixelFormatObj format = NULL;
    GLint count = 0;
    require(p_CGLChoosePixelFormat(attributes, &format, &count) == kCGLNoError && format, "pixel format failed");
    unsigned char pixels[64 * 64 * 4], observed[sizeof pixels];
    memset(pixels, 0x6b, sizeof pixels);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        CGLContextObj owner = NULL, survivor = NULL;
        require(p_CGLCreateContext(format, NULL, &owner) == kCGLNoError, "owner creation failed");
        require(p_CGLCreateContext(format, owner, &survivor) == kCGLNoError, "shared creation failed");
        require(p_CGLSetCurrentContext(owner) == kCGLNoError, "owner activation failed");
        GLuint texture, framebuffer;
        p_glGenTextures(1, &texture);
        p_glBindTexture(GL_TEXTURE_2D, texture);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        p_glGenFramebuffers(1, &framebuffer);
        p_glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        require(p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "framebuffer incomplete");
        p_glClearColor(1, 0, 0, 1);
        p_glClear(GL_COLOR_BUFFER_BIT);
        p_glFlush();
        p_glClearColor(0, 1, 0, 1);
        p_glClear(GL_COLOR_BUFFER_BIT);
        p_glFlush();
        p_glClearColor(0, 0, 1, 1);
        p_glClear(GL_COLOR_BUFFER_BIT); /* unsubmitted tail at destruction */
        require(p_glGetError() == GL_NO_ERROR, "submission error");
        require(p_CGLDestroyContext(owner) == kCGLNoError, "owner destruction failed");
        require(p_CGLSetCurrentContext(survivor) == kCGLNoError, "survivor activation failed");
        p_glBindTexture(GL_TEXTURE_2D, texture);
        p_glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, observed);
        for (size_t i = 0; i < sizeof observed; i += 4)
            require(observed[i] == 0 && observed[i + 1] == 0 && observed[i + 2] == 255 && observed[i + 3] == 255,
                    "shared texture did not survive pending owner work");
        /* Leave this shared texture and a per-context default texture live.
           The final context must release both without explicit deletion. */
        p_glBindTexture(GL_TEXTURE_2D, 0);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        require(p_glGetError() == GL_NO_ERROR, "survivor error");
        require(p_CGLDestroyContext(survivor) == kCGLNoError, "survivor destruction failed");
        mixed_reads(format);
        struct timespec idle = {0, 50000000};
        nanosleep(&idle, NULL);
        fprintf(stderr, "context teardown: %d/%d passed\n", iteration + 1, iterations);
    }
    p_CGLDestroyPixelFormat(format);
    return 0;
}
