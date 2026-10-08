/* Core profile (Apple: 4.1) features. */
#include "glc_gl_core.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glcompare.h"

#define VS_HEADER "#version 410 core\n"

GLC_CASE(core_buffer_binding_lifetime, .profile = GLC_CORE)
{
    const GLenum targets[] = {GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER, GL_PIXEL_PACK_BUFFER,
        GL_PIXEL_UNPACK_BUFFER, GL_UNIFORM_BUFFER, GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
        GL_TEXTURE_BUFFER, GL_TRANSFORM_FEEDBACK_BUFFER, GL_DRAW_INDIRECT_BUFFER};
    const GLenum queries[] = {GL_ARRAY_BUFFER_BINDING, GL_ELEMENT_ARRAY_BUFFER_BINDING, GL_PIXEL_PACK_BUFFER_BINDING,
        GL_PIXEL_UNPACK_BUFFER_BINDING, GL_UNIFORM_BUFFER_BINDING, GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
        GL_TEXTURE_BUFFER, GL_TRANSFORM_FEEDBACK_BUFFER_BINDING, GL_DRAW_INDIRECT_BUFFER_BINDING};
    GLuint vao, buffer;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &buffer);
    for (size_t i = 0; i < sizeof targets / sizeof *targets; ++i) {
        glBindBuffer(targets[i], buffer);
        glBindBuffer(targets[i], 0x7fffffffu);
        if (glGetError() != GL_INVALID_OPERATION) glc_fail("core accepted an ungenerated buffer");
        GLint binding;
        glGetIntegerv(queries[i], &binding);
        if ((GLuint)binding != buffer) glc_fail("invalid bind changed target 0x%x", targets[i]);
    }
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, buffer);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
    glDeleteBuffers(1, &buffer);
    for (size_t i = 0; i < sizeof targets / sizeof *targets; ++i) {
        GLint binding = -1;
        glGetIntegerv(queries[i], &binding);
        if (binding) glc_fail("deleted buffer remains on target 0x%x", targets[i]);
    }
    GLint binding = -1;
    glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, 0, &binding);
    if (binding) glc_fail("deleted uniform buffer remains on index 0");
    glGetIntegeri_v(GL_TRANSFORM_FEEDBACK_BUFFER_BINDING, 0, &binding);
    if (binding) glc_fail("deleted feedback buffer remains on index 0");
    glDeleteVertexArrays(1, &vao);
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLC_CASE(core_feedback_object_lifetime, .profile = GLC_CORE)
{
    GLuint object;
    glGenTransformFeedbacks(1, &object);
    if (glIsTransformFeedback(object)) glc_fail("unbound generated name is a feedback object");
    glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, object);
    if (!glIsTransformFeedback(object)) glc_fail("bound name is not a feedback object");
    glDeleteTransformFeedbacks(1, &object);
    if (glIsTransformFeedback(object)) glc_fail("deleted name is a feedback object");
    GLint binding = -1;
    glGetIntegerv(GL_TRANSFORM_FEEDBACK_BINDING, &binding);
    if (binding) glc_fail("deleted feedback object remains bound");
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

static unsigned build(const char *vs, const char *fs)
{
    static const char *const attributes[] = {"position", "color", "texcoord", NULL};
    return glc_program(vs, fs, attributes);
}

/* position (xy), color (rgba), texcoord (st) */
static const float quad_vertices[] = {
    -0.9f, -0.9f, 1, 0, 0, 1, 0, 0,
    0.9f, -0.85f, 0, 1, 0, 0.8f, 1, 0,
    0.85f, 0.9f, 0, 0, 1, 0.6f, 1, 1,
    -0.85f, 0.85f, 1, 1, 0, 0.4f, 0, 1,
};

static void bind_quad(void)
{
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad_vertices, quad_vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 32, (void *)0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 32, (void *)8);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void *)24);
}

static const char *const passthrough_vs =
    VS_HEADER "in vec2 position; in vec4 color; in vec2 texcoord; out vec4 v_color; out vec2 v_uv;\n"
    "void main(){ gl_Position = vec4(position, 0.0, 1.0); v_color = color; v_uv = texcoord; }";

