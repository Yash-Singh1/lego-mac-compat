/* GL 3.x/4.x core entry points beyond drawing: typed clears, reflection and
   indexed queries. Query results are drawn as colours so they compare
   pixel for pixel like everything else. */
#include "glc_gl_core.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glcompare.h"

#define GLSL "#version 410 core\n"

/* One 8x8 cell per integer, its low byte as red, the next as green. */
static void show_ints(const int *values, int count)
{
    glEnable(GL_SCISSOR_TEST);
    for (int i = 0; i < count && i < 64; ++i) {
        unsigned v = (unsigned)values[i];
        glScissor((i % 8) * 8, (i / 8) * 8, 8, 8);
        glClearColor((float)(v & 0xff) / 255.0f, (float)((v >> 8) & 0xff) / 255.0f, values[i] < 0 ? 1.0f : 0.0f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
}

/* The framebuffer the runner captures (an FBO, not the window's 0). */
static GLuint target_fbo(void)
{
    static GLint fbo = -1;
    if (fbo < 0) glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    return (GLuint)fbo;
}

static GLuint link(const char *vs, const char *fs)
{
    GLuint program = glCreateProgram();
    const char *sources[2] = {vs, fs};
    const GLenum types[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
    for (int i = 0; i < 2; ++i) {
        GLuint shader = glCreateShader(types[i]);
        glShaderSource(shader, 1, &sources[i], NULL);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = "";
            glGetShaderInfoLog(shader, sizeof log, NULL, log);
            glc_fail("stage %d: %s", i, log);
        }
        glAttachShader(program, shader);
    }
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        glGetProgramInfoLog(program, sizeof log, NULL, log);
        glc_fail("link: %s", log);
    }
    glUseProgram(program);
    return program;
}

/* Implicit stage interfaces match names even when declaration order differs.
   An array before a matrix also checks that their locations span multiple slots. */
GLC_CASE(gl4_varying_name_link, .profile = GLC_CORE, .tolerance = 0)
{
    const char *vs = GLSL
        "out vec4 texCoord[2]; out mat2 basis; out vec4 color;\n"
        "void main(){ vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
        " gl_Position = vec4(p * 2.0 - 1.0, 0, 1);\n"
        " texCoord[0] = vec4(0.125); texCoord[1] = vec4(0.25);\n"
        " basis = mat2(0.5, 0.25, 0.125, 0.75); color = vec4(0, 1, 0, 1); }";
    const char *fs = GLSL
        "in vec4 color; in mat2 basis; in vec4 texCoord[2]; out vec4 frag;\n"
        "void main(){ bool ok = all(equal(texCoord[0], vec4(0.125)))\n"
        " && all(equal(texCoord[1], vec4(0.25)))\n"
        " && all(equal(basis[0], vec2(0.5, 0.25)))\n"
        " && all(equal(basis[1], vec2(0.125, 0.75)));\n"
        " frag = ok ? color : vec4(1, 0, 0, 1); }";
    link(vs, fs);
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    GLubyte pixel[4];
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (pixel[0] != 0 || pixel[1] != 255 || pixel[2] != 0 || pixel[3] != 255)
        glc_fail("varying names linked incorrectly: %u %u %u %u", pixel[0], pixel[1], pixel[2], pixel[3]);
}

static GLuint color_texture(GLenum internal, GLenum format, GLenum type)
{
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, 64, 64, 0, format, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return texture;
}

static const char *const clear_names[] = {"float_buffers", "integer", "depth_stencil"};
GLC_CASE_VARIANTS(gl4_clear_buffer, clear_names, .profile = GLC_CORE)
{
    GLuint target = target_fbo();
    glClearColor(0.2f, 0.2f, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    switch (glc_variant) {
    case 0: {
        /* Two float targets; each cleared on its own, the second under a
           scissor and colour mask. */
        GLuint a = color_texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE), b = color_texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, a, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, b, 0);
        const GLenum buffers[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(2, buffers);
        const GLfloat red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1}, blue[4] = {0, 0, 1, 1};
        glClearBufferfv(GL_COLOR, 0, red);
        glClearBufferfv(GL_COLOR, 1, green);
        glEnable(GL_SCISSOR_TEST);
        glScissor(8, 8, 24, 40);
        glColorMaski(1, GL_FALSE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearBufferfv(GL_COLOR, 1, blue);
        glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBlitFramebuffer(0, 0, 64, 64, 0, 0, 32, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glReadBuffer(GL_COLOR_ATTACHMENT1);
        glBlitFramebuffer(0, 0, 64, 64, 32, 0, 64, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        break;
    }
    case 1: {
        /* Signed and unsigned integer targets, read back as integers. */
        GLuint si = color_texture(GL_RGBA32I, GL_RGBA_INTEGER, GL_INT);
        GLuint ui = color_texture(GL_RGBA16UI, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, si, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, ui, 0);
        const GLenum buffers[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(2, buffers);
        const GLint ints[4] = {-7, 300, 70000, -1};
        const GLuint uints[4] = {9, 65535, 1234, 42};
        glClearBufferiv(GL_COLOR, 0, ints);
        glClearBufferuiv(GL_COLOR, 1, uints);
        int values[16];
        GLint pixel[4];
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(5, 5, 1, 1, GL_RGBA_INTEGER, GL_INT, pixel);
        for (int i = 0; i < 4; ++i) values[i] = pixel[i];
        GLuint upixel[4];
        glReadBuffer(GL_COLOR_ATTACHMENT1);
        glReadPixels(60, 60, 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, upixel);
        for (int i = 0; i < 4; ++i) values[4 + i] = (int)upixel[i];
        values[8] = (int)glGetError();
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        show_ints(values, 9);
        break;
    }
    case 2: {
        /* Depth and stencil cleared by glClearBuffer, then tested against. */
        GLuint color = color_texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE), depth;
        glGenRenderbuffers(1, &depth);
        glBindRenderbuffer(GL_RENDERBUFFER, depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 64, 64);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
        const GLfloat black[4] = {0, 0, 0, 1};
        glClearBufferfv(GL_COLOR, 0, black);
        glClearBufferfi(GL_DEPTH_STENCIL, 0, 0.5f, 3);
        glEnable(GL_SCISSOR_TEST);
        glScissor(32, 0, 32, 64);
        const GLfloat far_depth = 1.0f;
        glClearBufferfv(GL_DEPTH, 0, &far_depth);
        glScissor(0, 32, 64, 32);
        const GLint one = 1;
        glClearBufferiv(GL_STENCIL, 0, &one);
        glDisable(GL_SCISSOR_TEST);
        link(GLSL "layout(location=0) in vec2 p; uniform float z; void main(){ gl_Position = vec4(p, z, 1.0); }",
             GLSL "uniform vec4 c; out vec4 frag; void main(){ frag = c; }");
        GLuint program = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, (GLint *)&program);
        static const float quad[] = {-1, -1, 1, -1, 1, 1, -1, 1};
        GLuint vao, vbo;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glUniform1f(glGetUniformLocation(program, "z"), 0.5f); /* window depth 0.75 */
        glUniform4f(glGetUniformLocation(program, "c"), 1, 0.5f, 0, 1);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_EQUAL, 3, 0xff);
        glUniform4f(glGetUniformLocation(program, "c"), 0, 0.5f, 1, 1);
        glUniform1f(glGetUniformLocation(program, "z"), 0.0f);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glDisable(GL_STENCIL_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
        glBlitFramebuffer(0, 0, 64, 64, 0, 0, 64, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        break;
    }
    }
}

GLC_CASE(gl4_uniform_block_reflection, .profile = GLC_CORE)
{
    GLuint program = link(
        GLSL "struct S { vec2 x; int y; };\n"
             "layout(std140) uniform Params { float a; vec3 b; mat4 c; float d[3]; S s[2]; layout(row_major) mat3 e; ivec2 f; } params;\n"
             "layout(std140) uniform Other { vec4 g; };\n"
             "uniform vec4 loose;\n"
             "layout(location=0) in vec2 p;\n"
             "void main(){ gl_Position = vec4(p, params.a + params.b.x + params.c[0][0] + params.d[2] + params.s[1].x.y + float(params.s[0].y)\n"
             "  + params.e[1][2] + float(params.f.y), 1.0) + loose; }",
        GLSL "layout(std140) uniform Other { vec4 g; }; out vec4 frag; void main(){ frag = g; }");
    static const char *const names[] = {"Params.a", "Params.b", "Params.c", "Params.d", "Params.d[0]", "Params.s[1].x",
                                        "Params.s[0].y", "Params.e", "Params.f", "g", "loose", "missing"};
    enum { N = sizeof names / sizeof names[0] };
    GLuint indices[N];
    glGetUniformIndices(program, N, names, indices);
    int values[64];
    int n = 0;
    GLint active = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &active);
    values[n++] = active;
    values[n++] = indices[N - 1] == GL_INVALID_INDEX;
    static const GLenum pnames[] = {GL_UNIFORM_TYPE, GL_UNIFORM_SIZE, GL_UNIFORM_OFFSET, GL_UNIFORM_ARRAY_STRIDE,
                                    GL_UNIFORM_MATRIX_STRIDE, GL_UNIFORM_IS_ROW_MAJOR};
    GLuint present[N - 1];
    for (int i = 0; i < N - 1; ++i) present[i] = indices[i];
    for (size_t k = 0; k < sizeof pnames / sizeof pnames[0]; ++k) {
        GLint out[N - 1];
        glGetActiveUniformsiv(program, N - 1, present, pnames[k], out);
        for (int i = 0; i < N - 1 && n < 64; ++i)
            if (pnames[k] != GL_UNIFORM_TYPE || i < 4) values[n++] = pnames[k] == GL_UNIFORM_TYPE ? out[i] & 0xffff : out[i];
    }
    GLuint params = glGetUniformBlockIndex(program, "Params"), other = glGetUniformBlockIndex(program, "Other");
    GLint size = 0, count = 0, vertex = 0, fragment = 0;
    glGetActiveUniformBlockiv(program, params, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
    glGetActiveUniformBlockiv(program, params, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &count);
    glGetActiveUniformBlockiv(program, other, GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER, &vertex);
    glGetActiveUniformBlockiv(program, other, GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER, &fragment);
    if (n < 60) {
        values[n++] = size;
        values[n++] = count;
        values[n++] = vertex;
        values[n++] = fragment;
    }
    show_ints(values, n);
}

GLC_CASE(gl4_indexed_queries, .profile = GLC_CORE)
{
    int values[64];
    int n = 0;
    GLuint buffer;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_UNIFORM_BUFFER, buffer);
    glBufferData(GL_UNIFORM_BUFFER, 4096, NULL, GL_STATIC_DRAW);
    glBindBufferRange(GL_UNIFORM_BUFFER, 3, buffer, 512, 1024);
    GLint i = 0;
    GLint64 wide = 0;
    glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, 3, &i);
    values[n++] = i == (GLint)buffer;
    glGetInteger64i_v(GL_UNIFORM_BUFFER_START, 3, &wide);
    values[n++] = (int)wide;
    glGetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, 3, &wide);
    values[n++] = (int)wide;
    glGetInteger64v(GL_MAX_UNIFORM_BLOCK_SIZE, &wide);
    values[n++] = (int)(wide >> 8);
    /* Viewport arrays: the plain calls set every index. */
    glViewport(1, 2, 30, 40);
    glViewportIndexedf(2, 4.5f, 5, 6, 7);
    GLfloat f[4];
    glGetFloati_v(GL_VIEWPORT, 1, f);
    for (int k = 0; k < 4; ++k) values[n++] = (int)f[k];
    glGetFloati_v(GL_VIEWPORT, 2, f);
    for (int k = 0; k < 4; ++k) values[n++] = (int)(f[k] * 2);
    GLint v[4];
    glGetIntegerv(GL_VIEWPORT, v);
    for (int k = 0; k < 4; ++k) values[n++] = v[k];
    glScissorIndexed(3, 9, 8, 7, 6);
    glGetIntegeri_v(GL_SCISSOR_BOX, 3, v);
    for (int k = 0; k < 4; ++k) values[n++] = v[k];
    glEnablei(GL_SCISSOR_TEST, 3);
    values[n++] = glIsEnabledi(GL_SCISSOR_TEST, 3);
    values[n++] = glIsEnabledi(GL_SCISSOR_TEST, 0);
    glDepthRangeIndexed(1, 0.25, 0.75);
    GLdouble d[2];
    glGetDoublei_v(GL_DEPTH_RANGE, 1, d);
    values[n++] = (int)(d[0] * 100);
    values[n++] = (int)(d[1] * 100);
    glGetIntegerv(GL_MAX_VIEWPORTS, &i);
    values[n++] = i;
    values[n++] = (int)glGetError();
    glGetIntegeri_v(GL_VIEWPORT, 99, v);
    values[n++] = (int)glGetError();
    glViewport(0, 0, 64, 64);
    glDisablei(GL_SCISSOR_TEST, 3);
    show_ints(values, n);
}

