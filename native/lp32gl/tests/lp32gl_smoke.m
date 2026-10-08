/* End-to-end check of LP32GL as a host uses it: dlopen'd RTLD_LOCAL next to
 * Apple's frameworks, NSOpenGL classes redirected, a legacy context drawing
 * with immediate mode and an ARB fragment program, and a core context
 * compiling GLSL 1.50.  Prints what it sees and exits non-zero on a failed
 * check.  usage: lp32gl_smoke <path/to/libLP32GL.dylib> [frames] */
#define GL_SILENCE_DEPRECATION 1
#import <Cocoa/Cocoa.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#define GL_MAJOR_VERSION 0x821B
#define GL_MINOR_VERSION 0x821C
#define GL_NUM_EXTENSIONS 0x821D
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *lib;
#define GLF(ret, name, ...) static ret (*p_##name)(__VA_ARGS__);
GLF(const GLubyte *, glGetString, GLenum)
GLF(const GLubyte *, glGetStringi, GLenum, GLuint)
GLF(void, glGetIntegerv, GLenum, GLint *)
GLF(void, glClearColor, float, float, float, float)
GLF(void, glClear, GLbitfield)
GLF(void, glBegin, GLenum)
GLF(void, glEnd, void)
GLF(void, glVertex2f, float, float)
GLF(void, glColor3f, float, float, float)
GLF(void, glReadPixels, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)
GLF(GLenum, glGetError, void)
GLF(void, glViewport, GLint, GLint, GLsizei, GLsizei)
GLF(void, glGenProgramsARB, GLsizei, GLuint *)
GLF(void, glBindProgramARB, GLenum, GLuint)
GLF(void, glProgramStringARB, GLenum, GLenum, GLsizei, const void *)
GLF(void, glProgramEnvParameters4fvEXT, GLenum, GLuint, GLsizei, const GLfloat *)
GLF(void, glGetProgramEnvParameterfvARB, GLenum, GLuint, GLfloat *)
GLF(void, glEnable, GLenum)
GLF(void, glDisable, GLenum)
GLF(GLuint, glCreateShader, GLenum)
GLF(void, glShaderSource, GLuint, GLsizei, const char *const *, const GLint *)
GLF(void, glCompileShader, GLuint)
GLF(void, glGetShaderiv, GLuint, GLenum, GLint *)
GLF(void, glGetShaderInfoLog, GLuint, GLsizei, GLsizei *, char *)
GLF(void, glGenFencesAPPLE, GLsizei, GLuint *)
GLF(void, glSetFenceAPPLE, GLuint)
GLF(void, glFinishFenceAPPLE, GLuint)
GLF(GLboolean, glTestFenceAPPLE, GLuint)
GLF(void, glPixelStorei, GLenum, GLint)
GLF(void, glFinish, void)
GLF(void, glGenTextures, GLsizei, GLuint *)
GLF(void, glBindTexture, GLenum, GLuint)
GLF(void, glTexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)
GLF(void, glGetTexImage, GLenum, GLint, GLenum, GLenum, void *)
GLF(GLboolean, glIsTexture, GLuint)
GLF(void, glCompressedTexImage2D, GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *)
GLF(void, glCompressedTexSubImage2D, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *)
GLF(void, glTexParameteri, GLenum, GLenum, GLint)
GLF(void, glTexCoord2f, float, float)
GLF(void, glGenBuffers, GLsizei, GLuint *)
GLF(void, glBindBuffer, GLenum, GLuint)
GLF(void, glBufferData, GLenum, GLsizeiptr, const void *, GLenum)
GLF(void, glVertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)
GLF(void, glEnableVertexAttribArray, GLuint)
GLF(void, glDisableVertexAttribArray, GLuint)
GLF(void, glDrawRangeElements, GLenum, GLuint, GLuint, GLsizei, GLenum, const void *)
static void load(void)
{
    void *(*get)(const char *) = dlsym(lib, "lp32gl_get_proc_address");
#define L(name) p_##name = get(#name); if (!p_##name) { fprintf(stderr, "missing %s\n", #name); exit(2); }
    L(glGetString) L(glGetStringi) L(glGetIntegerv) L(glClearColor) L(glClear) L(glBegin) L(glEnd)
    L(glVertex2f) L(glColor3f) L(glReadPixels) L(glGetError) L(glViewport) L(glGenProgramsARB)
    L(glBindProgramARB) L(glProgramStringARB) L(glProgramEnvParameters4fvEXT)
    L(glGetProgramEnvParameterfvARB) L(glEnable) L(glDisable) L(glCreateShader)
    L(glShaderSource) L(glCompileShader) L(glGetShaderiv) L(glGetShaderInfoLog)
    L(glGenFencesAPPLE) L(glSetFenceAPPLE) L(glFinishFenceAPPLE) L(glTestFenceAPPLE) L(glPixelStorei)
    L(glFinish) L(glGenTextures) L(glBindTexture) L(glTexImage2D) L(glGetTexImage) L(glIsTexture)
    L(glCompressedTexImage2D) L(glCompressedTexSubImage2D) L(glTexParameteri) L(glTexCoord2f)
    L(glGenBuffers) L(glBindBuffer) L(glBufferData) L(glVertexAttribPointer) L(glEnableVertexAttribArray)
    L(glDisableVertexAttribArray) L(glDrawRangeElements)
}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* Each 4x4 block of a DXT1 level is one solid RGB565 colour derived from
   its block coordinates, so a wrong row pitch or level offset shows up as a
   colour belonging to another block. */