GLC_CASE(core_shader_macro_parentheses, .profile = GLC_CORE)
{
    const char *condition = "#version 330\n#define OPEN (( (4) )\n#define INDIRECT OPEN\n"
        "#if INDIRECT) >= 4\n#define VALUE 1.0\n#else\n#define VALUE 0.0\n#endif\n";
    char vs[1024], fs[1024];
    snprintf(vs, sizeof vs, "%sin vec2 position; out float green;\n"
        "void main(){ gl_Position=vec4(position,0,1); green=VALUE; }", condition);
    snprintf(fs, sizeof fs, "%sin float green; out vec4 frag;\n"
        "void main(){ frag=vec4(0,green*VALUE,0,1); }", condition);
    build(vs, fs);
    bind_quad();
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

GLC_CASE(core_shader_subroutine_signatures, .profile = GLC_CORE)
{
    const char *declarations[] = {
        "subroutine vec4 T(in vec3 p);",
        "subroutine float T(in vec3 p);",
        "subroutine vec4 T(in vec3 p, out vec4 extra);",
        "subroutine vec4 T(in vec2 p);",
    };
    for (size_t i = 0; i < sizeof declarations / sizeof *declarations; ++i) {
        char source[1024];
        snprintf(source, sizeof source, "#version 400\n%s\n"
            "subroutine(T) vec4 implementation(vec3 other) { return vec4(other,1); }\n"
            "subroutine uniform T selected;\n"
            "void main(){ gl_Position=selected(vec3(0)); }", declarations[i]);
        GLuint shader = glCreateShader(GL_VERTEX_SHADER);
        const char *text = source;
        glShaderSource(shader, 1, &text, NULL);
        glCompileShader(shader);
        GLint compiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!!compiled != (i == 0)) {
            char log[1024] = {0};
            glGetShaderInfoLog(shader, sizeof log, NULL, log);
            glc_fail("subroutine signature %zu compiled=%d: %s", i, compiled, log);
        }
        glDeleteShader(shader);
    }
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

static GLuint texture_rgba8(GLenum target, int size, unsigned seed)
{
    unsigned char *pixels = malloc((size_t)size * size * 4);
    glc_pattern_rgba8(pixels, size, size, seed);
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(target, texture);
    glTexImage2D(target, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    free(pixels);
    return texture;
}

static const char *const basic_versions[] = {"150", "330", "410"};
GLC_CASE_VARIANTS(core_basic_quad, basic_versions, .profile = GLC_CORE)
{
    static const char *const vs[] = {
        "#version 150\nin vec2 position; in vec4 color; out vec4 v;\nvoid main(){ gl_Position = vec4(position, 0.0, 1.0); v = color; }",
        "#version 330 core\nlayout(location = 0) in vec2 position; layout(location = 1) in vec4 color; out vec4 v;\n"
        "void main(){ gl_Position = vec4(position, 0.0, 1.0); v = color; }",
        VS_HEADER "layout(location = 0) in vec2 position; layout(location = 1) in vec4 color; out vec4 v;\n"
        "void main(){ gl_Position = vec4(position, 0.0, 1.0); v = color; }",
    };
    static const char *const fs[] = {
        "#version 150\nin vec4 v; out vec4 frag;\nvoid main(){ frag = v; }",
        "#version 330 core\nin vec4 v; layout(location = 0) out vec4 frag;\nvoid main(){ frag = v; }",
        VS_HEADER "in vec4 v; layout(location = 0) out vec4 frag;\nvoid main(){ frag = v.gbra; }",
    };
    build(vs[glc_variant], fs[glc_variant]);
    bind_quad();
    glClearColor(0.1f, 0.1f, 0.1f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

GLC_CASE(core_uniform_buffer, .profile = GLC_CORE)
{
    GLuint program = build(passthrough_vs,
                           VS_HEADER "layout(std140) uniform Params { vec4 tint; mat2 rotate; float scale[3]; };\n"
                           "in vec4 v_color; in vec2 v_uv; out vec4 frag;\n"
                           "void main(){ vec2 uv = rotate * (v_uv - 0.5); frag = vec4(v_color.rgb * tint.rgb + vec3(uv * scale[2], scale[1]), 1.0); }");
    float data[4 + 8 + 12] = {0.8f, 0.4f, 1.0f, 1, /* mat2: columns padded to vec4 */ 0.7f, 0.7f, 0, 0, -0.7f, 0.7f, 0, 0,
                              0.25f, 0, 0, 0, 0.1f, 0, 0, 0, 0.9f, 0, 0, 0};
    GLuint ubo;
    glGenBuffers(1, &ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof data, data, GL_STATIC_DRAW);
    glUniformBlockBinding(program, glGetUniformBlockIndex(program, "Params"), 2);
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, ubo);
    bind_quad();
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

GLC_CASE(core_instancing_divisor, .profile = GLC_CORE)
{
    build(VS_HEADER "layout(location=0) in vec2 position; layout(location=1) in vec4 color; layout(location=3) in vec2 offset;\n"
          "out vec4 v; void main(){ gl_Position = vec4(position * 0.25 + offset, 0.0, 1.0); v = color * (0.5 + 0.1 * float(gl_InstanceID)); }",
          VS_HEADER "in vec4 v; out vec4 frag; void main(){ frag = v; }");
    bind_quad();
    const float offsets[] = {-0.6f, -0.6f, 0.6f, -0.6f, -0.6f, 0.6f, 0.6f, 0.6f, 0, 0};
    GLuint buffer;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof offsets, offsets, GL_STATIC_DRAW);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glVertexAttribDivisor(3, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArraysInstanced(GL_TRIANGLE_FAN, 0, 4, 5);
}

GLC_CASE(core_draw_elements_base_vertex_restart, .profile = GLC_CORE)
{
    build(VS_HEADER "layout(location=0) in vec2 position; layout(location=1) in vec4 color; out vec4 v;\n"
          "void main(){ gl_Position = vec4(position, 0.0, 1.0); v = color; }",
          VS_HEADER "in vec4 v; out vec4 frag; void main(){ frag = v; }");
    float vertices[12 * 6];
    for (int i = 0; i < 12; ++i) {
        float a = (float)i / 12 * 6.2831853f;
        float r = (i & 1) ? 0.9f : 0.5f;
        float *v = vertices + i * 6;
        v[0] = r * __builtin_cosf(a); v[1] = r * __builtin_sinf(a);
        v[2] = (float)(i % 3) / 2; v[3] = (float)(i % 4) / 3; v[4] = 1 - (float)i / 11; v[5] = 1;
    }
    const unsigned short indices[] = {0, 1, 2, 3, 0xffff, 4, 5, 6, 7, 0xffff, 8, 9, 10};
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
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 24, (void *)0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 24, (void *)8);
    glEnable(GL_PRIMITIVE_RESTART);
    glPrimitiveRestartIndex(0xffff);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawElementsBaseVertex(GL_TRIANGLE_STRIP, 13, GL_UNSIGNED_SHORT, NULL, 1);
}

GLC_CASE(core_integer_attributes_and_texture, .profile = GLC_CORE)
{
    build(VS_HEADER "layout(location=0) in vec2 position; layout(location=4) in ivec2 cell; flat out ivec2 c;\n"
          "void main(){ gl_Position = vec4(position, 0.0, 1.0); c = cell; }",
          VS_HEADER "uniform usampler2D lut; flat in ivec2 c; out vec4 frag;\n"
          "void main(){ uvec4 t = texelFetch(lut, c & 3, 0); frag = vec4(t) / 255.0; }");
    bind_quad();
    const int cells[] = {0, 0, 3, 1, 2, 3, 1, 2};
    GLuint buffer;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof cells, cells, GL_STATIC_DRAW);
    glEnableVertexAttribArray(4);
    glVertexAttribIPointer(4, 2, GL_INT, 0, NULL);
    unsigned char lut[4 * 4 * 4];
    glc_pattern_rgba8(lut, 4, 4, 41);
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, 4, 4, 0, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, lut);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDrawArrays(GL_TRIANGLES, 1, 3);
}

GLC_CASE(core_texture_array_and_lod, .profile = GLC_CORE, .tolerance = 1)
{
    build(passthrough_vs,
          VS_HEADER "uniform sampler2DArray layers; in vec4 v_color; in vec2 v_uv; out vec4 frag;\n"
          "void main(){ float layer = floor(v_uv.x * 3.99); frag = textureLod(layers, vec3(v_uv * 2.0, layer), v_uv.y * 3.0); }");
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 16, 16, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    for (int layer = 0; layer < 4; ++layer) {
        unsigned char pixels[16 * 16 * 4];
        glc_pattern_rgba8(pixels, 16, 16, (unsigned)(50 + layer));
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, 16, 16, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    bind_quad();
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

static const char *const compare_funcs[] = {"lequal", "greater", "always"};
GLC_CASE_VARIANTS(core_shadow_sampler, compare_funcs, .profile = GLC_CORE, .tolerance = 1)
{
    static const GLenum funcs[] = {GL_LEQUAL, GL_GREATER, GL_ALWAYS};
    build(passthrough_vs,
          VS_HEADER "uniform sampler2DShadow depth; in vec4 v_color; in vec2 v_uv; out vec4 frag;\n"
          "void main(){ float lit = texture(depth, vec3(v_uv, v_uv.x * 0.8 + 0.1)); frag = vec4(v_color.rgb * (0.3 + 0.7 * lit), 1.0); }");
    float depths[16 * 16];
    for (int i = 0; i < 256; ++i) depths[i] = (float)((i * 7) % 16) / 15.0f;
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, 16, 16, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depths);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, (GLint)funcs[glc_variant]);
    bind_quad();
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

GLC_CASE(core_srgb_texture_and_framebuffer, .profile = GLC_CORE, .tolerance = 1)
{
    GLint screen = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &screen);
    /* Render a gradient into an sRGB target with GL_FRAMEBUFFER_SRGB, then
       sample it (decoded) and write the result linearly. */
    GLuint target, framebuffer;
    glGenTextures(1, &target);
    glBindTexture(GL_TEXTURE_2D, target);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    glEnable(GL_FRAMEBUFFER_SRGB);
    build(passthrough_vs, VS_HEADER "in vec4 v_color; in vec2 v_uv; out vec4 frag; void main(){ frag = vec4(v_uv, v_color.b, 1.0); }");
    bind_quad();
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)screen);
    build(passthrough_vs, VS_HEADER "uniform sampler2D t; in vec4 v_color; in vec2 v_uv; out vec4 frag; void main(){ frag = texture(t, v_uv); }");
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

GLC_CASE(core_srgb_framebuffer_disabled, .profile = GLC_CORE, .tolerance = 1)
{
    GLint screen = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &screen);
    /* With GL_FRAMEBUFFER_SRGB disabled (the default) clears and draws into
       an sRGB target store their values unencoded; sampling still decodes.
       Left half disabled, right half enabled, bottom rows a clear. */
    GLuint target, framebuffer;
    glGenTextures(1, &target);
    glBindTexture(GL_TEXTURE_2D, target);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    build(passthrough_vs, VS_HEADER "in vec4 v_color; in vec2 v_uv; out vec4 frag; void main(){ frag = vec4(v_uv, v_color.b, 1.0); }");
    bind_quad();
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 16, 32, 48);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glEnable(GL_FRAMEBUFFER_SRGB);
    glScissor(32, 16, 32, 48);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)screen);
    build(passthrough_vs, VS_HEADER "uniform sampler2D t; in vec4 v_color; in vec2 v_uv; out vec4 frag; void main(){ frag = texture(t, v_uv); }");
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