/* A full-window quad through a program; `fs` gets `uv` in [0, 1]. */
static GLuint fullscreen(const char *fs)
{
    GLuint program = link(GLSL "layout(location=0) in vec2 p; out vec2 uv; void main(){ uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }", fs);
    static const float quad[] = {-1, -1, 1, -1, 1, 1, -1, 1};
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    return program;
}

static void fill_rgba8(unsigned char *p, int w, int h, int layer, int level)
{
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            unsigned char *px = p + (y * w + x) * 4;
            px[0] = (unsigned char)(x * 255 / (w > 1 ? w - 1 : 1));
            px[1] = (unsigned char)(y * 255 / (h > 1 ? h - 1 : 1));
            px[2] = (unsigned char)(layer * 60 + level * 40);
            px[3] = 255;
        }
}

static const char *const texture_names[] = {"storage_levels", "array_2d", "array_1d", "cube_array", "integer",
                                            "multisample_fetch", "immutable_queries"};
GLC_CASE_VARIANTS(gl4_textures, texture_names, .profile = GLC_CORE, .tolerance = 1, .outlier_fraction = 0.003)
{
    GLuint target = target_fbo();
    glClearColor(0.1f, 0.1f, 0.1f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char *pixels = malloc(64 * 64 * 4 * 6);
    GLuint texture;
    glGenTextures(1, &texture);
    GLuint program = 0;
    switch (glc_variant) {
    case 0: {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexStorage2D(GL_TEXTURE_2D, 4, GL_RGBA8, 16, 16);
        for (int level = 0; level < 4; ++level) {
            int size = 16 >> level;
            fill_rgba8(pixels, size, size, 0, level);
            glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        program = fullscreen(GLSL "uniform sampler2D t; in vec2 uv; out vec4 frag;\n"
                                  "void main(){ frag = textureLod(t, uv, floor(uv.x * 4.0)); }");
        break;
    }
    case 1: {
        glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 8, 8, 3, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        for (int layer = 0; layer < 3; ++layer) {
            fill_rgba8(pixels, 8, 8, layer, 0);
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, 8, 8, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        }
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        program = fullscreen(GLSL "uniform sampler2DArray t; in vec2 uv; out vec4 frag;\n"
                                  "void main(){ frag = texture(t, vec3(fract(uv * 2.0), floor(uv.x * 2.0) + floor(uv.y * 2.0))); }");
        break;
    }
    case 2: {
        glBindTexture(GL_TEXTURE_1D_ARRAY, texture);
        for (int layer = 0; layer < 4; ++layer) fill_rgba8(pixels + layer * 16 * 4, 16, 1, layer, 0);
        glTexImage2D(GL_TEXTURE_1D_ARRAY, 0, GL_RGBA8, 16, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        program = fullscreen(GLSL "uniform sampler1DArray t; in vec2 uv; out vec4 frag;\n"
                                  "void main(){ frag = texture(t, vec2(uv.x, floor(uv.y * 4.0))) + vec4(0.0, 0.0, 0.0, 0.0); }");
        break;
    }
    case 3: {
        glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, texture);
        for (int face = 0; face < 12; ++face) fill_rgba8(pixels + face * 8 * 8 * 4, 8, 8, face % 4, face / 4);
        glTexImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 0, GL_RGBA8, 8, 8, 12, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        program = fullscreen(GLSL "uniform samplerCubeArray t; in vec2 uv; out vec4 frag;\n"
                                  "void main(){ vec2 c = fract(uv * vec2(2.0, 1.0)) * 2.0 - 1.0;\n"
                                  " vec3 d = normalize(vec3(c, 0.6) * vec3(1.0, 1.0, uv.y > 0.5 ? -1.0 : 1.0));\n"
                                  " frag = texture(t, vec4(d, floor(uv.x * 2.0))); }");
        break;
    }
    case 4: {
        /* Signed 2D and unsigned 3D integer textures. */
        GLint ints[16 * 16];
        for (int i = 0; i < 16 * 16; ++i) ints[i] = (i * 37) % 511 - 255;
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32I, 16, 16, 0, GL_RED_INTEGER, GL_INT, ints);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        GLuint volume;
        glGenTextures(1, &volume);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_3D, volume);
        GLushort shorts[4 * 4 * 4 * 2];
        for (int i = 0; i < 4 * 4 * 4 * 2; ++i) shorts[i] = (GLushort)(i * 1000);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RG16UI, 4, 4, 4, 0, GL_RG_INTEGER, GL_UNSIGNED_SHORT, shorts);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glActiveTexture(GL_TEXTURE0);
        program = fullscreen(GLSL "uniform isampler2D a; uniform usampler3D b; in vec2 uv; out vec4 frag;\n"
                                  "void main(){ int v = texture(a, uv).r; uvec2 w = texelFetch(b, ivec3(uv * 4.0, int(uv.x * 3.99)), 0).rg;\n"
                                  " frag = vec4(float(v + 255) / 510.0, float(w.x) / 128000.0, float(w.y % 7000u) / 7000.0, 1.0); }");
        glUniform1i(glGetUniformLocation(program, "b"), 1);
        break;
    }
    case 5: {
        /* Render into a 4-sample texture, then read each sample back. */
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, texture);
        glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA8, 64, 64, GL_TRUE);
        GLuint fbo;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, texture, 0);
        glClearColor(0, 0, 0.5f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        GLuint tri = link(GLSL "layout(location=0) in vec2 p; void main(){ gl_Position = vec4(p, 0.0, 1.0); }",
                          GLSL "out vec4 frag; void main(){ frag = vec4(1.0, 0.8, 0.0, 1.0); }");
        (void)tri;
        static const float triangle[] = {-0.9f, -0.8f, 0.85f, -0.3f, -0.2f, 0.9f};
        GLuint vao, vbo;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof triangle, triangle, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        program = fullscreen(GLSL "uniform sampler2DMS t; in vec2 uv; out vec4 frag;\n"
                                  "void main(){ ivec2 p = ivec2(gl_FragCoord.xy); vec4 sum = vec4(0.0);\n"
                                  " for (int i = 0; i < 4; ++i) sum += texelFetch(t, p, i);\n"
                                  " frag = uv.x < 0.5 ? sum / 4.0 : texelFetch(t, p, 3); }");
        break;
    }
    case 6: {
        int values[16];
        int n = 0;
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexStorage2D(GL_TEXTURE_2D, 3, GL_RGBA8, 32, 8);
        GLint v = 0;
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_FORMAT, &v);
        values[n++] = v;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 2, GL_TEXTURE_WIDTH, &v);
        values[n++] = v;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 2, GL_TEXTURE_HEIGHT, &v);
        values[n++] = v;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 3, GL_TEXTURE_WIDTH, &v);
        values[n++] = v;
        values[n++] = (int)glGetError();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        values[n++] = (int)glGetError();
        GLuint ms;
        glGenTextures(1, &ms);
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, ms);
        glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA8, 16, 16, GL_FALSE);
        glGetTexLevelParameteriv(GL_TEXTURE_2D_MULTISAMPLE, 0, GL_TEXTURE_SAMPLES, &v);
        values[n++] = v;
        glGetTexLevelParameteriv(GL_TEXTURE_2D_MULTISAMPLE, 0, GL_TEXTURE_FIXED_SAMPLE_LOCATIONS, &v);
        values[n++] = v;
        values[n++] = (int)glGetError();
        free(pixels);
        show_ints(values, n);
        return;
    }
    }
    free(pixels);
    glUniform1i(glGetUniformLocation(program, "t"), 0);
    glUniform1i(glGetUniformLocation(program, "a"), 0);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

