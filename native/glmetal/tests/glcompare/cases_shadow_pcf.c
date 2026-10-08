/* Non-square depth24/stencil8 atlas with linear shadow comparisons.
 * Covers rendered and depth-blitted depth atlases with filtered comparison. */
#include "glc_gl_core.h"
#include "glcompare.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const char *const pcf_names[] = {"render", "depth_blit"};
static float pcf_depth(int x, int y)
{
    return y < 8 ? ((x + y * 3) % 4 + 1) / 8.0f : ((x * 3 + y) % 4 + 4) / 8.0f;
}
static void pcf_target(GLuint framebuffer, GLuint texture)
{
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, 8, 16, 0,
                 GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, texture, 0);
    glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        glc_fail("Linear shadow depth/stencil framebuffer incomplete");
}
static float pcf_expected(float u, float v, float reference)
{
    float x = u * 8 - .5f, y = v * 16 - .5f;
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - ix, fy = y - iy, result = 0;
    for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
        int sx = ix + dx, sy = iy + dy;
        if (sx < 0) sx = 0; if (sx > 7) sx = 7;
        if (sy < 0) sy = 0; if (sy > 15) sy = 15;
        float weight = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
        result += (reference <= pcf_depth(sx, sy) ? 1 : 0) * weight;
    }
    return result;
}
GLC_CASE_VARIANTS(core_shadow_pcf, pcf_names, .profile = GLC_CORE, .tolerance = 2)
{
    GLint original; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &original);
    GLuint textures[2], frames[2], vao;
    glGenTextures(2, textures); glGenFramebuffers(2, frames);
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glActiveTexture(GL_TEXTURE0);
    pcf_target(frames[0], textures[0]);
    GLuint depth_program = glc_program(
        "#version 150 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2-1,0,1);}",
        "#version 150 core\nout vec4 unused;void main(){ivec2 p=ivec2(gl_FragCoord.xy);"
        "int k=p.y<8?(p.x+p.y*3)%4+1:(p.x*3+p.y)%4+4;"
        "gl_FragDepth=float(k)/8.;unused=vec4(1);}", NULL);
    glUseProgram(depth_program); glViewport(0, 0, 8, 16);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS); glDepthMask(GL_TRUE);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glUseProgram(0); glDeleteProgram(depth_program);
    if (glc_variant) {
        pcf_target(frames[1], textures[1]);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, frames[0]);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, frames[1]);
        glBlitFramebuffer(0, 0, 8, 16, 0, 0, 8, 16, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)original);
    glBindTexture(GL_TEXTURE_2D, textures[glc_variant ? 1 : 0]);
    GLuint program = glc_program(
        "#version 150 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2-1,0,1);}",
        "#version 150 core\nuniform sampler2DShadow atlas;out vec4 color;void main(){"
        "ivec2 c=ivec2(gl_FragCoord.xy)/8;"
        "float u=(float(c.x)+((c.y&1)==0?.5:0.))/8.;"
        "float v=(c.y<2?3.5:c.y<4?7.5:c.y<6?8.5:12.5)/16.;"
        "if((c.y&1)!=0)v+=.5/16.;"
        "float ref=(c.x<4?.35:.65)+float(c.y%3)*.01;"
        "float q=.5+float(c.x+c.y)*.125;vec3 t=vec3(u,v,ref);"
        "float a=textureLod(atlas,t,0.);"
        "float b=textureProj(atlas,vec4(t*q,q));"
        "color=vec4(a,b,abs(a-b),1);}", NULL);
    glUseProgram(program); glUniform1i(glGetUniformLocation(program, "atlas"), 0);
    glViewport(0, 0, 64, 64); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_DITHER);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    unsigned char pixels[64 * 64 * 4];
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    GLenum error = glGetError();
    if (error) glc_fail("Linear shadow atlas %s GL error %x", pcf_names[glc_variant], error);
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
        float u = (x + (y % 2 ? 0 : .5f)) / 8;
        float v = (y < 2 ? 3.5f : y < 4 ? 7.5f : y < 6 ? 8.5f : 12.5f) / 16;
        if (y % 2) v += .5f / 16;
        float reference = (x < 4 ? .35f : .65f) + (y % 3) * .01f;
        int expected = (int)lroundf(pcf_expected(u, v, reference) * 255);
        const unsigned char *pixel = pixels + ((y * 8 + 4) * 64 + x * 8 + 4) * 4;
        if (abs((int)pixel[0] - expected) > 2 || abs((int)pixel[1] - expected) > 2 || pixel[2] > 2)
            glc_fail("Linear shadow %s cell %d,%d uv %.5f/%.5f ref %.3f lod/proj=%u/%u expected=%d",
                     pcf_names[glc_variant], x, y, u, v, reference, pixel[0], pixel[1], expected);
    }
    glUseProgram(0); glDeleteProgram(program);
    glDeleteFramebuffers(2, frames); glDeleteTextures(2, textures); glDeleteVertexArrays(1, &vao);
}
