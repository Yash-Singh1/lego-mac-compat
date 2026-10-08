#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdio.h>

static const char *const lod_names[] = {"implicit", "explicit_fragment", "explicit_vertex", "sampler_object", "sampler_array", "gradients", "sampler_function"};
GLC_CASE_VARIANTS(core_texture_lod_bias, lod_names, .profile = GLC_CORE, .tolerance = 1)
{
    const char *vs = "#version 410 core\nout vec2 uv; void main(){\n"
        "vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1); uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0, 1); }";
    const char *vertex_sample = "#version 410 core\nuniform sampler2D tex; uniform float shader_lod; out vec4 color; void main(){\n"
        "vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1); gl_Position = vec4(p * 2.0 - 1.0, 0, 1);"
        "color = textureLod(tex, vec2(0.5), shader_lod); }";
    char fs[1024];
    const char *expression = glc_variant == 6 ? "lookup(tex[which], uv * 8.0)" : glc_variant == 1 ? "textureLod(tex, uv, shader_lod)" :
                             glc_variant == 4 ? "texture(tex[which], uv * 8.0, shader_lod)" :
                             glc_variant == 5 ? "textureGrad(tex, uv, vec2(0.125, 0), vec2(0, 0.125))" :
                                               "texture(tex, uv * 8.0, shader_lod)";
    snprintf(fs, sizeof fs, "#version 410 core\nuniform sampler2D %s; uniform float shader_lod; uniform int which;\n"
             "vec4 lookup(sampler2D s, vec2 coord){ return texture(s, coord, shader_lod); }\n"
             "in vec2 uv; out vec4 frag; void main(){ frag = %s; }", (glc_variant == 4 || glc_variant == 6) ? "tex[2]" : "tex", expression);
    GLuint program = glc_program(glc_variant == 2 ? vertex_sample : vs,
        glc_variant == 2 ? "#version 410 core\nin vec4 color; out vec4 frag; void main(){ frag = color; }" : fs, NULL);
    glUseProgram(program);
    GLuint vao, textures[2], sampler = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenTextures(2, textures);
    for (int t = 0; t < 2; ++t) {
        glActiveTexture(GL_TEXTURE0 + t);
        glBindTexture(GL_TEXTURE_2D, textures[t]);
        for (int level = 0, size = 8; level < 4; ++level, size /= 2) {
            unsigned char pixels[8 * 8 * 4];
            for (int i = 0; i < size * size; ++i) {
                pixels[4 * i] = level == 0 || level == 3 ? 255 : 0;
                pixels[4 * i + 1] = level == 1 || level == 3 ? 255 : 0;
                pixels[4 * i + 2] = level == 2 ? 255 : 0;
                pixels[4 * i + 3] = 255;
            }
            glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    if (glc_variant == 3) {
        glGenSamplers(1, &sampler);
        glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindSampler(0, sampler);
    }
    GLint units[2] = {0, 1};
    glUniform1iv(glGetUniformLocation(program, (glc_variant == 4 || glc_variant == 6) ? "tex[0]" : "tex"), (glc_variant == 4 || glc_variant == 6) ? 2 : 1, units);
    const float biases[] = {-2, -0.5f, 0, 0.5f, 1, 1.5f, 2, 4};
    glEnable(GL_SCISSOR_TEST);
    for (int i = 0; i < 8; ++i) {
        int unit = (glc_variant == 4 || glc_variant == 6) ? i % 2 : 0;
        glActiveTexture(GL_TEXTURE0 + unit);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, biases[i]);
        if (sampler) glSamplerParameterf(sampler, GL_TEXTURE_LOD_BIAS, -biases[i]);
        glUniform1i(glGetUniformLocation(program, "which"), unit);
        glUniform1f(glGetUniformLocation(program, "shader_lod"), 0.5f);
        glScissor(i * 8, 0, 8, 64);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glDisable(GL_SCISSOR_TEST);
}