/* Separate shader objects: stage programs with same-named uniforms of
   their own, set by glProgramUniform and through the active program. */
static const char *const sso_names[] = {"two_programs", "switch_fragment", "active_program_uniform", "queries"};
GLC_CASE_VARIANTS(gl4_separate_programs, sso_names, .profile = GLC_CORE)
{
    glClearColor(0.1f, 0.1f, 0.1f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    static const char *vs =
        GLSL "out gl_PerVertex { vec4 gl_Position; };\n"
             "layout(location=0) in vec2 p; uniform vec4 tint; uniform vec2 offset; layout(location=0) out vec4 v;\n"
             "void main(){ gl_Position = vec4(p * 0.45 + offset, 0.0, 1.0); v = tint * vec4(p * 0.5 + 0.5, 1.0, 1.0); }";
    static const char *fs_a = GLSL "layout(location=0) in vec4 v; uniform vec4 tint; out vec4 frag; void main(){ frag = v + tint; }";
    static const char *fs_b = GLSL "layout(location=0) in vec4 v; uniform float scale; out vec4 frag; void main(){ frag = v.bgra * scale; }";
    GLuint vp = glCreateShaderProgramv(GL_VERTEX_SHADER, 1, &vs);
    GLuint fa = glCreateShaderProgramv(GL_FRAGMENT_SHADER, 1, &fs_a);
    GLuint fb = glCreateShaderProgramv(GL_FRAGMENT_SHADER, 1, &fs_b);
    GLint linked = 0;
    glGetProgramiv(fa, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024] = "";
        glGetProgramInfoLog(fa, sizeof log, NULL, log);
        glc_fail("fragment program: %s", log);
    }
    GLuint pipeline;
    glGenProgramPipelines(1, &pipeline);
    glBindProgramPipeline(pipeline);
    glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vp);
    glUseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fa);
    static const float quad[] = {-1, -1, 1, -1, 1, 1, -1, 1};
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glProgramUniform4f(vp, glGetUniformLocation(vp, "tint"), 1, 0.5f, 0.25f, 1);
    glProgramUniform4f(fa, glGetUniformLocation(fa, "tint"), 0, 0, 0.5f, 0);
    glProgramUniform1f(fb, glGetUniformLocation(fb, "scale"), 0.75f);
    GLint offset = glGetUniformLocation(vp, "offset");
    switch (glc_variant) {
    case 0:
        glProgramUniform2f(vp, offset, -0.5f, 0);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glProgramUniform2f(vp, offset, 0.5f, 0);
        glProgramUniform4f(fa, glGetUniformLocation(fa, "tint"), 0.5f, 0, 0, 0);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        break;
    case 1:
        glProgramUniform2f(vp, offset, -0.5f, 0.5f);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glUseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fb);
        glProgramUniform2f(vp, offset, 0.5f, -0.5f);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        /* A program bound with glUseProgram wins over the pipeline. */
        GLuint whole = link(GLSL "layout(location=0) in vec2 p; void main(){ gl_Position = vec4(p * 0.2, 0.0, 1.0); }",
                            GLSL "out vec4 frag; void main(){ frag = vec4(0.9, 0.9, 0.9, 1.0); }");
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glUseProgram(0);
        (void)whole;
        glProgramUniform2f(vp, offset, 0.5f, 0.5f);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        break;
    case 2:
        glActiveShaderProgram(pipeline, vp);
        glUniform2f(offset, 0, -0.4f);
        glUniform4f(glGetUniformLocation(vp, "tint"), 0.2f, 1, 0.2f, 1);
        glActiveShaderProgram(pipeline, fa);
        glUniform4f(glGetUniformLocation(fa, "tint"), 0.3f, 0, 0.3f, 0);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        break;
    case 3: {
        int values[16];
        int n = 0;
        GLint v = 0;
        glGetProgramPipelineiv(pipeline, GL_VERTEX_SHADER, &v);
        values[n++] = v == (GLint)vp;
        glGetProgramPipelineiv(pipeline, GL_FRAGMENT_SHADER, &v);
        values[n++] = v == (GLint)fa;
        glGetProgramPipelineiv(pipeline, GL_GEOMETRY_SHADER, &v);
        values[n++] = v;
        glGetProgramiv(vp, GL_PROGRAM_SEPARABLE, &v);
        values[n++] = v;
        glValidateProgramPipeline(pipeline);
        glGetProgramPipelineiv(pipeline, GL_VALIDATE_STATUS, &v);
        values[n++] = v;
        values[n++] = glIsProgramPipeline(pipeline);
        values[n++] = (int)glGetError();
        GLuint plain = glCreateProgram();
        glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, plain);
        values[n++] = (int)glGetError();
        show_ints(values, n);
        return;
    }
    }
}

static void quad_buffers(void)
{
    /* Two quads side by side: position xy, color rgb. */
    static const float vertices[] = {
        -0.9f, -0.8f, 1, 0, 0, -0.1f, -0.8f, 0, 1, 0, -0.1f, 0.8f, 0, 0, 1, -0.9f, 0.8f, 1, 1, 0,
        0.1f, -0.8f, 0, 1, 1, 0.9f, -0.8f, 1, 0, 1, 0.9f, 0.8f, 1, 1, 1, 0.1f, 0.8f, 0.5f, 0.5f, 0.5f,
    };
    static const GLushort indices[] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    GLuint vao, buffers[2];
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(2, buffers);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, indices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 20, 0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 20, (void *)8);
}

static const char *const draw_names[] = {"arrays_indirect", "elements_indirect", "multi_base_vertex", "dual_source_layout",
                                         "dual_source_bound"};
