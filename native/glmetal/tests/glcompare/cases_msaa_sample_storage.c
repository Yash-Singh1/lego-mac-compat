#include "glc_gl_core.h"
#include "glcompare.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t msaa_storage_bits(float value)
{
    uint32_t bits; memcpy(&bits, &value, sizeof bits); return bits;
}
static const char *const storage_samples[] = {"sample0", "sample1", "sample2", "sample3"};
GLC_CASE_VARIANTS(core_msaa_sample_storage, storage_samples, .profile = GLC_CORE)
{
    GLint original; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &original);
    GLuint textures[2], framebuffers[2], vaos[2], buffer;
    glGenTextures(2, textures); glGenFramebuffers(2, framebuffers); glGenVertexArrays(2, vaos);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, textures[0]);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA8, 64, 64, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[0]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, textures[0], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) glc_fail("MSAA sample target incomplete");
    GLfloat position[2]; glGetMultisamplefv(GL_SAMPLE_POSITION, (GLuint)glc_variant, position);
    GLuint source = glc_program(
        "#version 410 core\nlayout(location=0) in vec2 p;out vec2 uv;void main(){uv=p*.5+.5;gl_Position=vec4(p,0,1);}",
        "#version 410 core\nin vec2 uv;out vec4 color;void main(){color=vec4(uv,gl_FragCoord.xy/64.0);}", NULL);
    static const float triangle[] = {-1,-1, 1,-1, 1,1};
    glBindVertexArray(vaos[0]); glGenBuffers(1, &buffer); glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof triangle, triangle, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glViewport(0, 0, 64, 64); glClearColor(.1f,.1f,.1f,.1f); glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SAMPLE_SHADING); glMinSampleShading(1); glDrawArrays(GL_TRIANGLES, 0, 3); glDisable(GL_SAMPLE_SHADING);
    glBindTexture(GL_TEXTURE_2D, textures[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 64, 64, 0, GL_RGBA, GL_FLOAT, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[1]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[1], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) glc_fail("MSAA sample readback target incomplete");
    GLuint reader = glc_program(
        "#version 410 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2-1,0,1);}",
        "#version 410 core\nuniform sampler2DMS samples;uniform int index;out vec4 color;void main(){color=texelFetch(samples,ivec2(gl_FragCoord.xy),index);}", NULL);
    glUniform1i(glGetUniformLocation(reader, "samples"), 0); glUniform1i(glGetUniformLocation(reader, "index"), glc_variant);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, textures[0]);
    glBindVertexArray(vaos[1]); glDrawArrays(GL_TRIANGLES, 0, 3);
    float pixels[64*64*4]; glReadPixels(0, 0, 64, 64, GL_RGBA, GL_FLOAT, pixels);
    int uv_position_difference = 0, model_difference = 0, decoded_exact = 0, decoded_reciprocal = 0;
    volatile float den = 255, inv = 1.0f/den;
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
        int at = (y*64+x)*4;
        for (int channel = 0; channel < 4; ++channel) {
            float value = pixels[at+channel];
            int byte = (int)lrintf(value*255);
            volatile float numerator = (float)byte;
            decoded_exact += msaa_storage_bits(value) == msaa_storage_bits(numerator/den);
            decoded_reciprocal += msaa_storage_bits(value) == msaa_storage_bits(numerator*inv);
            if (x > y+2) {
                const float coord = ((channel&1) ? y+position[1] : x+position[0])/64;
                const int expected = (int)lrintf(coord*255);
                if (byte != expected) {
                    if (model_difference < 8) fprintf(stderr,"MSAA sample%d stored (%d,%d) c%d=%d expected%d raw%08x\n",glc_variant,x,y,channel,byte,expected,msaa_storage_bits(value));
                    ++model_difference;
                }
            }
        }
        if (x > y+2)
            uv_position_difference += msaa_storage_bits(pixels[at]) != msaa_storage_bits(pixels[at+2]) ||
                                      msaa_storage_bits(pixels[at+1]) != msaa_storage_bits(pixels[at+3]);
    }
    fprintf(stderr,"MSAA sample%d position=(%.9g,%.9g) UV/position mismatch=%d model mismatch=%d decode exact=%d reciprocal=%d of16384\n",
        glc_variant,position[0],position[1],uv_position_difference,model_difference,decoded_exact,decoded_reciprocal);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)original);
    glBlitFramebuffer(0,0,64,64,0,0,64,64,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)original);
    GLenum error=glGetError(); if(error) glc_fail("MSAA sample storage GL error %x",error);
    glDeleteProgram(source); glDeleteProgram(reader); glDeleteBuffers(1,&buffer);
    glDeleteVertexArrays(2,vaos); glDeleteFramebuffers(2,framebuffers); glDeleteTextures(2,textures);
}