static const char *const srgb_blit_names[] = {"srgb_to_linear", "linear_to_srgb", "srgb_to_srgb"};
GLC_CASE_VARIANTS(core_srgb_blit, srgb_blit_names, .profile = GLC_CORE, .tolerance = 1)
{
    static const GLenum formats[][2] = {{GL_SRGB8_ALPHA8, GL_RGBA8}, {GL_RGBA8, GL_SRGB8_ALPHA8}, {GL_SRGB8_ALPHA8, GL_SRGB8_ALPHA8}};
    GLint screen = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &screen);
    /* Blits between sRGB and linear textures: left half with
       GL_FRAMEBUFFER_SRGB disabled, right half enabled. */
    GLuint textures[2], framebuffers[2];
    glGenTextures(2, textures);
    glGenFramebuffers(2, framebuffers);
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, (GLint)formats[glc_variant][i], 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[i], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[0]);
    glEnable(GL_FRAMEBUFFER_SRGB);
    build(passthrough_vs, VS_HEADER "in vec4 v_color; in vec2 v_uv; out vec4 frag; void main(){ frag = vec4(v_uv, v_color.b, 1.0); }");
    bind_quad();
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffers[0]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers[1]);
    glBlitFramebuffer(32, 0, 64, 64, 32, 0, 64, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glBlitFramebuffer(0, 0, 32, 64, 0, 0, 32, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)screen);
    glBindTexture(GL_TEXTURE_2D, textures[1]);
    build(passthrough_vs, VS_HEADER "uniform sampler2D t; in vec4 v_color; in vec2 v_uv; out vec4 frag; void main(){ frag = texture(t, v_uv); }");
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

GLC_CASE(core_multiple_render_targets, .profile = GLC_CORE)
{
    GLint screen = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &screen);
    GLuint textures[3], framebuffer;
    glGenTextures(3, textures);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    for (int i = 0; i < 3; ++i) {
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (GLenum)i, GL_TEXTURE_2D, textures[i], 0);
    }
    const GLenum buffers[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    glDrawBuffers(3, buffers);
    build(passthrough_vs,
          VS_HEADER "in vec4 v_color; in vec2 v_uv; layout(location=0) out vec4 a; layout(location=1) out vec4 b; layout(location=2) out vec4 c;\n"
          "void main(){ a = v_color; b = vec4(v_uv, 0.0, 1.0); c = vec4(1.0 - v_color.rgb, 1.0); }");
    bind_quad();
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)screen);
    /* Compose the three targets side by side. */
    GLuint program = build(passthrough_vs,
                           VS_HEADER "uniform sampler2D t0, t1, t2; in vec4 v_color; in vec2 v_uv; out vec4 frag;\n"
                           "void main(){ vec2 p = gl_FragCoord.xy / 64.0; frag = p.x < 0.33 ? texture(t0, p) : p.x < 0.66 ? texture(t1, p) : texture(t2, p); }");
    for (int i = 0; i < 3; ++i) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)i);
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        char name[4] = {'t', (char)('0' + i), 0};
        glUniform1i(glGetUniformLocation(program, name), i);
    }
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