GLC_CASE_VARIANTS(gl4_draws, draw_names, .profile = GLC_CORE)
{
    glClearColor(0.1f, 0.1f, 0.1f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    quad_buffers();
    const char *vs = GLSL "layout(location=0) in vec2 p; layout(location=1) in vec3 c; out vec3 vc;\n"
                          "void main(){ gl_Position = vec4(p * (1.0 - 0.3 * float(gl_InstanceID)), 0.0, 1.0); vc = c * (1.0 - 0.4 * float(gl_InstanceID)); }";
    switch (glc_variant) {
    case 0: {
        link(vs, GLSL "in vec3 vc; out vec4 frag; void main(){ frag = vec4(vc, 1.0); }");
        const GLuint commands[] = {0, 0, 0, 0, /* skipped: offset 16 is the one drawn */ 4, 2, 4, 0};
        GLuint indirect;
        glGenBuffers(1, &indirect);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirect);
        glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof commands, commands, GL_STATIC_DRAW);
        glDrawArraysIndirect(GL_TRIANGLE_FAN, (const void *)16);
        break;
    }
    case 1: {
        link(vs, GLSL "in vec3 vc; out vec4 frag; void main(){ frag = vec4(vc.gbr, 1.0); }");
        /* count, instances, first index, base vertex, base instance */
        const GLuint commands[] = {6, 2, 0, 4, 0, 6, 1, 6, 0, 0};
        GLuint indirect;
        glGenBuffers(1, &indirect);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirect);
        glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof commands, commands, GL_STATIC_DRAW);
        glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT, 0);
        glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT, (const void *)20);
        break;
    }
    case 2: {
        link(vs, GLSL "in vec3 vc; out vec4 frag; void main(){ frag = vec4(vc.brg, 1.0); }");
        const GLsizei counts[] = {3, 3};
        const void *offsets[] = {(const void *)0, (const void *)6};
        const GLint bases[] = {4, 0};
        glMultiDrawElementsBaseVertex(GL_TRIANGLES, counts, GL_UNSIGNED_SHORT, offsets, 2, bases);
        break;
    }
    case 3:
    case 4: {
        const char *fs = glc_variant == 3
                             ? GLSL "in vec3 vc; layout(location=0, index=0) out vec4 a; layout(location=0, index=1) out vec4 b;\n"
                                    "void main(){ a = vec4(vc, 1.0); b = vec4(vc.brg, 0.5); }"
                             : GLSL "in vec3 vc; out vec4 a; out vec4 b; void main(){ a = vec4(vc, 1.0); b = vec4(vc.brg, 0.5); }";
        GLuint program = glCreateProgram();
        const char *sources[2] = {vs, fs};
        const GLenum types[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
        for (int i = 0; i < 2; ++i) {
            GLuint shader = glCreateShader(types[i]);
            glShaderSource(shader, 1, &sources[i], NULL);
            glCompileShader(shader);
            glAttachShader(program, shader);
        }
        if (glc_variant == 4) {
            glBindFragDataLocationIndexed(program, 0, 0, "a");
            glBindFragDataLocationIndexed(program, 0, 1, "b");
        }
        glLinkProgram(program);
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) glc_fail("dual source program did not link");
        glUseProgram(program);
        glClearColor(0.6f, 0.3f, 0.9f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR, GL_ONE, GL_SRC1_ALPHA);
        glDrawElements(GL_TRIANGLES, 12, GL_UNSIGNED_SHORT, 0);
        glDisable(GL_BLEND);
        int values[4] = {glGetFragDataIndex(program, "a"), glGetFragDataIndex(program, "b"), glGetFragDataLocation(program, "b"),
                         (int)glGetError()};
        glEnable(GL_SCISSOR_TEST);
        for (int i = 0; i < 4; ++i) {
            glScissor(i * 8, 0, 8, 8);
            glClearColor((float)(values[i] & 0xff) / 255.0f, values[i] < 0 ? 1.0f : 0.0f, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);
        break;
    }
    }
}

static const char *const shader_names[] = {"subroutines", "subroutine_array", "fp64", "integer_packed_attribs",
                                           "attrib_queries", "subroutine_unnamed_parameters"};