static uint16_t dxt1_block_color(unsigned bx, unsigned by, unsigned level)
{
    unsigned r = (bx * 7 + level * 5) & 31, g = (by * 5 + level * 3) & 63, b = (bx + by * 3) & 31;
    return (uint16_t)(r << 11 | g << 5 | b);
}

static void expand565(uint16_t c, unsigned char rgb[3])
{
    unsigned r = c >> 11, g = (c >> 5) & 63, b = c & 31;
    rgb[0] = (unsigned char)(r << 3 | r >> 2);
    rgb[1] = (unsigned char)(g << 2 | g >> 4);
    rgb[2] = (unsigned char)(b << 3 | b >> 2);
}

static void check_dxt1_mip_uploads(void)
{
    enum { kSize = 256 };
    GLuint texture;
    p_glGenTextures(1, &texture);
    p_glBindTexture(GL_TEXTURE_2D, texture);
    static unsigned char levels[9][(kSize / 4) * (kSize / 4) * 8];
    int level_count = 0;
    for (int size = kSize; size >= 1; size >>= 1, ++level_count) {
        unsigned blocks = size < 4 ? 1 : (unsigned)size / 4;
        for (unsigned by = 0; by < blocks; ++by)
            for (unsigned bx = 0; bx < blocks; ++bx) {
                unsigned char *block = levels[level_count] + (by * blocks + bx) * 8;
                uint16_t c = dxt1_block_color(bx, by, (unsigned)level_count);
                block[0] = block[2] = (unsigned char)c;
                block[1] = block[3] = (unsigned char)(c >> 8);
                memset(block + 4, 0, 4);
            }
        p_glCompressedTexImage2D(GL_TEXTURE_2D, level_count, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, size, size, 0,
                                 (GLsizei)(blocks * blocks * 8), levels[level_count]);
    }
    /* COD4 re-specifies every level smallest first. */
    for (int level = level_count - 1; level >= 0; --level) {
        int size = kSize >> level;
        unsigned blocks = size < 4 ? 1 : (unsigned)size / 4;
        p_glCompressedTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, size, size, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
                                    (GLsizei)(blocks * blocks * 8), levels[level]);
    }
    CHECK(p_glGetError() == GL_NO_ERROR, "DXT1 upload error");
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    static unsigned char readback[kSize * kSize * 4];
    int readback_bad = 0, draw_bad = 0;
    for (int level = 0; level < 3; ++level) {
        int size = kSize >> level;
        p_glGetTexImage(GL_TEXTURE_2D, level, GL_RGBA, GL_UNSIGNED_BYTE, readback);
        for (int by = 0; by < size / 4; ++by)
            for (int bx = 0; bx < size / 4; ++bx) {
                unsigned char rgb[3];
                expand565(dxt1_block_color((unsigned)bx, (unsigned)by, (unsigned)level), rgb);
                const unsigned char *px = readback + ((by * 4 + 1) * size + bx * 4 + 1) * 4;
                if (abs(px[0] - rgb[0]) > 8 || abs(px[1] - rgb[1]) > 8 || abs(px[2] - rgb[2]) > 8) {
                    if (readback_bad++ < 4)
                        printf("dxt1 readback level=%d block=%d,%d got=%u,%u,%u want=%u,%u,%u\n", level, bx, by,
                               px[0], px[1], px[2], rgb[0], rgb[1], rgb[2]);
                }
            }
    }

    p_glDisable(GL_FRAGMENT_PROGRAM_ARB);
    p_glEnable(GL_TEXTURE_2D);
    p_glViewport(0, 0, kSize, kSize);
    p_glBegin(GL_QUADS);
    p_glColor3f(1, 1, 1);
    p_glTexCoord2f(0, 0); p_glVertex2f(-1, -1);
    p_glTexCoord2f(1, 0); p_glVertex2f(1, -1);
    p_glTexCoord2f(1, 1); p_glVertex2f(1, 1);
    p_glTexCoord2f(0, 1); p_glVertex2f(-1, 1);
    p_glEnd();
    p_glDisable(GL_TEXTURE_2D);
    p_glReadPixels(0, 0, kSize, kSize, GL_RGBA, GL_UNSIGNED_BYTE, readback);
    for (int by = 0; by < kSize / 4; ++by)
        for (int bx = 0; bx < kSize / 4; ++bx) {
            unsigned char rgb[3];
            expand565(dxt1_block_color((unsigned)bx, (unsigned)by, 0), rgb);
            const unsigned char *px = readback + ((by * 4 + 2) * kSize + bx * 4 + 2) * 4;
            if (abs(px[0] - rgb[0]) > 8 || abs(px[1] - rgb[1]) > 8 || abs(px[2] - rgb[2]) > 8) {
                if (draw_bad++ < 4)
                    printf("dxt1 draw block=%d,%d got=%u,%u,%u want=%u,%u,%u\n", bx, by, px[0], px[1], px[2],
                           rgb[0], rgb[1], rgb[2]);
            }
        }
    printf("dxt1: readback mismatches=%d draw mismatches=%d\n", readback_bad, draw_bad);
    CHECK(!readback_bad, "DXT1 readback");
    CHECK(!draw_bad, "DXT1 sampled draw");
}