GLC_CASE(core_msaa_resolve, .profile = GLC_CORE, .samples = 4)
{
    build(passthrough_vs, VS_HEADER "in vec4 v_color; in vec2 v_uv; out vec4 frag; void main(){ frag = v_color; }");
    bind_quad();
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

GLC_CASE(core_fragcoord_facing_derivatives, .profile = GLC_CORE, .tolerance = 2)
{
    build(passthrough_vs,
          VS_HEADER "in vec4 v_color; in vec2 v_uv; out vec4 frag;\n"
          "void main(){ vec2 d = vec2(dFdx(v_uv.x), dFdy(v_uv.y)) * 16.0; float f = gl_FrontFacing ? 1.0 : 0.25;\n"
          " frag = vec4(fract(gl_FragCoord.xy / 16.0) * f, d.x + d.y, 1.0); }");
    bind_quad();
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

GLC_CASE(core_sampler_objects, .profile = GLC_CORE, .tolerance = 2)
{
    GLuint program = build(passthrough_vs,
                           VS_HEADER "uniform sampler2D a; uniform sampler2D b; in vec4 v_color; in vec2 v_uv; out vec4 frag;\n"
                           "void main(){ frag = mix(texture(a, v_uv * 3.0 - 1.0), texture(b, v_uv * 3.0 - 1.0), step(0.5, v_uv.x)); }");
    glActiveTexture(GL_TEXTURE0);
    GLuint texture = texture_rgba8(GL_TEXTURE_2D, 8, 43);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, texture);
    GLuint samplers[2];
    glGenSamplers(2, samplers);
    glSamplerParameteri(samplers[0], GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
    glSamplerParameteri(samplers[0], GL_TEXTURE_WRAP_T, GL_REPEAT);
    glSamplerParameteri(samplers[0], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(samplers[0], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(samplers[1], GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(samplers[1], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(samplers[1], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const float border[4] = {0.2f, 1, 0.2f, 1};
    glSamplerParameterfv(samplers[1], GL_TEXTURE_BORDER_COLOR, border);
    glSamplerParameteri(samplers[1], GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindSampler(0, samplers[0]);
    glBindSampler(1, samplers[1]);
    glUniform1i(glGetUniformLocation(program, "a"), 0);
    glUniform1i(glGetUniformLocation(program, "b"), 1);
    bind_quad();
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

GLC_CASE(core_texture_buffer, .profile = GLC_CORE)
{
    build(passthrough_vs,
          VS_HEADER "uniform samplerBuffer table; in vec4 v_color; in vec2 v_uv; out vec4 frag;\n"
          "void main(){ int i = int(v_uv.x * 15.0) + 16 * int(v_uv.y * 3.0); frag = texelFetch(table, i); }");
    float values[64 * 4];
    for (int i = 0; i < 64 * 4; ++i) values[i] = (float)((i * 13) % 64) / 63.0f;
    GLuint buffer, texture;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TEXTURE_BUFFER, buffer);
    glBufferData(GL_TEXTURE_BUFFER, sizeof values, values, GL_STATIC_DRAW);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_BUFFER, texture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, buffer);
    bind_quad();
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

GLC_CASE(core_point_sprites, .profile = GLC_CORE, .outlier_fraction = 0.01)
{
    build(VS_HEADER "layout(location=0) in vec2 position; layout(location=1) in vec4 color; out vec4 v;\n"
          "void main(){ gl_Position = vec4(position, 0.0, 1.0); gl_PointSize = 6.0 + 10.0 * color.r; v = color; }",
          VS_HEADER "in vec4 v; out vec4 frag; void main(){ frag = vec4(gl_PointCoord, v.b, 1.0); }");
    glEnable(GL_PROGRAM_POINT_SIZE);
    bind_quad();
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_POINTS, 0, 4);
}

GLC_CASE(core_clip_distance, .profile = GLC_CORE)
{
    build(VS_HEADER "layout(location=0) in vec2 position; layout(location=1) in vec4 color; out vec4 v;\n"
          "void main(){ gl_Position = vec4(position, 0.0, 1.0); gl_ClipDistance[0] = position.x + position.y * 0.5 + 0.2; v = color; }",
          VS_HEADER "in vec4 v; out vec4 frag; void main(){ frag = v; }");
    glEnable(GL_CLIP_DISTANCE0);
    bind_quad();
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

GLC_CASE(core_blend_per_target_and_depth_clamp, .profile = GLC_CORE, .tolerance = 1)
{
    build(VS_HEADER "layout(location=0) in vec2 position; layout(location=1) in vec4 color; out vec4 v;\n"
          "void main(){ gl_Position = vec4(position, 1.5 - color.r * 3.0, 1.0); v = color; }",
          VS_HEADER "in vec4 v; out vec4 frag; void main(){ frag = v; }");
    glEnable(GL_DEPTH_CLAMP);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunci(0, GL_ONE_MINUS_DST_COLOR, GL_ONE);
    glClearColor(0.2f, 0.3f, 0.4f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    bind_quad();
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

GLC_CASE(core_geometry_shader, .profile = GLC_CORE)
{
    GLuint program = glCreateProgram();
    const char *sources[3] = {
        VS_HEADER "layout(location=0) in vec2 position; layout(location=1) in vec4 color; out vec4 vc;\n"
        "void main(){ gl_Position = vec4(position, 0.0, 1.0); vc = color; }",
        VS_HEADER "layout(triangles) in; layout(triangle_strip, max_vertices = 6) out; in vec4 vc[]; out vec4 gc;\n"
        "void main(){ for (int copy = 0; copy < 2; ++copy) { for (int i = 0; i < 3; ++i) {\n"
        "  gl_Position = gl_in[i].gl_Position * vec4(0.5, 0.5, 1, 1) + vec4(copy == 0 ? -0.4 : 0.4, 0.0, 0.0, 0.0);\n"
        "  gc = copy == 0 ? vc[i] : vc[i].bgra; EmitVertex(); } EndPrimitive(); } }",
        VS_HEADER "in vec4 gc; out vec4 frag; void main(){ frag = gc; }",
    };
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
    glUseProgram(program);
    bind_quad();
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

GLC_CASE(core_transform_feedback, .profile = GLC_CORE)
{
    /* Capture a transformed triangle, then draw from the captured buffer. */
    GLuint capture = glCreateProgram();
    const char *vs = VS_HEADER "layout(location=0) in vec2 position; layout(location=1) in vec4 color; out vec2 moved; out vec4 tinted;\n"
                     "void main(){ moved = position.yx * vec2(0.8, -0.9); tinted = color.gbra; gl_Position = vec4(0.0); }";
    GLuint shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(shader, 1, &vs, NULL);
    glCompileShader(shader);
    glAttachShader(capture, shader);
    const char *varyings[] = {"moved", "tinted"};
    glTransformFeedbackVaryings(capture, 2, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(capture);
    GLint ok = 0;
    glGetProgramiv(capture, GL_LINK_STATUS, &ok);
    if (!ok) glc_fail("transform feedback program did not link");
    glUseProgram(capture);
    bind_quad();
    GLuint output;
    glGenBuffers(1, &output);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, output);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 4 * 6 * sizeof(float), NULL, GL_STATIC_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, output);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, 4);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    build(VS_HEADER "layout(location=0) in vec2 position; layout(location=1) in vec4 color; out vec4 v;\n"
          "void main(){ gl_Position = vec4(position, 0.0, 1.0); v = color; }",
          VS_HEADER "in vec4 v; out vec4 frag; void main(){ frag = v; }");
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, output);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 24, (void *)0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 24, (void *)8);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

GLC_CASE(core_tessellation, .profile = GLC_CORE, .tolerance = 1, .outlier_fraction = 0.01)
{
    GLuint program = glCreateProgram();
    const char *sources[4] = {
        VS_HEADER "layout(location=0) in vec2 position; layout(location=1) in vec4 color; out vec4 vc;\n"
        "void main(){ gl_Position = vec4(position, 0.0, 1.0); vc = color; }",
        VS_HEADER "layout(vertices = 3) out; in vec4 vc[]; out vec4 tc[];\n"
        "void main(){ gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position; tc[gl_InvocationID] = vc[gl_InvocationID];\n"
        " gl_TessLevelOuter[0] = 3.0; gl_TessLevelOuter[1] = 4.0; gl_TessLevelOuter[2] = 5.0; gl_TessLevelInner[0] = 4.0; }",
        VS_HEADER "layout(triangles, equal_spacing, ccw) in; in vec4 tc[]; out vec4 ec;\n"
        "void main(){ vec3 b = gl_TessCoord; vec4 p = b.x * gl_in[0].gl_Position + b.y * gl_in[1].gl_Position + b.z * gl_in[2].gl_Position;\n"
        " p.xy *= 1.0 - 0.2 * b.x * b.y * b.z * 27.0; gl_Position = p; ec = vec4(b, 1.0); }",
        VS_HEADER "in vec4 ec; out vec4 frag; void main(){ frag = ec; }",
    };
    const GLenum types[4] = {GL_VERTEX_SHADER, GL_TESS_CONTROL_SHADER, GL_TESS_EVALUATION_SHADER, GL_FRAGMENT_SHADER};
    for (int i = 0; i < 4; ++i) {
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
    glUseProgram(program);
    bind_quad();
    glPatchParameteri(GL_PATCH_VERTICES, 3);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_PATCHES, 0, 3);
}

GLC_CASE(core_occlusion_query_visualized, .profile = GLC_CORE)
{
    build(passthrough_vs, VS_HEADER "uniform vec4 c; in vec4 v_color; in vec2 v_uv; out vec4 frag; void main(){ frag = c; }");
    GLuint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, (GLint *)&program);
    bind_quad();
    GLuint query;
    glGenQueries(1, &query);
    glClear(GL_COLOR_BUFFER_BIT);
    glBeginQuery(GL_SAMPLES_PASSED, query);
    glUniform4f(glGetUniformLocation(program, "c"), 0.2f, 0.2f, 0.2f, 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glEndQuery(GL_SAMPLES_PASSED);
    GLuint samples = 0;
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, &samples);
    /* Encode the count as a colour so the image carries the result. */
    glUniform4f(glGetUniformLocation(program, "c"), (float)(samples & 0xff) / 255, (float)((samples >> 8) & 0xff) / 255, 1, 1);
    glDrawArrays(GL_TRIANGLES, 1, 3);
}

/* ---- framebuffer blits ----------------------------------------------------- */

static const char *const blit_names[] = {"copy", "nearest_up", "linear_down", "flip_xy", "rgba16f_source", "msaa_source",
                                         "msaa_subrect"};
GLC_CASE_VARIANTS(core_blit, blit_names, .profile = GLC_CORE, .tolerance = 1)
{
    GLint target = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &target);
    bool msaa = glc_variant >= 5;
    GLuint fbo, color;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    if (msaa) {
        glGenRenderbuffers(1, &color);
        glBindRenderbuffer(GL_RENDERBUFFER, color);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, 32, 32);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    } else {
        glGenTextures(1, &color);
        glBindTexture(GL_TEXTURE_2D, color);
        glTexImage2D(GL_TEXTURE_2D, 0, glc_variant == 4 ? GL_RGBA16F : GL_RGBA8, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) glc_fail("source framebuffer incomplete");
    /* Paint a pattern with scissored clears. */
    glEnable(GL_SCISSOR_TEST);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            glScissor(x * 8, y * 8, 8, 8);
            glClearColor((float)x / 3, (float)y / 3, (float)((x + y) & 1), 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)target);
    glClearColor(0.2f, 0.2f, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    switch (glc_variant) {
    case 0: glBlitFramebuffer(0, 0, 32, 32, 8, 8, 40, 40, GL_COLOR_BUFFER_BIT, GL_NEAREST); break;
    case 1: glBlitFramebuffer(4, 4, 20, 20, 0, 0, 64, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST); break;
    case 2: glBlitFramebuffer(0, 0, 32, 32, 10, 10, 26, 26, GL_COLOR_BUFFER_BIT, GL_LINEAR); break;
    case 3: glBlitFramebuffer(0, 0, 32, 32, 60, 60, 4, 4, GL_COLOR_BUFFER_BIT, GL_NEAREST); break;
    case 4: glBlitFramebuffer(0, 0, 32, 32, 0, 0, 64, 48, GL_COLOR_BUFFER_BIT, GL_LINEAR); break;
    case 5: glBlitFramebuffer(0, 0, 32, 32, 16, 16, 48, 48, GL_COLOR_BUFFER_BIT, GL_NEAREST); break;
    case 6: glBlitFramebuffer(8, 0, 24, 16, 30, 2, 46, 18, GL_COLOR_BUFFER_BIT, GL_NEAREST); break;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)target);
}

/* ---- texture swizzle and sRGB decode --------------------------------------- */

static const char *const swizzle_names[] = {"rgba_bgra", "single_channels", "zero_one", "srgb_decode", "srgb_skip_decode"};
GLC_CASE_VARIANTS(core_texture_swizzle_srgb, swizzle_names, .profile = GLC_CORE, .tolerance = 1)
{
    build(passthrough_vs, VS_HEADER "uniform sampler2D t; in vec4 v_color; in vec2 v_uv; out vec4 frag;\n"
                                    "void main(){ frag = texture(t, v_uv); }");
    unsigned char pixels[8 * 8 * 4];
    glc_pattern_rgba8(pixels, 8, 8, 61);
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    bool srgb = glc_variant >= 3;
    glTexImage2D(GL_TEXTURE_2D, 0, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (glc_variant == 0) {
        const GLint swizzle[4] = {GL_BLUE, GL_GREEN, GL_RED, GL_ALPHA};
        glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
    } else if (glc_variant == 1) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_ALPHA);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    } else if (glc_variant == 2) {
        const GLint swizzle[4] = {GL_ZERO, GL_GREEN, GL_ONE, GL_ONE};
        glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
    } else if (glc_variant == 4) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);
    }
    bind_quad();
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

/* ---- transform feedback variants ------------------------------------------ */

/* Shows `count` floats as a strip of scissored clears (each value in
   [0, 1] becomes a grey level; out-of-range values are wrapped). */
static void show_values(const float *values, int count)
{
    glEnable(GL_SCISSOR_TEST);
    for (int i = 0; i < count && i < 64; ++i) {
        float v = values[i] - (float)(int)values[i];
        if (v < 0) v += 1;
        glScissor((i % 8) * 8, (i / 8) * 8, 8, 8);
        glClearColor(v, 1 - v, (float)(i & 1), 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
}

static GLuint feedback_program(const char *vs, const char *const *varyings, int count, GLenum mode)
{
    GLuint program = glCreateProgram();
    GLuint shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(shader, 1, &vs, NULL);
    glCompileShader(shader);
    glAttachShader(program, shader);
    glTransformFeedbackVaryings(program, count, varyings, mode);
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) glc_fail("transform feedback program did not link");
    glUseProgram(program);
    return program;
}

static GLuint feedback_buffer(GLuint index, GLsizeiptr size)
{
    GLuint buffer;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
    float *zero = calloc(1, (size_t)size);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, size, zero, GL_STATIC_READ);
    free(zero);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, index, buffer);
    return buffer;
}

GLC_CASE(core_feedback_skip_multiple_buffers, .profile = GLC_CORE)
{
    bind_quad();
    const char *names[] = {"gl_SkipComponents1", "a", "gl_NextBuffer", "gl_SkipComponents2", "b",
                          "gl_NextBuffer", "gl_SkipComponents3", "c", "gl_NextBuffer", "gl_SkipComponents4", "d"};
    feedback_program(VS_HEADER "out float a, b, c, d; void main(){\n"
                     "a = float(gl_VertexID); b = a + 10.0; c = a + 20.0; d = a + 30.0; gl_Position = vec4(0); }",
                     names, sizeof names / sizeof *names, GL_INTERLEAVED_ATTRIBS);
    GLuint buffers[4];
    float initial[24];
    for (int i = 0; i < 24; ++i) initial[i] = -20.0f - (float)i;
    for (int b = 0; b < 4; ++b) {
        buffers[b] = feedback_buffer(b, sizeof initial);
        glBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof initial, initial);
    }
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, 4);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    for (int b = 0; b < 4; ++b) {
        float actual[24];
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[b]);
        glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof actual, actual);
        for (int i = 0; i < 24; ++i) {
            int stride = b + 2;
            float expected = i < 4 * stride && i % stride == stride - 1 ? (float)(i / stride + b * 10) : initial[i];
            if (actual[i] != expected) glc_fail("feedback buffer %d word %d: %g != %g", b, i, actual[i], expected);
        }
    }
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