GLC_CASE_VARIANTS(gl4_shader_features, shader_names, .profile = GLC_CORE, .tolerance = 1)
{
    glClearColor(0.1f, 0.1f, 0.1f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    switch (glc_variant) {
    case 0:
    case 5: {
        char source[2048];
        snprintf(source, sizeof source,
            GLSL "#extension GL_ARB_shader_subroutine : require\n%s\n"
                 "subroutine(Shade) vec4 stripes(vec2 p) { return vec4(step(0.5, fract(p.x * 4.0)), p.y, 0.2, 1.0); }\n"
                 "subroutine(Shade) vec4 rings(vec2 p) { return vec4(0.2, fract(length(p - 0.5) * 6.0), p.x, 1.0); }\n"
                 "subroutine(Tint) void warm(inout vec4 c) { c.r = min(c.r + 0.3, 1.0); }\n"
                 "subroutine(Tint) void cool(inout vec4 c) { c.b = min(c.b + 0.4, 1.0); }\n"
                 "subroutine uniform Shade shade;\nsubroutine uniform Tint tint;\n"
                 "in vec2 uv; out vec4 frag; void main(){ vec4 c = shade(uv); tint(c); frag = c; }",
                 glc_variant == 0 ? "subroutine vec4 Shade(vec2 p); subroutine void Tint(inout vec4 c);"
                                  : "subroutine vec4 Shade(vec2); subroutine void Tint(inout vec4);");
        GLuint program = fullscreen(source);
        GLint shade = glGetSubroutineUniformLocation(program, GL_FRAGMENT_SHADER, "shade");
        GLint tint = glGetSubroutineUniformLocation(program, GL_FRAGMENT_SHADER, "tint");
        GLuint indices[2];
        indices[shade] = glGetSubroutineIndex(program, GL_FRAGMENT_SHADER, "rings");
        indices[tint] = glGetSubroutineIndex(program, GL_FRAGMENT_SHADER, "cool");
        glUniformSubroutinesuiv(GL_FRAGMENT_SHADER, 2, indices);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 32, 64);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        indices[shade] = glGetSubroutineIndex(program, GL_FRAGMENT_SHADER, "stripes");
        indices[tint] = glGetSubroutineIndex(program, GL_FRAGMENT_SHADER, "warm");
        glUniformSubroutinesuiv(GL_FRAGMENT_SHADER, 2, indices);
        glScissor(32, 0, 32, 64);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glDisable(GL_SCISSOR_TEST);
        GLint counts[4];
        glGetProgramStageiv(program, GL_FRAGMENT_SHADER, GL_ACTIVE_SUBROUTINES, &counts[0]);
        glGetProgramStageiv(program, GL_FRAGMENT_SHADER, GL_ACTIVE_SUBROUTINE_UNIFORMS, &counts[1]);
        glGetProgramStageiv(program, GL_FRAGMENT_SHADER, GL_ACTIVE_SUBROUTINE_UNIFORM_LOCATIONS, &counts[2]);
        GLuint current = 0;
        glGetUniformSubroutineuiv(GL_FRAGMENT_SHADER, shade, &current);
        counts[3] = current == indices[shade];
        glEnable(GL_SCISSOR_TEST);
        for (int i = 0; i < 4; ++i) {
            glScissor(i * 4, 60, 4, 4);
            glClearColor((float)counts[i] / 8.0f, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);
        break;
    }
    case 1: {
        GLuint program = fullscreen(
            GLSL "subroutine float Channel(vec2 p);\n"
                 "subroutine(Channel) float horizontal(vec2 p) { return p.x; }\n"
                 "subroutine(Channel) float vertical(vec2 p) { return p.y; }\n"
                 "subroutine(Channel) float diagonal(vec2 p) { return (p.x + p.y) * 0.5; }\n"
                 "subroutine uniform Channel channels[3];\n"
                 "in vec2 uv; out vec4 frag; void main(){ frag = vec4(channels[0](uv), channels[1](uv), channels[2](uv), 1.0); }");
        GLuint h = glGetSubroutineIndex(program, GL_FRAGMENT_SHADER, "horizontal");
        GLuint v = glGetSubroutineIndex(program, GL_FRAGMENT_SHADER, "vertical");
        GLuint d = glGetSubroutineIndex(program, GL_FRAGMENT_SHADER, "diagonal");
        GLint base = glGetSubroutineUniformLocation(program, GL_FRAGMENT_SHADER, "channels");
        GLuint indices[3];
        indices[base] = d;
        indices[base + 1] = h;
        indices[base + 2] = v;
        glUniformSubroutinesuiv(GL_FRAGMENT_SHADER, 3, indices);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        break;
    }
    case 2: {
        GLuint program = fullscreen(
            GLSL "#extension GL_ARB_gpu_shader_fp64 : enable\n"
                 "uniform double scale; uniform dvec2 offset; in vec2 uv; out vec4 frag;\n"
                 "void main(){ dvec2 p = dvec2(uv) * scale + offset; double r = length(p - dvec2(0.5lf));\n"
                 " frag = vec4(vec2(fract(p)), float(r), 1.0); }");
        glUniform1d(glGetUniformLocation(program, "scale"), 1.75);
        glUniform2d(glGetUniformLocation(program, "offset"), 0.125, -0.25);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        break;
    }
    case 3: {
        /* Constant integer and packed attributes feeding the shader. */
        GLuint program = link(GLSL "layout(location=0) in vec2 p; layout(location=2) in ivec4 k; layout(location=3) in uvec2 u;\n"
                                   "layout(location=4) in vec4 packed; flat out ivec4 vk; flat out uvec2 vu; out vec4 vp;\n"
                                   "void main(){ gl_Position = vec4(p, 0.0, 1.0); vk = k; vu = u; vp = packed; }",
                              GLSL "flat in ivec4 vk; flat in uvec2 vu; in vec4 vp; out vec4 frag;\n"
                                   "void main(){ float x = gl_FragCoord.x;\n"
                                   " frag = x < 21.0 ? vec4(vec4(vk) + vec4(0.0, 0.0, 128.0, 0.0)) / 255.0\n"
                                   "      : x < 42.0 ? vec4(vec2(vu) / 255.0, 0.0, 1.0) : vp; }");
        (void)program;
        static const float quad[] = {-1, -1, 1, -1, 1, 1, -1, 1};
        GLuint vao, vbo;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
        glVertexAttribI4i(2, 100, 60, -40, 7);
        glVertexAttribI2ui(3, 3, 4);
        glVertexAttribP4ui(4, GL_UNSIGNED_INT_2_10_10_10_REV, GL_TRUE, 1023u | 512u << 10 | 256u << 20 | 2u << 30);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        break;
    }
    case 4: {
        int values[24];
        int n = 0;
        GLuint vao, vbo;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, 64, NULL, GL_STATIC_DRAW);
        glVertexAttribIPointer(5, 3, GL_SHORT, 12, (void *)8);
        glEnableVertexAttribArray(5);
        glVertexAttribDivisor(5, 2);
        GLint i = 0;
        static const GLenum pnames[] = {GL_VERTEX_ATTRIB_ARRAY_ENABLED, GL_VERTEX_ATTRIB_ARRAY_SIZE, GL_VERTEX_ATTRIB_ARRAY_STRIDE,
                                        GL_VERTEX_ATTRIB_ARRAY_TYPE, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                                        GL_VERTEX_ATTRIB_ARRAY_INTEGER, GL_VERTEX_ATTRIB_ARRAY_DIVISOR};
        for (size_t k = 0; k < sizeof pnames / sizeof pnames[0]; ++k) {
            glGetVertexAttribiv(5, pnames[k], &i);
            values[n++] = pnames[k] == GL_VERTEX_ATTRIB_ARRAY_TYPE ? i & 0xff : i;
        }
        glGetVertexAttribiv(5, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &i);
        values[n++] = i == (GLint)vbo;
        void *pointer = NULL;
        glGetVertexAttribPointerv(5, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
        values[n++] = (int)(intptr_t)pointer;
        glVertexAttrib4f(6, 0.25f, 0.5f, 2, 3);
        GLfloat f[4];
        glGetVertexAttribfv(6, GL_CURRENT_VERTEX_ATTRIB, f);
        for (int k = 0; k < 4; ++k) values[n++] = (int)(f[k] * 4);
        glVertexAttribI4i(7, -3, 9, 1000, 5);
        GLint iv[4];
        glGetVertexAttribIiv(7, GL_CURRENT_VERTEX_ATTRIB, iv);
        for (int k = 0; k < 4; ++k) values[n++] = iv[k];
        values[n++] = (int)glGetError();
        show_ints(values, n);
        break;
    }
    }
}

static const char *const state_names[] = {"conditional_render", "point_parameters", "misc_queries"};
GLC_CASE_VARIANTS(gl4_state, state_names, .profile = GLC_CORE)
{
    glClearColor(0.1f, 0.1f, 0.1f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    switch (glc_variant) {
    case 0: {
        GLuint program = fullscreen(GLSL "uniform vec4 c; out vec4 frag; void main(){ frag = c; }");
        GLint c = glGetUniformLocation(program, "c");
        GLuint queries[2];
        glGenQueries(2, queries);
        /* Query 0: an off-screen quad (no samples); query 1: visible. */
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 0, 0);
        glBeginQuery(GL_SAMPLES_PASSED, queries[0]);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glEndQuery(GL_SAMPLES_PASSED);
        glScissor(0, 0, 8, 8);
        glBeginQuery(GL_ANY_SAMPLES_PASSED, queries[1]);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glEndQuery(GL_ANY_SAMPLES_PASSED);
        glScissor(0, 0, 32, 64);
        glUniform4f(c, 1, 0, 0, 1);
        glBeginConditionalRender(queries[0], GL_QUERY_WAIT);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glEndConditionalRender();
        glScissor(32, 0, 32, 64);
        glUniform4f(c, 0, 1, 0, 1);
        glBeginConditionalRender(queries[1], GL_QUERY_BY_REGION_WAIT);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glEndConditionalRender();
        glDisable(GL_SCISSOR_TEST);
        break;
    }
    case 1: {
        int values[16];
        int n = 0;
        GLfloat f[3];
        glPointParameterf(GL_POINT_FADE_THRESHOLD_SIZE, 2.5f);
        glPointParameteri(GL_POINT_SPRITE_COORD_ORIGIN, GL_LOWER_LEFT);
        glGetFloatv(GL_POINT_FADE_THRESHOLD_SIZE, f);
        values[n++] = (int)(f[0] * 10);
        GLint i;
        glGetIntegerv(GL_POINT_SPRITE_COORD_ORIGIN, &i);
        values[n++] = i & 0xff;
        values[n++] = (int)glGetError();
        glPointParameterf(GL_POINT_FADE_THRESHOLD_SIZE, -1);
        values[n++] = (int)glGetError();
        show_ints(values, n);
        break;
    }
    case 2: {
        int values[32];
        int n = 0;
        GLint i = -1;
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &i);
        values[n++] = i;
        glGetIntegerv(GL_NUM_SHADER_BINARY_FORMATS, &i);
        values[n++] = i;
        GLint range[2], precision;
        glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range, &precision);
        values[n++] = range[0];
        values[n++] = range[1];
        values[n++] = precision;
        GLint count = 0;
        glGetInternalformativ(GL_RENDERBUFFER, GL_RGBA8, GL_NUM_SAMPLE_COUNTS, 1, &count);
        values[n++] = count;
        GLint samples[8] = {0};
        glGetInternalformativ(GL_RENDERBUFFER, GL_RGBA8, GL_SAMPLES, 8, samples);
        for (int k = 0; k < 4; ++k) values[n++] = samples[k];
        GLint64 wide = 0;
        glGetInteger64v(GL_MAX_SERVER_WAIT_TIMEOUT, &wide);
        values[n++] = wide > 0;
        glGetIntegerv(GL_MAX_VERTEX_STREAMS, &i);
        values[n++] = i;
        glGetIntegerv(GL_MAX_SUBROUTINES, &i);
        values[n++] = i >> 4;
        glGetIntegerv(GL_MAX_SUBROUTINE_UNIFORM_LOCATIONS, &i);
        values[n++] = i >> 4;
        glGetIntegerv(GL_MAX_DUAL_SOURCE_DRAW_BUFFERS, &i);
        values[n++] = i;
        values[n++] = (int)glGetError();
        show_ints(values, n);
        break;
    }
    }
}