/* COD4's model vertices: 32-byte stride, float4 position, then byte4
   attributes that its vertex programs read unnormalized (packed half-float
   texcoords, normals) or normalized (colour), drawn with DrawRangeElements. */
static void check_byte_vertex_attributes(void)
{
    struct vertex { float position[4]; unsigned char texcoord[4], normal[4], color[4], pad[4]; };
    struct vertex vertices[4];
    const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int i = 0; i < 4; ++i) {
        memset(&vertices[i], 0, sizeof vertices[i]);
        vertices[i].position[0] = corners[i][0];
        vertices[i].position[1] = corners[i][1];
        vertices[i].position[3] = 1;
        memcpy(vertices[i].texcoord, (unsigned char[]){200, 100, 50, 255}, 4);
        memcpy(vertices[i].normal, (unsigned char[]){30, 60, 90, 120}, 4);
        memcpy(vertices[i].color, (unsigned char[]){10, 220, 130, 255}, 4);
    }
    const unsigned short indices[6] = {0, 1, 2, 0, 2, 3};
    GLuint buffers[2];
    p_glGenBuffers(2, buffers);
    p_glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
    p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, indices, GL_STATIC_DRAW);
    p_glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (void *)0);
    p_glVertexAttribPointer(8, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(struct vertex), (void *)16);
    p_glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(struct vertex), (void *)20);
    p_glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(struct vertex), (void *)24);
    const GLuint attributes[4] = {0, 8, 2, 3};
    for (int i = 0; i < 4; ++i) p_glEnableVertexAttribArray(attributes[i]);

    const struct { const char *name; const char *input; float scale; unsigned char want[3]; } cases[] = {
        {"attrib[8] ubyte", "vertex.attrib[8]", 1.0f / 255, {200, 100, 50}},
        {"attrib[2] ubyte", "vertex.attrib[2]", 1.0f / 255, {30, 60, 90}},
        {"attrib[3] unorm", "vertex.attrib[3]", 1.0f, {10, 220, 130}},
    };
    GLuint programs[2];
    p_glGenProgramsARB(2, programs);
    const char *fp = "!!ARBfp1.0\nMOV result.color, fragment.color;\nEND\n";
    p_glBindProgramARB(GL_FRAGMENT_PROGRAM_ARB, programs[1]);
    p_glProgramStringARB(GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)strlen(fp), fp);
    p_glEnable(GL_FRAGMENT_PROGRAM_ARB);
    p_glEnable(GL_VERTEX_PROGRAM_ARB);
    p_glViewport(0, 0, 64, 64);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        char vp[512];
        snprintf(vp, sizeof vp,
                 "!!ARBvp1.0\nMOV result.position, vertex.attrib[0];\n"
                 "MUL result.color, %s, {%.9g, %.9g, %.9g, 1};\nEND\n",
                 cases[i].input, cases[i].scale, cases[i].scale, cases[i].scale);
        p_glBindProgramARB(GL_VERTEX_PROGRAM_ARB, programs[0]);
        p_glProgramStringARB(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)strlen(vp), vp);
        CHECK(p_glGetError() == GL_NO_ERROR, "vertex program for %s rejected", cases[i].name);
        p_glClearColor(0, 0, 0, 1);
        p_glClear(GL_COLOR_BUFFER_BIT);
        p_glDrawRangeElements(GL_TRIANGLES, 0, 3, 6, GL_UNSIGNED_SHORT, (void *)0);
        unsigned char px[4];
        p_glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        printf("vertex %s: got=%u,%u,%u want=%u,%u,%u\n", cases[i].name, px[0], px[1], px[2],
               cases[i].want[0], cases[i].want[1], cases[i].want[2]);
        CHECK(abs(px[0] - cases[i].want[0]) <= 2 && abs(px[1] - cases[i].want[1]) <= 2 &&
              abs(px[2] - cases[i].want[2]) <= 2, "vertex %s", cases[i].name);
    }
    p_glDisable(GL_VERTEX_PROGRAM_ARB);
    p_glDisable(GL_FRAGMENT_PROGRAM_ARB);
    for (int i = 0; i < 4; ++i) p_glDisableVertexAttribArray(attributes[i]);
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

