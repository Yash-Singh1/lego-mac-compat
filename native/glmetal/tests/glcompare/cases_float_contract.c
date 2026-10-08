#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *const contract_modes[] = {"default", "precise"};
GLC_CASE_VARIANTS(core_float_contraction, contract_modes, .profile = GLC_CORE)
{
    GLint original;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &original);
    GLuint fb, texture, vao;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 64, 64, 0, GL_RGBA, GL_FLOAT, NULL);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    char fragment[512];
    snprintf(fragment, sizeof fragment,
             "#version 410 core\nuniform vec3 values;out vec4 color;void main(){"
             "%s float result=values.x*values.y;result=result+values.z;color=vec4(result);}",
             glc_variant ? "precise" : "");
    GLuint program = glc_program(
        "#version 410 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
        "gl_Position=vec4(p*2.-1.,0,1);}", fragment, NULL);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glUseProgram(program);
    glUniform3f(glGetUniformLocation(program, "values"), 0x1.0008p0f, 0x1.fffp-1f, -1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    float result[4];
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_FLOAT, result);
    uint32_t bits[4];
    memcpy(bits, result, sizeof bits);
    uint32_t expected = glc_variant ? 0 : 0xb2800000u;
    for (int i = 0; i < 4; ++i)
        if (bits[i] != expected) glc_fail("float contraction %s[%d]: %08x expected %08x",
                                        contract_modes[glc_variant], i, bits[i], expected);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)original);
    glClearColor(.25f, .5f, .75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDeleteProgram(program);
    glDeleteTextures(1, &texture);
    glDeleteFramebuffers(1, &fb);
    glDeleteVertexArrays(1, &vao);
}
