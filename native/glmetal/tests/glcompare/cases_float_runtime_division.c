#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t runtime_division_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}
static const char *const runtime_division_modes[] = {
    "uniform_positive", "uniform_negative", "texture_mixed",
    "precise_uniform_positive", "precise_uniform_negative", "precise_texture_mixed"
};
GLC_CASE_VARIANTS(core_float_runtime_division, runtime_division_modes, .profile = GLC_CORE)
{
    const int texture_mode = glc_variant % 3 == 2;
    const int negative = glc_variant % 3 == 1;
    const float choices[] = {3, 7, 10, 127, 255, 510, 13.25f, 37.125f};
    float denominators[64*64];
    for (int i = 0; i < 4096; ++i)
        denominators[i] = texture_mode ? choices[(i/7)%8] * ((i&1) ? -1 : 1) : negative ? -510 : 510;
    char fragment[1024];
    snprintf(fragment, sizeof fragment,
        "#version 410 core\nuniform float denominator;uniform sampler2D denominators;out vec4 frag;void main(){"
        "ivec2 at=ivec2(gl_FragCoord.xy);float d=%s;float n=float(((at.x+64*at.y)%%511)+1)*(d<0?-1:1);"
        "%s float q=n/d;float reciprocal=n*(1.0/d);frag=vec4(q,reciprocal,d,n);}",
        texture_mode ? "texelFetch(denominators,at,0).r" : "denominator",
        glc_variant >= 3 ? "precise" : "");
    GLuint program = glc_program(
        "#version 410 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2-1,0,1);}", fragment, NULL);
    glUniform1f(glGetUniformLocation(program, "denominator"), negative ? -510 : 510);
    glUniform1i(glGetUniformLocation(program, "denominators"), 0);
    GLint original; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &original);
    GLuint vao, textures[2], framebuffer;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenTextures(2, textures); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, textures[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, 64, 64, 0, GL_RED, GL_FLOAT, denominators);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, textures[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 64, 64, 0, GL_RGBA, GL_FLOAT, NULL);
    glGenFramebuffers(1, &framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[1], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) glc_fail("Runtime division target incomplete");
    glBindTexture(GL_TEXTURE_2D, textures[0]); glDrawArrays(GL_TRIANGLES, 0, 3);
    float pixels[64*64*4]; glReadPixels(0, 0, 64, 64, GL_RGBA, GL_FLOAT, pixels);
    int exact = 0, reciprocal = 0, shader_reciprocal = 0;
    for (int i = 0; i < 4096; ++i) {
        volatile float d = denominators[i];
        volatile float n = (float)((i%511)+1) * (d < 0 ? -1 : 1);
        volatile float inverse = 1.0f/d;
        const float q = n/d, m = n*inverse;
        const uint32_t actual = runtime_division_bits(pixels[4*i]);
        exact += actual == runtime_division_bits(q);
        reciprocal += actual == runtime_division_bits(m);
        shader_reciprocal += actual == runtime_division_bits(pixels[4*i+1]);
        if (runtime_division_bits(pixels[4*i+2]) != runtime_division_bits(d) ||
            runtime_division_bits(pixels[4*i+3]) != runtime_division_bits(n))
            glc_fail("Runtime division operand mismatch at %d", i);
    }
    fprintf(stderr, "Runtime division %s: exact=%d reciprocal=%d shader_reciprocal=%d of4096\n",
            runtime_division_modes[glc_variant], exact, reciprocal, shader_reciprocal);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)original);
    glBlitFramebuffer(0, 0, 64, 64, 0, 0, 64, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)original);
    GLenum error = glGetError(); if (error) glc_fail("Runtime division GL error %x", error);
    glDeleteFramebuffers(1, &framebuffer); glDeleteTextures(2, textures);
    glDeleteVertexArrays(1, &vao); glDeleteProgram(program);
}