static const char *const feedback_names[] = {"separate", "strip_query", "pause_resume", "object_draw", "matrix_int"};
GLC_CASE_VARIANTS(core_feedback, feedback_names, .profile = GLC_CORE)
{
    bind_quad();
    float values[64] = {0};
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (glc_variant == 0) {
        const char *vs = VS_HEADER "layout(location=0) in vec2 position; layout(location=1) in vec4 color; out float a; out vec3 b;\n"
                         "void main(){ a = position.x * 0.5 + 0.5; b = color.rgb * 0.75; gl_Position = vec4(0.0); }";
        const char *names[] = {"a", "b"};
        feedback_program(vs, names, 2, GL_SEPARATE_ATTRIBS);
        GLuint b0 = feedback_buffer(0, 4 * sizeof(float)), b1 = feedback_buffer(1, 12 * sizeof(float));
        glEnable(GL_RASTERIZER_DISCARD);
        glBeginTransformFeedback(GL_POINTS);
        glDrawArrays(GL_POINTS, 0, 4);
        glEndTransformFeedback();
        glDisable(GL_RASTERIZER_DISCARD);
        glBindBuffer(GL_ARRAY_BUFFER, b0);
        glGetBufferSubData(GL_ARRAY_BUFFER, 0, 4 * sizeof(float), values);
        glBindBuffer(GL_ARRAY_BUFFER, b1);
        glGetBufferSubData(GL_ARRAY_BUFFER, 0, 12 * sizeof(float), values + 4);
        show_values(values, 16);
    } else if (glc_variant == 1) {
        const char *vs = VS_HEADER "layout(location=0) in vec2 position; out vec2 p;\n"
                         "void main(){ p = position * 0.5 + 0.5; gl_Position = vec4(position, 0.0, 1.0); }";
        const char *names[] = {"p"};
        feedback_program(vs, names, 1, GL_INTERLEAVED_ATTRIBS);
        GLuint b0 = feedback_buffer(0, 5 * 3 * 2 * sizeof(float)); /* room for 5 triangles; a 4-vertex fan makes 2 */
        GLuint query;
        glGenQueries(1, &query);
        glEnable(GL_RASTERIZER_DISCARD);
        glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, query);
        glBeginTransformFeedback(GL_TRIANGLES);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glEndTransformFeedback();
        glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
        glDisable(GL_RASTERIZER_DISCARD);
        GLuint written = 0;
        glGetQueryObjectuiv(query, GL_QUERY_RESULT, &written);
        glBindBuffer(GL_ARRAY_BUFFER, b0);
        glGetBufferSubData(GL_ARRAY_BUFFER, 0, 12 * sizeof(float), values);
        values[12] = (float)written / 4.0f;
        show_values(values, 13);
    } else if (glc_variant == 2) {
        const char *vs = VS_HEADER "layout(location=0) in vec2 position; out vec2 p;\n"
                         "void main(){ p = position * 0.25 + 0.5; gl_Position = vec4(0.0); }";
        const char *names[] = {"p"};
        feedback_program(vs, names, 1, GL_INTERLEAVED_ATTRIBS);
        GLuint b0 = feedback_buffer(0, 8 * 2 * sizeof(float));
        glEnable(GL_RASTERIZER_DISCARD);
        glBeginTransformFeedback(GL_POINTS);
        glDrawArrays(GL_POINTS, 0, 2);
        glPauseTransformFeedback();
        glDrawArrays(GL_POINTS, 0, 4); /* not captured */
        glResumeTransformFeedback();
        glDrawArrays(GL_POINTS, 2, 2);
        glEndTransformFeedback();
        glDisable(GL_RASTERIZER_DISCARD);
        glBindBuffer(GL_ARRAY_BUFFER, b0);
        glGetBufferSubData(GL_ARRAY_BUFFER, 0, 16 * sizeof(float), values);
        show_values(values, 16);
    } else if (glc_variant == 3) {
        const char *vs = VS_HEADER "layout(location=0) in vec2 position; layout(location=1) in vec4 color; out vec2 p; out vec4 c;\n"
                         "void main(){ p = position.yx * 0.7; c = color.bgra; gl_Position = vec4(0.0); }";
        const char *names[] = {"p", "c"};
        feedback_program(vs, names, 2, GL_INTERLEAVED_ATTRIBS);
        GLuint object;
        glGenTransformFeedbacks(1, &object);
        glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, object);
        GLuint b0 = feedback_buffer(0, 4 * 6 * sizeof(float));
        glEnable(GL_RASTERIZER_DISCARD);
        glBeginTransformFeedback(GL_POINTS);
        glDrawArrays(GL_POINTS, 0, 4);
        glEndTransformFeedback();
        glDisable(GL_RASTERIZER_DISCARD);
        glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, 0);
        build(VS_HEADER "layout(location=0) in vec2 position; layout(location=1) in vec4 color; out vec4 v;\n"
              "void main(){ gl_Position = vec4(position, 0.0, 1.0); v = color; }",
              VS_HEADER "in vec4 v; out vec4 frag; void main(){ frag = v; }");
        GLuint vao;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, b0);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 24, (void *)0);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 24, (void *)8);
        glDrawTransformFeedback(GL_TRIANGLE_FAN, object);
    } else {
        const char *vs = VS_HEADER "layout(location=0) in vec2 position; out mat2 m; flat out ivec2 k;\n"
                         "void main(){ m = mat2(position.x, position.y, 0.25, 0.75); k = ivec2(gl_VertexID, 7);\n"
                         "  gl_Position = vec4(0.0); }";
        const char *names[] = {"m", "k"};
        GLuint program = feedback_program(vs, names, 2, GL_INTERLEAVED_ATTRIBS);
        GLuint b0 = feedback_buffer(0, 4 * 6 * sizeof(float));
        glEnable(GL_RASTERIZER_DISCARD);
        glBeginTransformFeedback(GL_POINTS);
        glDrawArrays(GL_POINTS, 0, 4);
        glEndTransformFeedback();
        glDisable(GL_RASTERIZER_DISCARD);
        glBindBuffer(GL_ARRAY_BUFFER, b0);
        float raw[24];
        glGetBufferSubData(GL_ARRAY_BUFFER, 0, sizeof raw, raw);
        for (int i = 0; i < 24; ++i) {
            if (i % 6 >= 4) {
                int bits;
                memcpy(&bits, &raw[i], 4);
                values[i] = (float)bits / 8.0f;
            } else {
                values[i] = raw[i] * 0.5f + 0.5f;
            }
        }
        show_values(values, 24);
        GLsizei length = 0, size = 0;
        GLenum type = 0;
        char name[16];
        glGetTransformFeedbackVarying(program, 0, sizeof name, &length, &size, &type, name);
        if (type != GL_FLOAT_MAT2 || size != 1 || strcmp(name, "m")) glc_fail("glGetTransformFeedbackVarying: %s %x %d", name, type, size);
    }
}

