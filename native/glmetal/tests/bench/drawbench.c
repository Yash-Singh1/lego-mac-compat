/* Draw-call throughput: many small VBO draws per frame with per-draw state
 * changes, the way games submit, on Apple's OpenGL or a provider.
 *
 *   drawbench [--provider apple|PATH] [--scene glsl|arb|ff] [--draws N] [--frames N] [--pause-ms N]
 *
 * Renders offscreen (FBO, 1280x720) and reports CPU time per frame and per
 * draw, with glFinish at the end of each frame. */
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *lib;
static void *sym(const char *name)
{
    void *p = dlsym(lib, name);
    if (!p) { fprintf(stderr, "missing %s\n", name); exit(1); }
    return p;
}
#define F(ret, name, params) static ret(*name##_) params;
#define L(name) name##_ = sym(#name)
F(CGLError, CGLChoosePixelFormat, (const CGLPixelFormatAttribute *, CGLPixelFormatObj *, GLint *))
F(CGLError, CGLCreateContext, (CGLPixelFormatObj, CGLContextObj, CGLContextObj *))
F(CGLError, CGLSetCurrentContext, (CGLContextObj))
F(void, glGenFramebuffersEXT, (GLsizei, GLuint *))
F(void, glBindFramebufferEXT, (GLenum, GLuint))
F(void, glGenRenderbuffersEXT, (GLsizei, GLuint *))
F(void, glBindRenderbufferEXT, (GLenum, GLuint))
F(void, glRenderbufferStorageEXT, (GLenum, GLenum, GLsizei, GLsizei))
F(void, glFramebufferRenderbufferEXT, (GLenum, GLenum, GLenum, GLuint))
F(void, glViewport, (GLint, GLint, GLsizei, GLsizei))
F(void, glClear, (GLbitfield))
F(void, glEnable, (GLenum))
F(void, glGenBuffers, (GLsizei, GLuint *))
F(void, glBindBuffer, (GLenum, GLuint))
F(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum))
F(void, glVertexPointer, (GLint, GLenum, GLsizei, const void *))
F(void, glColorPointer, (GLint, GLenum, GLsizei, const void *))
F(void, glEnableClientState, (GLenum))
F(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *))
F(void, glFinish, (void))
F(GLuint, glCreateShader, (GLenum))
F(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))
F(void, glCompileShader, (GLuint))
F(GLuint, glCreateProgram, (void))
F(void, glAttachShader, (GLuint, GLuint))
F(void, glLinkProgram, (GLuint))
F(void, glUseProgram, (GLuint))
F(GLint, glGetUniformLocation, (GLuint, const GLchar *))
F(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))
F(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *))
F(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))
F(void, glEnableVertexAttribArray, (GLuint))
F(void, glGenProgramsARB, (GLsizei, GLuint *))
F(void, glBindProgramARB, (GLenum, GLuint))
F(void, glProgramStringARB, (GLenum, GLenum, GLsizei, const void *))
F(void, glProgramLocalParameter4fARB, (GLenum, GLuint, GLfloat, GLfloat, GLfloat, GLfloat))
F(void, glMatrixMode, (GLenum))
F(void, glLoadIdentity, (void))
F(void, glTranslatef, (GLfloat, GLfloat, GLfloat))
F(void, glScalef, (GLfloat, GLfloat, GLfloat))
F(void, glColor4f, (GLfloat, GLfloat, GLfloat, GLfloat))
F(GLenum, glGetError, (void))
F(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))

static void load(void)
{
    L(CGLChoosePixelFormat); L(CGLCreateContext); L(CGLSetCurrentContext); L(glGenFramebuffersEXT);
    L(glBindFramebufferEXT); L(glGenRenderbuffersEXT); L(glBindRenderbufferEXT); L(glRenderbufferStorageEXT);
    L(glFramebufferRenderbufferEXT); L(glViewport); L(glClear); L(glEnable); L(glGenBuffers); L(glBindBuffer);
    L(glBufferData); L(glVertexPointer); L(glColorPointer); L(glEnableClientState); L(glDrawElements); L(glFinish);
    L(glCreateShader); L(glShaderSource); L(glCompileShader); L(glCreateProgram); L(glAttachShader); L(glLinkProgram);
    L(glUseProgram); L(glGetUniformLocation); L(glUniform4f); L(glBindAttribLocation); L(glVertexAttribPointer);
    L(glEnableVertexAttribArray); L(glGenProgramsARB); L(glBindProgramARB); L(glProgramStringARB);
    L(glProgramLocalParameter4fARB); L(glMatrixMode); L(glLoadIdentity); L(glTranslatef); L(glScalef); L(glColor4f);
    L(glGetError); L(glReadPixels);
}