static const char *const readback_names[] = {"get_tex_image", "copy_tex_sub_image_3d", "compressed_array"};
GLC_CASE_VARIANTS(gl4_texture_transfers, readback_names, .profile = GLC_CORE)
{
    glClearColor(0.1f, 0.1f, 0.1f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char *pixels = malloc(16 * 16 * 4 * 4);
    GLuint texture;
    glGenTextures(1, &texture);
    switch (glc_variant) {
    case 0: {
        /* Levels, layers, depth slices and cube faces read back, then
           shown by scissored clears of their bytes. */
        int values[64];
        int n = 0;
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexStorage2D(GL_TEXTURE_2D, 2, GL_RGBA8, 8, 8);
        fill_rgba8(pixels, 4, 4, 1, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 1, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        unsigned char back[16 * 16 * 4 * 4];
        glGetTexImage(GL_TEXTURE_2D, 1, GL_RGBA, GL_UNSIGNED_BYTE, back);
        for (int i = 0; i < 16; ++i) values[n++] = back[i * 4 + (i % 3)];
        GLuint array;
        glGenTextures(1, &array);
        glBindTexture(GL_TEXTURE_2D_ARRAY, array);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 2, 2, 3, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        for (int layer = 0; layer < 3; ++layer) {
            fill_rgba8(pixels, 2, 2, layer, 0);
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, 2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        }
        glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_BGRA, GL_UNSIGNED_BYTE, back);
        for (int i = 0; i < 12; ++i) values[n++] = back[i * 4 + (i % 4)];
        GLuint cube;
        glGenTextures(1, &cube);
        glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
        for (int face = 0; face < 6; ++face) {
            fill_rgba8(pixels, 2, 2, face, 0);
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        }
        glGetTexImage(GL_TEXTURE_CUBE_MAP_NEGATIVE_Y, 0, GL_RGB, GL_FLOAT, back);
        const float *f = (const float *)back;
        for (int i = 0; i < 6; ++i) values[n++] = (int)(f[i * 2] * 255);
        values[n++] = (int)glGetError();
        free(pixels);
        show_ints(values, n);
        return;
    }
    case 1: {
        /* Copy framebuffer regions into an array layer and a 3D slice,
           then sample them. */
        glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 16, 16, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        GLuint volume;
        glGenTextures(1, &volume);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_3D, volume);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 16, 16, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glActiveTexture(GL_TEXTURE0);
        /* Source pattern in the framebuffer. */
        glEnable(GL_SCISSOR_TEST);
        for (int i = 0; i < 16; ++i) {
            glScissor((i % 4) * 4, (i / 4) * 4, 4, 4);
            glClearColor((float)(i % 4) / 3.0f, (float)(i / 4) / 3.0f, (float)(i & 1), 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);
        glCopyTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, 0, 0, 16, 16);
        glActiveTexture(GL_TEXTURE1);
        glCopyTexSubImage3D(GL_TEXTURE_3D, 0, 4, 4, 2, 2, 2, 8, 8);
        glActiveTexture(GL_TEXTURE0);
        GLuint program = fullscreen(GLSL "uniform sampler2DArray a; uniform sampler3D v; in vec2 uv; out vec4 frag;\n"
                                         "void main(){ frag = uv.x < 0.5 ? texture(a, vec3(uv * vec2(2.0, 1.0), 1.0))\n"
                                         " : texture(v, vec3(fract(uv * vec2(2.0, 1.0)), 0.625)); }");
        glUniform1i(glGetUniformLocation(program, "a"), 0);
        glUniform1i(glGetUniformLocation(program, "v"), 1);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        break;
    }
    case 2: {
        /* DXT1 layers uploaded with glCompressedTexImage3D, read back raw. */
        unsigned char blocks[3 * 4 * 8];
        for (int i = 0; i < (int)sizeof blocks; ++i) blocks[i] = (unsigned char)(i * 29 + 7);
        glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
        glCompressedTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 8, 8, 3, 0, sizeof blocks, blocks);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        unsigned char back[sizeof blocks];
        memset(back, 0, sizeof back);
        glGetCompressedTexImage(GL_TEXTURE_2D_ARRAY, 0, back);
        int same = !memcmp(back, blocks, sizeof blocks);
        GLuint program = fullscreen(GLSL "uniform sampler2DArray a; uniform float ok; in vec2 uv; out vec4 frag;\n"
                                         "void main(){ frag = texture(a, vec3(uv, floor(uv.x * 3.0))) * vec4(1.0, 1.0, ok, 1.0); }");
        glUniform1i(glGetUniformLocation(program, "a"), 0);
        glUniform1f(glGetUniformLocation(program, "ok"), (float)same);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        break;
    }
    }
    free(pixels);
}

static GLuint link_geometry(const char *vs, const char *gs, const char *fs)
{
    GLuint program = glCreateProgram();
    const char *sources[3] = {vs, gs, fs};
    const GLenum types[3] = {GL_VERTEX_SHADER, GL_GEOMETRY_SHADER, GL_FRAGMENT_SHADER};
    for (int i = 0; i < 3; ++i) {
        GLuint shader = glCreateShader(types[i]);
        glShaderSource(shader, 1, &sources[i], NULL);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = "";
            glGetShaderInfoLog(shader, sizeof log, NULL, log);
            glc_fail("stage %d: %s", i, log);
        }
        glAttachShader(program, shader);
    }
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) glc_fail("geometry program did not link");
    glUseProgram(program);
    return program;
}

GLC_CASE(gl4_geometry_subroutine_viewport, .profile = GLC_CORE)
{
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    GLuint program = link_geometry(
        GLSL "void main(){ gl_Position = vec4(0, 0, 0, 1); }",
        GLSL "layout(points) in; layout(triangle_strip, max_vertices = 4) out; out vec4 gc;\n"
             "subroutine void Select();\n"
             "subroutine(Select) void left(){ gl_ViewportIndex = 0; gc = vec4(1, 0, 0, 1); }\n"
             "subroutine(Select) void right(){ gl_ViewportIndex = 1; gc = vec4(0, 1, 0, 1); }\n"
             "subroutine uniform Select selected;\n"
             "void main(){ for (int i = 0; i < 4; ++i) { selected();\n"
             "gl_Position = vec4(vec2(i & 1, i >> 1) * 2.0 - 1.0, 0, 1); EmitVertex(); } }",
        GLSL "in vec4 gc; out vec4 frag; void main(){ frag = gc; }");
    glViewportIndexedf(0, 0, 0, 32, 64);
    glViewportIndexedf(1, 32, 0, 32, 64);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    const char *names[] = {"left", "right"};
    for (int i = 0; i < 2; ++i) {
        GLuint index = glGetSubroutineIndex(program, GL_GEOMETRY_SHADER, names[i]);
        if (index == GL_INVALID_INDEX) glc_fail("missing geometry subroutine %s", names[i]);
        glUniformSubroutinesuiv(GL_GEOMETRY_SHADER, 1, &index);
        glDrawArrays(GL_POINTS, 0, 1);
    }
    glViewport(0, 0, 64, 64);
}