/* ---- geometry shader variants ---------------------------------------------- */

static GLuint link_stages(const char *vs, const char *gs, const char *fs)
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
    if (!ok) {
        char log[2048] = "";
        glGetProgramInfoLog(program, sizeof log, NULL, log);
        glc_fail("link: %s", log);
    }
    glUseProgram(program);
    return program;
}

static const char *const geometry_names[] = {"point_sprites", "line_strip", "primitive_id_flat", "invocations",
                                             "adjacency", "texture_in_gs"};
GLC_CASE_VARIANTS(core_geometry, geometry_names, .profile = GLC_CORE, .tolerance = 1)
{
    const char *vs = VS_HEADER "layout(location=0) in vec2 position; layout(location=1) in vec4 color; out vec4 vc;\n"
                     "void main(){ gl_Position = vec4(position, 0.0, 1.0); vc = color; }";
    const char *fs = VS_HEADER "in vec4 gc; out vec4 frag; void main(){ frag = gc; }";
    bind_quad();
    glClearColor(0.1f, 0.1f, 0.1f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    switch (glc_variant) {
    case 0: {
        GLuint p = link_stages(vs, VS_HEADER "layout(points) in; layout(triangle_strip, max_vertices = 4) out;\n"
                                   "uniform float size; in vec4 vc[]; out vec4 gc;\n"
                                   "void main(){ for (int i = 0; i < 4; ++i) { vec2 c = vec2(i & 1, i >> 1) * 2.0 - 1.0;\n"
                                   "  gl_Position = gl_in[0].gl_Position + vec4(c * size, 0.0, 0.0); gc = vc[0] * (0.5 + 0.5 * c.x); EmitVertex(); } }",
                           fs);
        glUniform1f(glGetUniformLocation(p, "size"), 0.2f);
        glDrawArrays(GL_POINTS, 0, 4);
        break;
    }
    case 1:
        link_stages(vs, VS_HEADER "layout(triangles) in; layout(line_strip, max_vertices = 4) out; in vec4 vc[]; out vec4 gc;\n"
                        "void main(){ for (int i = 0; i < 4; ++i) { gl_Position = gl_in[i % 3].gl_Position * 0.9; gc = vc[i % 3]; EmitVertex(); } }",
                    fs);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        break;
    case 2:
        link_stages(vs, VS_HEADER "layout(triangles) in; layout(triangle_strip, max_vertices = 3) out; in vec4 vc[]; flat out vec4 gc;\n"
                        "void main(){ for (int i = 0; i < 3; ++i) { gl_Position = gl_in[i].gl_Position;\n"
                        "  gc = vec4(float(gl_PrimitiveIDIn) * 0.5, vc[i].g, float(i) / 2.0, 1.0); EmitVertex(); } }",
                    VS_HEADER "flat in vec4 gc; out vec4 frag; void main(){ frag = gc; }");
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        break;
    case 3:
        link_stages(vs, VS_HEADER "layout(triangles, invocations = 3) in; layout(triangle_strip, max_vertices = 3) out; in vec4 vc[]; out vec4 gc;\n"
                        "void main(){ for (int i = 0; i < 3; ++i) { gl_Position = gl_in[i].gl_Position * vec4(0.3, 0.3, 1.0, 1.0)\n"
                        "  + vec4(float(gl_InvocationID) * 0.6 - 0.6, 0.0, 0.0, 0.0); gc = vc[i]; EmitVertex(); } EndPrimitive(); }",
                    fs);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        break;
    case 4: {
        link_stages(vs, VS_HEADER "layout(triangles_adjacency) in; layout(triangle_strip, max_vertices = 3) out; in vec4 vc[]; out vec4 gc;\n"
                        "void main(){ for (int i = 0; i < 6; i += 2) { gl_Position = gl_in[i].gl_Position; gc = vc[i + 1]; EmitVertex(); } }",
                    fs);
        static const float adjacency[] = {
            -0.8f, -0.8f, 1, 0, 0, 1, 0, 0, 0, -0.9f, 0, 1, 0, 1, 0, 0, 0.8f, -0.8f, 0, 0, 1, 1, 0, 0,
            0.9f, 0.0f, 1, 1, 0, 1, 0, 0, 0.0f, 0.8f, 0, 1, 1, 1, 0, 0, -0.9f, 0.0f, 1, 0, 1, 1, 0, 0,
        };
        GLuint vbo;
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof adjacency, adjacency, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 32, (void *)0);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 32, (void *)8);
        glDrawArrays(GL_TRIANGLES_ADJACENCY, 0, 6);
        break;
    }
    case 5: {
        GLuint p = link_stages(vs, VS_HEADER "layout(triangles) in; layout(triangle_strip, max_vertices = 3) out; uniform sampler2D t;\n"
                                   "in vec4 vc[]; out vec4 gc;\n"
                                   "void main(){ for (int i = 0; i < 3; ++i) { gl_Position = gl_in[i].gl_Position;\n"
                                   "  gc = textureLod(t, gl_in[i].gl_Position.xy * 0.5 + 0.5, 0.0) * vc[i]; EmitVertex(); } }",
                               fs);
        glActiveTexture(GL_TEXTURE2);
        texture_rgba8(GL_TEXTURE_2D, 4, 91);
        glUniform1i(glGetUniformLocation(p, "t"), 2);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        break;
    }
    }
}