static void pump(double seconds)
{
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
}

struct worker_result {
    NSOpenGLContext *context;
    GLuint texture;
    GLuint source_texture;
    GLubyte observed_pixel[4];
    GLenum error;
    int current_matches;
};

static void *upload_on_worker(void *argument)
{
    @autoreleasepool {
        struct worker_result *result = argument;
        [result->context makeCurrentContext];
        result->current_matches = [NSOpenGLContext currentContext] == result->context;
        p_glBindTexture(GL_TEXTURE_2D, result->source_texture);
        p_glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                        result->observed_pixel);
        GLubyte red[4] = {255, 0, 0, 255};
        p_glGenTextures(1, &result->texture);
        p_glBindTexture(GL_TEXTURE_2D, result->texture);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0,
                       GL_RGBA, GL_UNSIGNED_BYTE, red);
        p_glFinish();
        result->error = p_glGetError();
        [NSOpenGLContext clearCurrentContext];
    }
    return NULL;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        lib = dlopen(argc > 1 ? argv[1] : "libLP32GL.dylib", RTLD_NOW | RTLD_LOCAL);
        if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 2; }
        int frames = argc > 2 ? atoi(argv[2]) : 60;
        load();
        ((void (*)(void))dlsym(lib, "lp32gl_install_appkit_overrides"))();
        NSOpenGLPixelFormatAttribute zero_value_attrs[] = {
            NSOpenGLPFAColorSize, 32, NSOpenGLPFADepthSize, 0,
            NSOpenGLPFAStencilSize, 8, 0};
        NSOpenGLPixelFormat *zero_value_format =
            [[NSOpenGLPixelFormat alloc] initWithAttributes:zero_value_attrs];
        CHECK(zero_value_format, "pixel format stopped at a zero-valued attribute");
        [zero_value_format release];
        if (getenv("LP32GL_SMOKE_PIXEL_FORMAT_ONLY")) return failures ? 1 : 0;

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 200, 640, 400)
            styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        if (getenv("LP32GL_SMOKE_BACKGROUND")) [window orderFront:nil];
        else {
            [window makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
        }

        NSOpenGLPixelFormatAttribute legacy_attrs[] = {NSOpenGLPFADoubleBuffer, NSOpenGLPFAAccelerated,
            NSOpenGLPFAColorSize, 24, NSOpenGLPFADepthSize, 24, NSOpenGLPFAStencilSize, 8, 0};
        NSOpenGLPixelFormat *format = [[NSOpenGLPixelFormat alloc] initWithAttributes:legacy_attrs];
        printf("pixel format class: %s\n", object_getClassName(format));
        CHECK(format && !strcmp(object_getClassName(format), "LP32OpenGLPixelFormat"), "pixel format not redirected");
        NSOpenGLContext *context = [[NSOpenGLContext alloc] initWithFormat:format shareContext:nil];
        printf("context class: %s kind-of-NSOpenGLContext=%d\n", object_getClassName(context),
               [context isKindOfClass:[NSOpenGLContext class]]);
        CHECK(context, "no legacy context");
        [context setView:window.contentView];
        [context makeCurrentContext];
        CHECK([NSOpenGLContext currentContext] == context, "currentContext mismatch");
        GLint one = 1;
        [context setValues:&one forParameter:NSOpenGLContextParameterSwapInterval];
        printf("legacy: VENDOR=%s RENDERER=%s VERSION=%s GLSL=%s\n", p_glGetString(GL_VENDOR),
               p_glGetString(GL_RENDERER), p_glGetString(GL_VERSION),
               p_glGetString(GL_SHADING_LANGUAGE_VERSION));
        CHECK(!strncmp((const char *)p_glGetString(GL_VERSION), "2.1 ", 4), "legacy version");
        const char *ext = (const char *)p_glGetString(GL_EXTENSIONS);
        int count = 0;
        for (const char *c = ext; *c; ++c) count += *c == ' ';
        printf("legacy extensions: %d\n", count + 1);
        CHECK(strstr(ext, "GL_APPLE_fence") && strstr(ext, "GL_ARB_fragment_program "), "extension list");
        p_glPixelStorei(0x85B2 /* UNPACK_CLIENT_STORAGE_APPLE */, 1);
        CHECK(p_glGetError() == GL_NO_ERROR, "client storage enum rejected");

        /* ARB fragment program: output solid green. */
        GLuint program;
        const char *fp = "!!ARBfp1.0\nMOV result.color, {0.0, 1.0, 0.0, 1.0};\nEND\n";
        p_glGenProgramsARB(1, &program);
        p_glBindProgramARB(GL_FRAGMENT_PROGRAM_ARB, program);
        p_glProgramStringARB(GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)strlen(fp), fp);
        CHECK(p_glGetError() == GL_NO_ERROR, "ARB program rejected");

        const GLfloat expected_env[8] = {0.125f, -0.5f, 3.0f, 1.0f,
                                         0.75f, 2.0f, -4.0f, 0.25f};
        GLfloat observed_env[4] = {0};
        p_glProgramEnvParameters4fvEXT(GL_FRAGMENT_PROGRAM_ARB, 8, 2,
                                      expected_env);
        CHECK(p_glGetError() == GL_NO_ERROR, "bulk ARB program environment upload rejected");
        for (GLuint index = 0; index < 2; ++index) {
            p_glGetProgramEnvParameterfvARB(GL_FRAGMENT_PROGRAM_ARB, 8 + index,
                                           observed_env);
            CHECK(!memcmp(observed_env, expected_env + index * 4,
                          sizeof observed_env),
                  "bulk ARB program environment vector %u mismatch", index);
        }

        GLuint fence;
        p_glGenFencesAPPLE(1, &fence);
        for (int frame = 0; frame < frames; ++frame) {
            p_glViewport(0, 0, 640, 400);
            p_glClearColor(0.1f, 0.1f, 0.4f, 1);
            p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            p_glDisable(GL_FRAGMENT_PROGRAM_ARB);
            p_glBegin(GL_TRIANGLES);
            p_glColor3f(1, 0, 0); p_glVertex2f(-0.9f, -0.9f);
            p_glColor3f(1, 0, 0); p_glVertex2f(-0.1f, -0.9f);
            p_glColor3f(1, 0, 0); p_glVertex2f(-0.5f, 0.9f);
            p_glEnd();
            p_glEnable(GL_FRAGMENT_PROGRAM_ARB);
            p_glBegin(GL_TRIANGLES);
            p_glVertex2f(0.1f, -0.9f); p_glVertex2f(0.9f, -0.9f); p_glVertex2f(0.5f, 0.9f);
            p_glEnd();
            if (frame == frames - 1) {
                unsigned char left[4], right[4], bg[4];
                p_glReadPixels(160, 150, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, left);
                p_glReadPixels(480, 150, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, right);
                p_glReadPixels(320, 390, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bg);
                printf("pixels: fixed-function=%u,%u,%u arb=%u,%u,%u clear=%u,%u,%u\n", left[0], left[1],
                       left[2], right[0], right[1], right[2], bg[0], bg[1], bg[2]);
                CHECK(left[0] > 200 && left[1] < 50, "fixed-function triangle");
                CHECK(right[1] > 200 && right[0] < 50, "ARB program triangle");
                CHECK(bg[2] > 90 && bg[2] < 115, "clear color");
            }
            p_glSetFenceAPPLE(fence);
            [context flushBuffer];
            pump(0.001);
        }
        check_dxt1_mip_uploads();
        check_byte_vertex_attributes();
        p_glFinishFenceAPPLE(fence);
        CHECK(p_glTestFenceAPPLE(fence), "fence not signalled");
        CHECK(p_glGetError() == GL_NO_ERROR, "GL error after legacy frames");

        /* Core profile. */
        NSOpenGLPixelFormatAttribute core_attrs[] = {NSOpenGLPFADoubleBuffer, NSOpenGLPFAAccelerated,
            NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion3_2Core, NSOpenGLPFAColorSize, 24, 0};
        NSOpenGLPixelFormat *core_format = [[NSOpenGLPixelFormat alloc] initWithAttributes:core_attrs];
        NSOpenGLContext *core = [[NSOpenGLContext alloc] initWithFormat:core_format shareContext:nil];
        CHECK(core, "no core context");
        [core makeCurrentContext];
        GLint major = 0, minor = 0, n = 0;
        p_glGetIntegerv(GL_MAJOR_VERSION, &major);
        p_glGetIntegerv(GL_MINOR_VERSION, &minor);
        p_glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        printf("core: VERSION=%s GLSL=%s (%d.%d) extensions=%d first=%s\n", p_glGetString(GL_VERSION),
               p_glGetString(GL_SHADING_LANGUAGE_VERSION), major, minor, n, n ? (const char *)p_glGetStringi(GL_EXTENSIONS, 0) : "");
        CHECK(major == 4 && minor == 1, "core version");
        GLint default_vertex_array = 0;
        p_glGetIntegerv(0x85B5 /* GL_VERTEX_ARRAY_BINDING */, &default_vertex_array);
        CHECK(default_vertex_array > 0, "core default vertex array missing");
        GLubyte shared_pixel[4] = {17, 39, 83, 255};
        GLuint source_texture = 0;
        p_glGenTextures(1, &source_texture);
        p_glBindTexture(GL_TEXTURE_2D, source_texture);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0,
                       GL_RGBA, GL_UNSIGNED_BYTE, shared_pixel);
        struct worker_result worker = {.context = core, .source_texture = source_texture};
        pthread_t thread;
        int thread_error = pthread_create(&thread, NULL, upload_on_worker, &worker);
        CHECK(thread_error == 0, "worker thread creation failed");
        if (!thread_error) {
            pthread_join(thread, NULL);
            CHECK(worker.current_matches, "worker current context mismatch");
            CHECK(worker.error == GL_NO_ERROR, "worker texture upload failed");
            CHECK(memcmp(worker.observed_pixel, shared_pixel, sizeof shared_pixel) == 0,
                  "worker did not see texture uploaded on original context");
            CHECK(worker.texture && p_glIsTexture(worker.texture), "worker texture not shared");
        }
        const char *vs = "#version 150\nin vec4 p; out vec4 c; void main(){ gl_Position = p; c = p; }\n";
        GLuint shader = p_glCreateShader(GL_VERTEX_SHADER);
        p_glShaderSource(shader, 1, &vs, NULL);
        p_glCompileShader(shader);
        GLint ok = 0;
        p_glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[1024]; p_glGetShaderInfoLog(shader, sizeof log, NULL, log); printf("GLSL log: %s\n", log); }
        CHECK(ok, "GLSL 1.50 compile");
        p_glBegin(GL_TRIANGLES);
        CHECK(p_glGetError() == GL_INVALID_OPERATION, "glBegin must fail in core");
        [NSOpenGLContext clearCurrentContext];
        printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    }
    return failures ? 1 : 0;
}