static const char *const layered_names[] = {"layer_array", "layer_cube", "viewport_index"};
GLC_CASE_VARIANTS(gl4_layered, layered_names, .profile = GLC_CORE)
{
    GLuint target = target_fbo();
    glClearColor(0.1f, 0.1f, 0.1f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    static const float triangle[] = {-0.8f, -0.8f, 0.8f, -0.6f, 0.0f, 0.9f};
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof triangle, triangle, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    const char *vs = GLSL "layout(location=0) in vec2 p; void main(){ gl_Position = vec4(p, 0.0, 1.0); }";
    if (glc_variant == 2) {
        link_geometry(vs,
                      GLSL "layout(triangles, invocations = 4) in; layout(triangle_strip, max_vertices = 3) out; out vec4 gc;\n"
                           "void main(){ for (int i = 0; i < 3; ++i) { gl_Position = gl_in[i].gl_Position;\n"
                           "  gc = vec4(float(gl_InvocationID) / 3.0, 1.0 - float(gl_InvocationID) / 3.0, float(i) / 2.0, 1.0);\n"
                           "  gl_ViewportIndex = gl_InvocationID; EmitVertex(); } }",
                      GLSL "in vec4 gc; out vec4 frag; void main(){ frag = gc; }");
        for (int i = 0; i < 4; ++i) glViewportIndexedf((GLuint)i, (float)(i % 2) * 32, (float)(i / 2) * 32, 32 - (float)i * 4, 32);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glViewport(0, 0, 64, 64);
        return;
    }
    bool cube = glc_variant == 1;
    int layers = cube ? 6 : 3;
    GLuint texture, fbo;
    glGenTextures(1, &texture);
    GLenum type = cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D_ARRAY;
    glBindTexture(type, texture);
    if (cube)
        for (int f = 0; f < 6; ++f)
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    else
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 32, 32, 3, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(type, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(type, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) glc_fail("layered framebuffer incomplete");
    glViewport(0, 0, 32, 32);
    glClearColor(0, 0, 0.3f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    char gs[512];
    snprintf(gs, sizeof gs,
             GLSL "layout(triangles, invocations = %d) in; layout(triangle_strip, max_vertices = 3) out; out vec4 gc;\n"
                  "void main(){ for (int i = 0; i < 3; ++i) { gl_Position = gl_in[i].gl_Position * vec4(1.0 - 0.12 * float(gl_InvocationID), 1.0, 1.0, 1.0);\n"
                  "  gc = vec4(float(gl_InvocationID) / %d.0, float(i) / 2.0, 1.0 - float(gl_InvocationID) / %d.0, 1.0);\n"
                  "  gl_Layer = gl_InvocationID; EmitVertex(); } }",
             layers, layers - 1, layers - 1);
    link_geometry(vs, gs, GLSL "in vec4 gc; out vec4 frag; void main(){ frag = gc; }");
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, 64, 64);
    GLuint program = fullscreen(cube ? GLSL "uniform samplerCube t; in vec2 uv; out vec4 frag;\n"
                                            "void main(){ int f = int(floor(uv.x * 3.0)) + 3 * int(floor(uv.y * 2.0));\n"
                                            " vec2 c = fract(uv * vec2(3.0, 2.0)) * 2.0 - 1.0; vec3 d;\n"
                                            " if (f == 0) d = vec3(1.0, -c.y, -c.x); else if (f == 1) d = vec3(-1.0, -c.y, c.x);\n"
                                            " else if (f == 2) d = vec3(c.x, 1.0, c.y); else if (f == 3) d = vec3(c.x, -1.0, -c.y);\n"
                                            " else if (f == 4) d = vec3(c.x, -c.y, 1.0); else d = vec3(-c.x, -c.y, -1.0);\n"
                                            " frag = texture(t, d); }"
                                     : GLSL "uniform sampler2DArray t; in vec2 uv; out vec4 frag;\n"
                                            "void main(){ frag = texture(t, vec3(fract(uv * vec2(3.0, 1.0)), floor(uv.x * 3.0))); }");
    glUniform1i(glGetUniformLocation(program, "t"), 0);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

/* Unnormalized byte and short arrays feeding float inputs (skinning
   indices, packed positions). */
GLC_CASE(gl4_integer_arrays_to_float, .profile = GLC_CORE)
{
    glClearColor(0.1f, 0.1f, 0.1f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    link(GLSL "layout(location=0) in vec2 p; layout(location=1) in vec4 idx; layout(location=2) in vec2 off;\n"
              "out vec4 c; void main(){ gl_Position = vec4(p * 0.8 + off / 1000.0, 0.0, 1.0); c = idx / 255.0; }",
         GLSL "in vec4 c; out vec4 frag; void main(){ frag = c; }");
    struct vertex { float p[2]; GLubyte idx[4]; GLshort off[2]; };
    static const struct vertex vertices[] = {
        {{-1, -1}, {255, 0, 0, 255}, {-100, 50}}, {{1, -1}, {0, 200, 30, 255}, {100, -50}},
        {{1, 1}, {10, 20, 250, 255}, {-30, 0}}, {{-1, 1}, {128, 128, 0, 255}, {0, 120}}};
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
    for (GLuint i = 0; i < 3; ++i) glEnableVertexAttribArray(i);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (void *)0);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(struct vertex), (void *)8);
    glVertexAttribPointer(2, 2, GL_SHORT, GL_FALSE, sizeof(struct vertex), (void *)12);
    static const GLushort indices[] = {0, 1, 2, 0, 2, 3};
    GLuint ibo;
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, indices, GL_STATIC_DRAW);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
}

/* ---- GLSL 4.x built-ins --------------------------------------------------- */

/* A 16x16 mipmapped RGBA8 pattern on unit 0. */
static GLuint builtin_texture(void)
{
    GLuint texture;
    glGenTextures(1, &texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    for (int level = 0, size = 16; size >= 1; ++level, size /= 2) {
        unsigned char pixels[16 * 16 * 4];
        for (int i = 0; i < size * size; ++i) {
            pixels[i * 4 + 0] = (unsigned char)((i * 37 + level * 50) & 255);
            pixels[i * 4 + 1] = (unsigned char)((i * 91 + 30) & 255);
            pixels[i * 4 + 2] = (unsigned char)((i * 13 + level * 90) & 255);
            pixels[i * 4 + 3] = (unsigned char)(255 - ((i * 7) & 127));
        }
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return texture;
}

static const char *const builtin_names[] = {"gather", "query_lod", "bitfield", "packing", "derivatives",
                                            "fetch_offsets", "integer_math", "matrix_funcs", "frag_depth"};
GLC_CASE_VARIANTS(gl4_glsl_builtins, builtin_names, .profile = GLC_CORE, .tolerance = 1)
{
    static const char *const shaders[] = {
        /* gather: components 0..3 and an offset gather, by quadrant */
        GLSL "uniform sampler2D t; in vec2 uv; out vec4 frag;\n"
             "void main(){ int q = int(uv.x * 2.0) + 2 * int(uv.y * 2.0);\n"
             "  vec4 g = q == 0 ? textureGather(t, uv) : q == 1 ? textureGather(t, uv, 1) : q == 2 ? textureGather(t, uv, 3)\n"
             "         : textureGatherOffset(t, uv, ivec2(1, -1), 2);\n"
             "  frag = g; }",
        /* query_lod: the LOD for a coordinate scaled across the target */
        GLSL "uniform sampler2D t; in vec2 uv; out vec4 frag;\n"
             "void main(){ vec2 lod = textureQueryLod(t, uv * mix(0.1, 4.0, uv.x));\n"
             "  frag = vec4(lod.x / 4.0, lod.y / 4.0, fract(lod.y), 1.0); }",
        /* bitfield: integer bit operations on values from the pixel */
        GLSL "in vec2 uv; out vec4 frag;\n"
             "void main(){ uint v = uint(gl_FragCoord.x) * 2654435761u ^ uint(gl_FragCoord.y) * 40503u;\n"
             "  int s = int(v) >> 3;\n"
             "  uint carry; uint sum = uaddCarry(v, 0xF0000000u, carry);\n"
             "  uint hi, lo; umulExtended(v, 0x9E3779B9u, hi, lo);\n"
             "  int ihi, ilo; imulExtended(s, -12345, ihi, ilo);\n"
             "  frag = vec4(float(bitCount(v)) / 32.0, float(findMSB(v) + 1) / 33.0, float(findLSB(v) + 1) / 33.0,\n"
             "              float(bitfieldExtract(v, 5, 8)) / 255.0);\n"
             "  if (uv.y > 0.5) frag = vec4(float(bitfieldReverse(v) >> 24) / 255.0, float(bitfieldInsert(v, 0x5u, 4, 3) & 255u) / 255.0,\n"
             "                           float((hi ^ lo) & 255u) / 255.0, float(carry) * 0.5 + float(uint(ihi ^ ilo) & 127u) / 255.0);\n"
             "  if (uv.x > 0.5) frag.r = float(findMSB(s) + 1) / 33.0; }",
        /* packing (GLSL 4.00 functions; packSnorm2x16 / packHalf2x16 are 4.20) */
        GLSL "in vec2 uv; out vec4 frag;\n"
             "void main(){ vec2 a = uv * 2.0 - 0.5;\n"
             "  uint u2 = packUnorm2x16(a), u4 = packUnorm4x8(vec4(a, 1.0 - a)), s4 = packSnorm4x8(vec4(a, -a));\n"
             "  vec2 back = unpackUnorm2x16(u2); vec4 back4 = unpackSnorm4x8(s4);\n"
             "  int e; float m = frexp(a.x * 37.0 + 0.1, e);\n"
             "  frag = vec4(float(u2 & 255u) / 255.0, float((u2 >> 16) & 255u) / 255.0, float(u4 >> 24) / 255.0, float(s4 & 255u) / 255.0);\n"
             "  if (uv.y > 0.5) frag = vec4(back.x, back4.y * 0.5 + 0.5, m * 0.5 + 0.5, float(e + 8) / 16.0);\n"
             "  if (uv.x > 0.75) frag.b = ldexp(0.75, -int(uv.y * 4.0)) + float(floatBitsToUint(a.x) >> 24) / 1024.0; }",
        /* derivatives (dFdxFine etc. are GLSL 4.50) */
        GLSL "in vec2 uv; out vec4 frag;\n"
             "void main(){ float f = sin(uv.x * 20.0) * cos(uv.y * 13.0) + uv.x * uv.y * 9.0;\n"
             "  frag = vec4(dFdx(f) * 4.0 + 0.5, dFdy(f) * 4.0 + 0.5, dFdx(uv.x * uv.y) * 64.0, fwidth(f) * 4.0); }",
        /* fetch_offsets: texelFetchOffset, textureOffset, textureLodOffset, textureGradOffset */
        GLSL "uniform sampler2D t; in vec2 uv; out vec4 frag;\n"
             "void main(){ int q = int(uv.x * 2.0) + 2 * int(uv.y * 2.0);\n"
             "  ivec2 p = ivec2(uv * 14.0);\n"
             "  frag = q == 0 ? texelFetchOffset(t, p, 0, ivec2(1, 1)) : q == 1 ? textureOffset(t, uv, ivec2(-2, 3))\n"
             "       : q == 2 ? textureLodOffset(t, uv, 1.0, ivec2(1, 0)) : textureGradOffset(t, uv, vec2(0.1, 0), vec2(0, 0.05), ivec2(0, 2)); }",
        /* integer_math: signed division / modulo, shifts and overflow */
        GLSL "in vec2 uv; out vec4 frag;\n"
             "void main(){ int a = int(gl_FragCoord.x) - 32, b = int(gl_FragCoord.y) / 4 - 8; if (b == 0) b = 3;\n"
             "  uint u = uint(a) * 0x01000193u + uint(b);\n"
             "  frag = vec4(float(a / b + 32) / 64.0, float(a % b + 16) / 32.0, float((u >> 7) & 255u) / 255.0,\n"
             "              float((a << 3) & 255) / 255.0); }",
        /* matrix functions */
        GLSL "in vec2 uv; out vec4 frag;\n"
             "void main(){ mat3 m = mat3(1.0 + uv.x, 0.2, 0.1, 0.3, 2.0 - uv.y, 0.4, uv.x * uv.y, 0.5, 1.5);\n"
             "  mat3 i = inverse(m); mat2 o = outerProduct(uv, uv.yx); mat3 c = matrixCompMult(m, transpose(m));\n"
             "  frag = vec4(i[0][0] * 0.5, determinant(m) / 4.0, o[1][0] * 2.0 + c[2][1], abs(i[2][1]) + 0.1); }",
        /* frag_depth: an explicit depth write, visualised by a depth test.
           Conservative depth qualifiers require GLSL 4.20 or an extension. */
        GLSL "in vec2 uv; out vec4 frag;\n"
             "void main(){ gl_FragDepth = gl_FragCoord.z + uv.x * 0.4 * uv.y; frag = vec4(uv, 0.5, 1.0); }",
    };
    GLuint program = fullscreen(shaders[glc_variant]);
    builtin_texture();
    GLint location = glGetUniformLocation(program, "t");
    if (location >= 0) glUniform1i(location, 0);
    if (glc_variant == 8) {
        glEnable(GL_DEPTH_TEST);
        glClearDepth(0.7);
        glClear(GL_DEPTH_BUFFER_BIT);
        glDepthFunc(GL_LESS);
    }
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisable(GL_DEPTH_TEST);
}

GLC_CASE(gl4_conservative_depth_requires_extension, .profile = GLC_CORE)
{
    const char *source = GLSL
        "layout(depth_greater) out float gl_FragDepth;out vec4 frag;\n"
        "void main(){gl_FragDepth=gl_FragCoord.z;frag=vec4(1);}";
    GLuint shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    glDeleteShader(shader);
    if (compiled) glc_fail("GLSL 4.10 accepted conservative depth without its extension");
    glClearColor(.2f, .6f, .8f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

/* ---- per-sample shading --------------------------------------------------- */

static const char *const sample_names[] = {"sample_id", "sample_position", "interpolate_at", "sample_mask",
                                           "min_sample_shading"};
GLC_CASE_VARIANTS(gl4_sample_shading, sample_names, .profile = GLC_CORE, .tolerance = 2)
{
    static const char *const shaders[] = {
        GLSL "in vec2 uv; out vec4 frag; void main(){ frag = vec4(float(gl_SampleID) / 3.0, uv, 1.0); }",
        GLSL "in vec2 uv; out vec4 frag; void main(){ frag = vec4(gl_SamplePosition, uv.x, 1.0); }",
        GLSL "in vec2 uv; out vec4 frag; void main(){ vec2 a = interpolateAtOffset(uv, vec2(0.3, -0.2));\n"
             "  vec2 b = interpolateAtSample(uv, 2); vec2 c = interpolateAtCentroid(uv);\n"
             "  frag = vec4(fract((a - uv) * 64.0 + 0.5).x, fract((b - uv) * 64.0 + 0.5).y, fract((c - uv) * 64.0 + 0.5).x, 1.0); }",
        GLSL "in vec2 uv; out vec4 frag; void main(){ gl_SampleMask[0] = int(uv.x * 16.0) & 15;\n"
             "  frag = vec4(uv, float(gl_SampleMaskIn[0] & 15) / 15.0, 1.0); }",
        GLSL "in vec2 uv; out vec4 frag; void main(){ frag = vec4(fract(gl_FragCoord.xy * 0.37), uv.y, 1.0); }",
    };
    GLuint target = target_fbo();
    GLuint color, framebuffer;
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, 64, 64);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    glViewport(0, 0, 64, 64);
    glClearColor(0.1f, 0.1f, 0.1f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    fullscreen(shaders[glc_variant]);
    if (glc_variant == 4) {
        glEnable(GL_SAMPLE_SHADING);
        glMinSampleShading(1.0f);
    }
    /* A triangle so edges cover partial samples. */
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_SAMPLE_SHADING);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
    glBlitFramebuffer(0, 0, 64, 64, 0, 0, 64, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
}

/* ---- shadow sampler types -------------------------------------------------- */

/* A 16x16 depth ramp (x), per layer/face offset by `bias`. */
static void depth_image(GLenum target, int layer_or_face, float bias)
{
    float depth[16 * 16];
    for (int i = 0; i < 256; ++i) depth[i] = (float)(i % 16) / 15.0f * 0.8f + bias + (float)(i / 16) * 0.004f;
    if (target == GL_TEXTURE_2D_ARRAY)
        glTexSubImage3D(target, 0, 0, 0, layer_or_face, 16, 16, 1, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
    else if (target == GL_TEXTURE_CUBE_MAP)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)layer_or_face, 0, GL_DEPTH_COMPONENT32F, 16, 16, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, depth);
    else
        glTexImage2D(target, 0, GL_DEPTH_COMPONENT32F, 16, 16, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
}

static void shadow_params(GLenum target)
{
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(target, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
}

static const char *const shadow_names[] = {"proj", "offset_grad", "cube", "array", "rect", "texture_size"};
GLC_CASE_VARIANTS(gl4_shadow_samplers, shadow_names, .profile = GLC_CORE, .tolerance = 2)
{
    static const char *const shaders[] = {
        GLSL "uniform sampler2DShadow t; in vec2 uv; out vec4 frag;\n"
             "void main(){ vec4 p = vec4(uv * 2.0, uv.y * 1.6, 2.0);\n"
             "  frag = vec4(textureProj(t, p), textureProjOffset(t, p, ivec2(2, 0)), textureLod(t, vec3(uv, uv.y), 0.0), 1.0); }",
        GLSL "uniform sampler2DShadow t; in vec2 uv; out vec4 frag;\n"
             "void main(){ vec3 p = vec3(uv, uv.x * 0.9 + 0.05);\n"
             "  frag = vec4(textureOffset(t, p, ivec2(-3, 1)), textureGrad(t, p, vec2(0.02, 0.0), vec2(0.0, 0.02)),\n"
             "              textureGradOffset(t, p, vec2(0.05, 0.0), vec2(0.0, 0.01), ivec2(1, 1)), 1.0); }",
        GLSL "uniform samplerCubeShadow t; in vec2 uv; out vec4 frag;\n"
             "void main(){ vec3 d = normalize(vec3(uv * 2.0 - 1.0, 0.6)); float r = uv.x * 0.8 + 0.1;\n"
             "  frag = vec4(texture(t, vec4(d, r)), texture(t, vec4(d.zxy, r)), texture(t, vec4(-d.yzx, r)), 1.0); }",
        GLSL "uniform sampler2DArrayShadow t; in vec2 uv; out vec4 frag;\n"
             "void main(){ float r = uv.y * 0.9;\n"
             "  frag = vec4(texture(t, vec4(uv, 0.0, r)), texture(t, vec4(uv, 1.0, r)), texture(t, vec4(uv, 2.0, r)), 1.0); }",
        GLSL "uniform sampler2DRectShadow t; in vec2 uv; out vec4 frag;\n"
             "void main(){ frag = vec4(texture(t, vec3(uv * 16.0, uv.y)), textureProj(t, vec4(uv * 32.0, uv.x * 2.0, 2.0)), 0.5, 1.0); }",
        GLSL "uniform sampler2DShadow t; uniform samplerCubeShadow c; uniform sampler2DArrayShadow a; in vec2 uv; out vec4 frag;\n"
             "void main(){ ivec2 s = textureSize(t, 0); ivec2 cs = textureSize(c, 0); ivec3 as = textureSize(a, 0);\n"
             "  frag = vec4(float(s.x) / 32.0, float(cs.y) / 32.0, float(as.z) / 4.0, float(textureSize(t, 1).x) / 32.0); }",
    };
    GLuint program = fullscreen(shaders[glc_variant]);
    GLuint textures[3];
    glGenTextures(3, textures);
    glActiveTexture(GL_TEXTURE0);
    switch (glc_variant) {
    case 0: case 1: case 5:
        glBindTexture(GL_TEXTURE_2D, textures[0]);
        depth_image(GL_TEXTURE_2D, 0, 0.1f);
        {
            float small[64];
            for (int i = 0; i < 64; ++i) small[i] = 0.5f;
            glTexImage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT32F, 8, 8, 0, GL_DEPTH_COMPONENT, GL_FLOAT, small);
        }
        shadow_params(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
        break;
    case 2:
        glBindTexture(GL_TEXTURE_CUBE_MAP, textures[0]);
        for (int f = 0; f < 6; ++f) depth_image(GL_TEXTURE_CUBE_MAP, f, 0.03f * (float)f);
        shadow_params(GL_TEXTURE_CUBE_MAP);
        break;
    case 3:
        glBindTexture(GL_TEXTURE_2D_ARRAY, textures[0]);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT32F, 16, 16, 3, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
        for (int l = 0; l < 3; ++l) depth_image(GL_TEXTURE_2D_ARRAY, l, 0.1f * (float)l);
        shadow_params(GL_TEXTURE_2D_ARRAY);
        break;
    case 4:
        glBindTexture(GL_TEXTURE_RECTANGLE, textures[0]);
        depth_image(GL_TEXTURE_RECTANGLE, 0, 0.05f);
        shadow_params(GL_TEXTURE_RECTANGLE);
        break;
    }
    if (glc_variant == 5) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_CUBE_MAP, textures[1]);
        for (int f = 0; f < 6; ++f) depth_image(GL_TEXTURE_CUBE_MAP, f, 0);
        shadow_params(GL_TEXTURE_CUBE_MAP);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D_ARRAY, textures[2]);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT32F, 16, 16, 3, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
        shadow_params(GL_TEXTURE_2D_ARRAY);
        glUniform1i(glGetUniformLocation(program, "c"), 1);
        glUniform1i(glGetUniformLocation(program, "a"), 2);
        glActiveTexture(GL_TEXTURE0);
    }
    glUniform1i(glGetUniformLocation(program, "t"), 0);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}