static GLuint link_tessellation(const char *vs, const char *tcs, const char *tes, const char *fs)
{
    GLuint program = glCreateProgram();
    const char *sources[4] = {vs, tcs, tes, fs};
    const GLenum types[4] = {GL_VERTEX_SHADER, GL_TESS_CONTROL_SHADER, GL_TESS_EVALUATION_SHADER, GL_FRAGMENT_SHADER};
    for (int i = 0; i < 4; ++i) {
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

/* Tessellation control with per-vertex outputs, levels from uniforms. */
#define TESS_VS \
    VS_HEADER "layout(location=0) in vec2 position; layout(location=1) in vec4 color; out vec4 vc;\n" \
              "void main(){ gl_Position = vec4(position, 0.0, 1.0); vc = color; }"
#define TESS_TCS(n) \
    VS_HEADER "layout(vertices = " #n ") out; uniform vec4 outer; uniform vec2 inner; in vec4 vc[]; out vec4 tc[];\n" \
              "void main(){ gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position; tc[gl_InvocationID] = vc[gl_InvocationID];\n" \
              " gl_TessLevelOuter[0] = outer.x; gl_TessLevelOuter[1] = outer.y; gl_TessLevelOuter[2] = outer.z; gl_TessLevelOuter[3] = outer.w;\n" \
              " gl_TessLevelInner[0] = inner.x; gl_TessLevelInner[1] = inner.y; }"
#define TESS_FS VS_HEADER "in vec4 ec; out vec4 frag; void main(){ frag = ec; }"
#define TESS_QUAD_TES(mode) \
    VS_HEADER "layout(quads, " mode ") in; in vec4 tc[]; out vec4 ec;\n" \
              "void main(){ vec2 uv = gl_TessCoord.xy;\n" \
              " vec4 a = mix(gl_in[0].gl_Position, gl_in[1].gl_Position, uv.x), b = mix(gl_in[3].gl_Position, gl_in[2].gl_Position, uv.x);\n" \
              " vec4 p = mix(a, b, uv.y); p.xy *= 1.0 - 0.3 * uv.x * (1.0 - uv.x) * uv.y * (1.0 - uv.y) * 16.0 * 0.5; gl_Position = p;\n" \
              " ec = mix(mix(tc[0], tc[1], uv.x), mix(tc[3], tc[2], uv.x), uv.y) * vec4(uv, 1.0, 1.0); }"
#define TESS_TRI_TES(mode) \
    VS_HEADER "layout(triangles, " mode ") in; in vec4 tc[]; out vec4 ec;\n" \
              "void main(){ vec3 b = gl_TessCoord; vec4 p = b.x * gl_in[0].gl_Position + b.y * gl_in[1].gl_Position + b.z * gl_in[2].gl_Position;\n" \
              " p.xy *= 1.0 - 0.2 * b.x * b.y * b.z * 27.0; gl_Position = p; ec = vec4(b, 1.0) * (b.x * tc[0] + b.y * tc[1] + b.z * tc[2]); }"

static void tess_levels(GLuint program, float o0, float o1, float o2, float o3, float i0, float i1)
{
    glUniform4f(glGetUniformLocation(program, "outer"), o0, o1, o2, o3);
    glUniform2f(glGetUniformLocation(program, "inner"), i0, i1);
}

static const char *const tessellation_names[] = {"quads_equal",       "quads_wire",       "tri_fractional_odd_wire",
                                                 "tri_fractional_even_wire", "tri_cw_cull", "quads_ccw_cull",
                                                 "patch_varyings",    "texture_in_tes",   "tcs_shared_outputs"};
GLC_CASE_VARIANTS(core_tessellation_modes, tessellation_names, .profile = GLC_CORE, .tolerance = 1,
                  .outlier_fraction = 0.01)
{
    bind_quad();
    glClearColor(0.1f, 0.1f, 0.1f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    GLuint program;
    switch (glc_variant) {
    case 0:
    case 1:
        program = link_tessellation(TESS_VS, TESS_TCS(4), TESS_QUAD_TES("equal_spacing"), TESS_FS);
        tess_levels(program, 2, 3, 4, 5, 3, 6);
        if (glc_variant == 1) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glPatchParameteri(GL_PATCH_VERTICES, 4);
        glDrawArrays(GL_PATCHES, 0, 4);
        break;
    case 2:
    case 3:
        program = glc_variant == 2 ? link_tessellation(TESS_VS, TESS_TCS(3), TESS_TRI_TES("fractional_odd_spacing"), TESS_FS)
                                   : link_tessellation(TESS_VS, TESS_TCS(3), TESS_TRI_TES("fractional_even_spacing"), TESS_FS);
        tess_levels(program, 2.5f, 3.7f, 1.2f, 1, 4.3f, 1);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glPatchParameteri(GL_PATCH_VERTICES, 3);
        glDrawArrays(GL_PATCHES, 0, 3);
        break;
    case 4: {
        /* Two triangle patches of the quad; cw output order with back
           faces culled keeps only what GL considers front-facing. */
        program = link_tessellation(TESS_VS, TESS_TCS(3), TESS_TRI_TES("equal_spacing, cw"), TESS_FS);
        tess_levels(program, 3, 2, 4, 1, 3, 1);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        static const GLubyte indices[] = {0, 1, 2, 0, 2, 3};
        GLuint ibo;
        glGenBuffers(1, &ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, indices, GL_STATIC_DRAW);
        glPatchParameteri(GL_PATCH_VERTICES, 3);
        glDrawElements(GL_PATCHES, 6, GL_UNSIGNED_BYTE, 0);
        break;
    }
    case 5:
        program = link_tessellation(TESS_VS, TESS_TCS(4), TESS_QUAD_TES("equal_spacing, ccw"), TESS_FS);
        tess_levels(program, 4, 4, 4, 4, 4, 4);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glPatchParameteri(GL_PATCH_VERTICES, 4);
        glDrawArrays(GL_PATCHES, 0, 4);
        break;
    case 6: {
        /* Patch outputs, tessellation levels and gl_PrimitiveID read by the
           evaluation stage; two patches from one draw. */
        program = link_tessellation(
            TESS_VS,
            VS_HEADER "layout(vertices = 3) out; in vec4 vc[]; out vec4 tc[]; patch out vec4 pc; patch out float shade;\n"
                      "void main(){ gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position; tc[gl_InvocationID] = vc[gl_InvocationID];\n"
                      " pc = (vc[0] + vc[1] + vc[2]) / 3.0; shade = float(gl_PrimitiveID) * 0.5 + 0.25;\n"
                      " gl_TessLevelOuter[0] = 2.0 + float(gl_PrimitiveID); gl_TessLevelOuter[1] = 3.0; gl_TessLevelOuter[2] = 4.0; gl_TessLevelInner[0] = 3.0; }",
            VS_HEADER "layout(triangles) in; in vec4 tc[]; patch in vec4 pc; patch in float shade; out vec4 ec;\n"
                      "void main(){ vec3 b = gl_TessCoord; gl_Position = b.x * gl_in[0].gl_Position + b.y * gl_in[1].gl_Position + b.z * gl_in[2].gl_Position;\n"
                      " ec = vec4(mix(pc.rgb, tc[0].rgb * b.x + tc[1].rgb * b.y + tc[2].rgb * b.z, shade),\n"
                      "  gl_TessLevelOuter[0] / 8.0 + gl_TessLevelInner[0] / 8.0 + float(gl_PrimitiveID) * 0.1); }",
            TESS_FS);
        glPatchParameteri(GL_PATCH_VERTICES, 3);
        static const GLushort indices[] = {0, 1, 2, 0, 2, 3};
        GLuint ibo;
        glGenBuffers(1, &ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, indices, GL_STATIC_DRAW);
        glDrawElements(GL_PATCHES, 6, GL_UNSIGNED_SHORT, 0);
        break;
    }
    case 7: {
        /* A texture displacing and colouring the evaluation stage's output. */
        program = link_tessellation(
            TESS_VS, TESS_TCS(4),
            VS_HEADER "layout(quads) in; uniform sampler2D tex; uniform float amount; in vec4 tc[]; out vec4 ec;\n"
                      "void main(){ vec2 uv = gl_TessCoord.xy; vec4 t = textureLod(tex, uv, 0.0);\n"
                      " vec4 a = mix(gl_in[0].gl_Position, gl_in[1].gl_Position, uv.x), b = mix(gl_in[3].gl_Position, gl_in[2].gl_Position, uv.x);\n"
                      " gl_Position = mix(a, b, uv.y) + vec4((t.rg - 0.5) * amount, 0.0, 0.0); ec = t; }",
            TESS_FS);
        tess_levels(program, 8, 8, 8, 8, 8, 8);
        GLuint texture = texture_rgba8(GL_TEXTURE_2D, 8, 7);
        (void)texture;
        glUniform1i(glGetUniformLocation(program, "tex"), 0);
        glUniform1f(glGetUniformLocation(program, "amount"), 0.1f);
        glPatchParameteri(GL_PATCH_VERTICES, 4);
        glDrawArrays(GL_PATCHES, 0, 4);
        break;
    }
    case 8:
        /* Two input vertices, four control points computed from each
           other's outputs after a barrier. */
        link_tessellation(
            TESS_VS,
            VS_HEADER "layout(vertices = 4) out; in vec4 vc[]; out vec4 tc[];\n"
                      "void main(){ int i = gl_InvocationID; vec4 lo = gl_in[0].gl_Position, hi = gl_in[1].gl_Position;\n"
                      " float x = (i == 0 || i == 3) ? lo.x : hi.x; float y = i < 2 ? lo.y : hi.y;\n"
                      " gl_out[gl_InvocationID].gl_Position = vec4(x, y, 0.0, 1.0);\n"
                      " tc[gl_InvocationID] = vc[i & 1] * float(i + 1) / 4.0; barrier();\n"
                      " vec4 other = tc[(i + 1) % 4]; barrier(); tc[gl_InvocationID] = vec4(tc[i].rg, other.b, 1.0);\n"
                      " if (i == 0) { gl_TessLevelOuter[0] = 3.0; gl_TessLevelOuter[1] = 5.0; gl_TessLevelOuter[2] = 3.0; gl_TessLevelOuter[3] = 5.0;\n"
                      "  gl_TessLevelInner[0] = 4.0; gl_TessLevelInner[1] = 2.0; } }",
            TESS_QUAD_TES("equal_spacing"), TESS_FS);
        glPatchParameteri(GL_PATCH_VERTICES, 2);
        glDrawArrays(GL_PATCHES, 0, 2);
        break;
    }
}

/* Primitive restart beyond Metal's (strips at the type's maximum index). */
static const char *const restart_names[] = {"uint_index_ffff", "triangle_list", "fan", "line_loop", "fan_base_vertex"};
GLC_CASE_VARIANTS(core_primitive_restart_modes, restart_names, .profile = GLC_CORE)
{
    build(passthrough_vs, VS_HEADER "in vec4 v_color; in vec2 v_uv; out vec4 frag; void main(){ frag = v_color; }");
    /* A 4x4 grid of vertices over the target. */
    float vertices[16 * 8];
    for (int i = 0; i < 16; ++i) {
        float *v = vertices + i * 8;
        v[0] = -0.9f + 0.6f * (float)(i % 4);
        v[1] = -0.9f + 0.6f * (float)(i / 4);
        v[2] = (float)(i % 4) / 3.0f; v[3] = (float)(i / 4) / 3.0f; v[4] = 0.5f; v[5] = 1;
        v[6] = v[7] = 0;
    }
    GLuint vao, vbo, ibo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 32, (void *)0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 32, (void *)8);
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glEnable(GL_PRIMITIVE_RESTART);
    glClear(GL_COLOR_BUFFER_BIT);
    const GLuint R = 0xffff;
    switch (glc_variant) {
    case 0: {
        const GLuint idx[] = {0, 4, 1, 5, 2, 6, R, 8, 12, 9, 13, 10, 14, 11, 15};
        glPrimitiveRestartIndex(R);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
        glDrawElements(GL_TRIANGLE_STRIP, 15, GL_UNSIGNED_INT, 0);
        break;
    }
    case 1: {
        const GLushort idx[] = {0, 1, 5, 0xffff, 2, 3, 0xffff, 6, 7, 11, 9, 10, 14};
        glPrimitiveRestartIndex(0xffff);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
        glDrawElements(GL_TRIANGLES, 13, GL_UNSIGNED_SHORT, 0);
        break;
    }
    case 2: {
        const GLushort idx[] = {5, 0, 1, 2, 6, 7, 99, 10, 13, 14, 15, 11, 7};
        glPrimitiveRestartIndex(99);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
        glDrawElements(GL_TRIANGLE_FAN, 13, GL_UNSIGNED_SHORT, 0);
        break;
    }
    case 3: {
        const GLubyte idx[] = {0, 3, 15, 12, 200, 5, 6, 10, 9};
        glPrimitiveRestartIndex(200);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
        glDrawElements(GL_LINE_LOOP, 9, GL_UNSIGNED_BYTE, 0);
        break;
    }
    case 4: {
        const GLushort idx[] = {1, 0, 4, 5, 6, 0xffff, 6, 5, 9, 10};
        glPrimitiveRestartIndex(0xffff);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
        glDrawElementsBaseVertex(GL_TRIANGLE_FAN, 10, GL_UNSIGNED_SHORT, 0, 1);
        break;
    }
    }
    glDisable(GL_PRIMITIVE_RESTART);
}

GLC_CASE(core_geometry_feedback_emission, .profile = GLC_CORE)
{
    GLuint program = glCreateProgram();
    const char *sources[] = {
        "#version 400\nvoid main(){ gl_Position=vec4(0); }",
        "#version 400\nlayout(points) in; layout(triangle_strip,max_vertices=6) out;\n"
        "void main(){ for(int i=0;i<4;++i){gl_Position=vec4(float(i),0,0,1); EmitVertex();}\n"
        "EndPrimitive(); gl_Position=vec4(99); EmitVertex(); EndPrimitive(); }",
    };
    GLenum stages[] = {GL_VERTEX_SHADER, GL_GEOMETRY_SHADER};
    for (int i=0;i<2;++i) {
        GLuint shader=glCreateShader(stages[i]);
        glShaderSource(shader,1,&sources[i],NULL); glCompileShader(shader);
        glAttachShader(program,shader); glDeleteShader(shader);
    }
    const char *varying="gl_Position";
    glTransformFeedbackVaryings(program,1,&varying,GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(program);
    GLint linked=0; glGetProgramiv(program,GL_LINK_STATUS,&linked);
    if (!linked) { char log[2048]; glGetProgramInfoLog(program,sizeof log,NULL,log); glc_fail("geometry feedback link: %s",log); return; }
    glUseProgram(program);
    GLuint vao,buffer,queries[2];
    glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    glGenBuffers(1,&buffer); glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,buffer);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER,6*4*sizeof(float),NULL,GL_STATIC_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,buffer);
    glGenQueries(2,queries);
    glBeginQuery(GL_PRIMITIVES_GENERATED,queries[0]);
    glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN,queries[1]);
    glEnable(GL_RASTERIZER_DISCARD); glBeginTransformFeedback(GL_TRIANGLES);
    glDrawArrays(GL_POINTS,0,1);
    glEndTransformFeedback(); glDisable(GL_RASTERIZER_DISCARD);
    glEndQuery(GL_PRIMITIVES_GENERATED); glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
    GLuint generated=0,written=0;
    glGetQueryObjectuiv(queries[0],GL_QUERY_RESULT,&generated);
    glGetQueryObjectuiv(queries[1],GL_QUERY_RESULT,&written);
    if(generated!=2 || written!=2) glc_fail("geometry counts %u generated, %u captured",generated,written);
    float values[24]; glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER,0,sizeof values,values);
    const float expected[]={0,1,2,2,1,3};
    for(int i=0;i<6;++i) if(values[4*i]!=expected[i] || values[4*i+3]!=1)
        glc_fail("geometry feedback vertex %d is (%g,%g), expected (%g,1)",i,values[4*i],values[4*i+3],expected[i]);
    glDeleteQueries(2,queries); glDeleteBuffers(1,&buffer); glDeleteVertexArrays(1,&vao); glDeleteProgram(program);
}