static double now(void)
{
    static mach_timebase_info_data_t base;
    if (!base.denom) mach_timebase_info(&base);
    return (double)mach_absolute_time() * base.numer / base.denom / 1e9;
}

int main(int argc, char **argv)
{
    const char *provider = "apple", *scene = "glsl";
    int draws = 4000, frames = 60, pause_ms = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--provider") && i + 1 < argc) provider = argv[++i];
        else if (!strcmp(argv[i], "--scene") && i + 1 < argc) scene = argv[++i];
        else if (!strcmp(argv[i], "--draws") && i + 1 < argc) draws = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pause-ms") && i + 1 < argc) pause_ms = atoi(argv[++i]);
    }
    if (draws < 1 || frames < 1 || pause_ms < 0 || pause_ms > 5000 ||
        (strcmp(scene, "glsl") && strcmp(scene, "arb") && strcmp(scene, "ff"))) return 2;
    if (!strcmp(provider, "apple")) {
        lib = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_NOW | RTLD_LOCAL);
    } else {
        lib = dlopen(provider, RTLD_NOW | RTLD_LOCAL);
        bool (*init)(void) = lib ? dlsym(lib, "glmetal_initialize") : NULL;
        if (init && !init()) return 1;
    }
    if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    load();
    CGLPixelFormatAttribute attributes[] = {kCGLPFAAccelerated, kCGLPFAColorSize, 24, kCGLPFADepthSize, 24, 0};
    CGLPixelFormatObj pix;
    GLint n;
    CGLContextObj ctx;
    if (CGLChoosePixelFormat_(attributes, &pix, &n) || CGLCreateContext_(pix, NULL, &ctx)) return 1;
    CGLSetCurrentContext_(ctx);
    GLuint fbo, color, depth;
    glGenFramebuffersEXT_(1, &fbo);
    glBindFramebufferEXT_(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT_(1, &color);
    glBindRenderbufferEXT_(GL_RENDERBUFFER_EXT, color);
    glRenderbufferStorageEXT_(GL_RENDERBUFFER_EXT, GL_RGBA8, 1280, 720);
    glFramebufferRenderbufferEXT_(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, color);
    glGenRenderbuffersEXT_(1, &depth);
    glBindRenderbufferEXT_(GL_RENDERBUFFER_EXT, depth);
    glRenderbufferStorageEXT_(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT24, 1280, 720);
    glFramebufferRenderbufferEXT_(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, depth);
    glViewport_(0, 0, 1280, 720);
    glEnable_(GL_DEPTH_TEST);

    /* A cube: 8 vertices (xyz + rgba8), 36 indices. */
    float positions[8][3];
    unsigned char colors[8][4];
    for (int i = 0; i < 8; ++i) {
        positions[i][0] = i & 1 ? 0.02f : -0.02f;
        positions[i][1] = i & 2 ? 0.02f : -0.02f;
        positions[i][2] = i & 4 ? 0.02f : -0.02f;
        colors[i][0] = (unsigned char)(i * 30); colors[i][1] = 200; colors[i][2] = (unsigned char)(255 - i * 30); colors[i][3] = 255;
    }
    static const unsigned short cube[36] = {0,1,3, 0,3,2, 4,6,7, 4,7,5, 0,4,5, 0,5,1, 2,3,7, 2,7,6, 0,2,6, 0,6,4, 1,5,7, 1,7,3};
    GLuint buffers[3];
    glGenBuffers_(3, buffers);
    glBindBuffer_(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData_(GL_ARRAY_BUFFER, sizeof positions, positions, GL_STATIC_DRAW);
    glBindBuffer_(GL_ARRAY_BUFFER, buffers[1]);
    glBufferData_(GL_ARRAY_BUFFER, sizeof colors, colors, GL_STATIC_DRAW);
    glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER, buffers[2]);
    glBufferData_(GL_ELEMENT_ARRAY_BUFFER, sizeof cube, cube, GL_STATIC_DRAW);

    GLint offset_location = -1;
    if (!strcmp(scene, "glsl")) {
        const char *vs = "#version 120\nattribute vec3 position; attribute vec4 color; uniform vec4 offset; varying vec4 c;\n"
                         "void main(){ gl_Position = vec4(position + offset.xyz, 1.0); c = color * offset.w; }";
        const char *fs = "#version 120\nvarying vec4 c; void main(){ gl_FragColor = c; }";
        GLuint v = glCreateShader_(GL_VERTEX_SHADER), f = glCreateShader_(GL_FRAGMENT_SHADER), p = glCreateProgram_();
        glShaderSource_(v, 1, &vs, NULL); glCompileShader_(v);
        glShaderSource_(f, 1, &fs, NULL); glCompileShader_(f);
        glAttachShader_(p, v); glAttachShader_(p, f);
        glBindAttribLocation_(p, 0, "position"); glBindAttribLocation_(p, 1, "color");
        glLinkProgram_(p);
        glUseProgram_(p);
        offset_location = glGetUniformLocation_(p, "offset");
        glBindBuffer_(GL_ARRAY_BUFFER, buffers[0]);
        glVertexAttribPointer_(0, 3, GL_FLOAT, GL_FALSE, 0, NULL);
        glEnableVertexAttribArray_(0);
        glBindBuffer_(GL_ARRAY_BUFFER, buffers[1]);
        glVertexAttribPointer_(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, NULL);
        glEnableVertexAttribArray_(1);
    } else {
        glBindBuffer_(GL_ARRAY_BUFFER, buffers[0]);
        glVertexPointer_(3, GL_FLOAT, 0, NULL);
        glEnableClientState_(GL_VERTEX_ARRAY);
        glBindBuffer_(GL_ARRAY_BUFFER, buffers[1]);
        glColorPointer_(4, GL_UNSIGNED_BYTE, 0, NULL);
        glEnableClientState_(GL_COLOR_ARRAY);
        if (!strcmp(scene, "arb")) {
            const char *vp = "!!ARBvp1.0\nPARAM o = program.local[0];\nPARAM mvp[4] = { state.matrix.mvp };\nTEMP p;\n"
                             "ADD p, vertex.position, o;\nMOV p.w, 1.0;\nDP4 result.position.x, mvp[0], p;\n"
                             "DP4 result.position.y, mvp[1], p;\nDP4 result.position.z, mvp[2], p;\n"
                             "DP4 result.position.w, mvp[3], p;\nMUL result.color, vertex.color, o.w;\nEND\n";
            const char *fp = "!!ARBfp1.0\nMOV result.color, fragment.color;\nEND\n";
            GLuint programs[2];
            glGenProgramsARB_(2, programs);
            glBindProgramARB_(GL_VERTEX_PROGRAM_ARB, programs[0]);
            glProgramStringARB_(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)strlen(vp), vp);
            glBindProgramARB_(GL_FRAGMENT_PROGRAM_ARB, programs[1]);
            glProgramStringARB_(GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)strlen(fp), fp);
            glEnable_(GL_VERTEX_PROGRAM_ARB);
            glEnable_(GL_FRAGMENT_PROGRAM_ARB);
        }
        glMatrixMode_(GL_MODELVIEW);
    }
    if (glGetError_()) { fprintf(stderr, "setup GL error\n"); return 1; }

    double total = 0, best = 1e9, submit_total = 0;
    for (int frame = 0; frame < frames + 5; ++frame) {
        double start = now();
        glClear_(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        for (int d = 0; d < draws; ++d) {
            float x = (float)(d % 80) / 40.0f - 0.99f, y = (float)(d / 80 % 60) / 30.0f - 0.99f, z = (float)(d % 7) * 0.1f;
            float w = 0.5f + 0.5f * (float)((d + frame) % 10) / 10.0f;
            if (offset_location >= 0) {
                glUniform4f_(offset_location, x, y, z, w);
            } else if (!strcmp(scene, "arb")) {
                glProgramLocalParameter4fARB_(GL_VERTEX_PROGRAM_ARB, 0, x, y, z, w);
            } else {
                glLoadIdentity_();
                glTranslatef_(x, y, z);
                glColor4f_(w, w, w, 1);
            }
            glDrawElements_(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, NULL);
        }
        double submitted = now() - start;
        glFinish_();
        double t = now() - start;
        if (frame >= 5) {
            total += t;
            submit_total += submitted;
            if (t < best) best = t;
        }
        /* Cool down between frames without including the pause in timings. */
        if (pause_ms) usleep((useconds_t)pause_ms * 1000);
    }
    if (glGetError_()) { fprintf(stderr, "GL error\n"); return 1; }
    unsigned char *pixels = malloc(1280 * 720 * 4);
    if (!pixels) return 1;
    glReadPixels_(0, 0, 1280, 720, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    size_t colored = 0;
    for (size_t i = 0; i < 1280 * 720; ++i)
        colored += pixels[4 * i] || pixels[4 * i + 1] || pixels[4 * i + 2];
    free(pixels);
    if (glGetError_() || !colored) { fprintf(stderr, "benchmark readback failed or image empty\n"); return 1; }
    fprintf(stderr, "validation: %zu colored pixels; pause %d ms outside timing\n", colored, pause_ms);
    double mean = total / frames;
    printf("%-7s %-5s draws=%d  frame %.2f ms (best %.2f)  submit %.2f ms  %.2f us/draw CPU\n",
           !strcmp(provider, "apple") ? "apple" : "glmetal", scene, draws, mean * 1e3, best * 1e3,
           submit_total / frames * 1e3, submit_total / frames * 1e6 / draws);
    return 0;
}
