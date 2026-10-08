/* Fixed-point shadow references clamp after projective division. Floating
 * depth references do not clamp. Keep depth-zero and cleared depth-one atlas
 * regions, which distinguish the rules even when interior depths do not.
 * Build: make -j2 glcompare. Run later with --filter shadow_reference.
 */
#include "glc_gl_core.h"
#include "glcompare.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const char *const reference_names[] = {
    "depth24stencil8_nearest", "depth24stencil8_linear",
    "depth32f_nearest", "depth32f_linear"
};
static const GLenum reference_funcs[] = {GL_LEQUAL, GL_GEQUAL, GL_EQUAL, GL_NOTEQUAL};
static const char *const reference_func_names[] = {"lequal", "gequal", "equal", "notequal"};
static const float reference_values[] = {-.25f, 0, .25f, .5f, .75f, 1, 1.25f, 1.0009765625f};

static float reference_compare(GLenum function, float reference, float depth)
{
    switch (function) {
    case GL_LEQUAL: return reference <= depth;
    case GL_GEQUAL: return reference >= depth;
    case GL_EQUAL: return reference == depth;
    default: return reference != depth;
    }
}

static float reference_expected(unsigned column, float reference, GLenum function, int fixed, int linear)
{
    if (fixed) reference = fmaxf(0, fminf(reference, 1));
    float low = column < 2 ? 0 : column < 4 ? .5f : column < 6 ? 1 : column == 6 ? 0 : .5f;
    float high = column == 6 ? .5f : 1;
    float result = reference_compare(function, reference, low);
    if (column >= 6)
        result = linear ? .5f * (result + reference_compare(function, reference, high))
                        : reference_compare(function, reference, high);
    return result;
}

static void reference_atlas(GLuint texture, GLuint framebuffer, GLuint depth_program, int fixed, int linear)
{
    GLenum internal = fixed ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT32F;
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, 8, 16, 0,
                 fixed ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT,
                 fixed ? GL_UNSIGNED_INT_24_8 : GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, fixed ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, texture, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        glc_fail("Shadow reference %s atlas framebuffer incomplete", reference_names[glc_variant]);

    glClearDepth(.25);
    glClear(GL_DEPTH_BUFFER_BIT);
    glUseProgram(depth_program);
    GLint depth_location = glGetUniformLocation(depth_program, "ndcDepth");
    /* Render two viewport tiles, then clear only the upper half to one. */
    glViewport(0, 0, 8, 4);
    glUniform1f(depth_location, -1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glViewport(0, 4, 8, 4);
    glUniform1f(depth_location, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 8, 8, 8);
    glClearDepth(1);
    glClear(GL_DEPTH_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(0);
}

static void glc_run_core_shadow_reference(void);
static const struct glc_case glc_case_core_shadow_reference GLC_SECTION = {
    .name = "core_shadow_reference", .run = glc_run_core_shadow_reference,
    .profile = GLC_CORE, .width = 64, .height = 256, .tolerance = 2,
    .variant_names = reference_names,
    .variant_count = sizeof reference_names / sizeof *reference_names
};
static void glc_run_core_shadow_reference(void)
{
    int fixed = glc_variant < 2, linear = glc_variant & 1;
    GLint original;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &original);
    GLuint textures[2], framebuffer, vao;
    glGenTextures(2, textures);
    glGenFramebuffers(1, &framebuffer);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    GLuint depth_program = glc_program(
        "#version 150 core\nuniform float ndcDepth;void main(){"
        "vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2-1,ndcDepth,1);}",
        "#version 150 core\nout vec4 unused;void main(){unused=vec4(1);}", NULL);
    reference_atlas(textures[0], framebuffer, depth_program, fixed, linear);
    reference_atlas(textures[1], framebuffer, depth_program, !fixed, linear);
    glDeleteProgram(depth_program);

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)original);
    glDisable(GL_DEPTH_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    GLuint program = glc_program(
        "#version 150 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
        "gl_Position=vec4(p*2-1,0,1);}",
        "#version 150 core\nuniform sampler2DShadow atlas,other;out vec4 color;"
        "vec3 samplePair(sampler2DShadow a,sampler2DShadow b,vec3 t,float q){"
        "return vec3(texture(a,t),textureProj(a,vec4(t*q,q)),textureLod(b,t,0.));}void main(){"
        "ivec2 c=(ivec2(gl_FragCoord.xy)%64)/8;"
        "float ref=c.y==0?-.25:c.y==1?0.:c.y==2?.25:c.y==3?.5:"
        "c.y==4?.75:c.y==5?1.:c.y==6?1.25:1.0009765625;"
        "float v=c.x<2?.125:c.x<4?.375:c.x<6?.75:c.x==6?.25:.5;"
        "vec3 t=vec3(.5,v,ref);float q=(c.x&2)==0?.5:2.;if((c.x&1)!=0)q=-q;"
        "color=vec4(samplePair(atlas,other,t,q),1);}", NULL);
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "atlas"), 0);
    glUniform1i(glGetUniformLocation(program, "other"), 1);
    /* Swap formats on the same sampler slots without relinking the program. */
    for (unsigned binding = 0; binding < 2; ++binding) {
        for (unsigned unit = 0; unit < 2; ++unit) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, textures[unit ^ binding]);
        }
        for (unsigned pass = 0; pass < 4; ++pass) {
            for (unsigned unit = 0; unit < 2; ++unit) {
                glActiveTexture(GL_TEXTURE0 + unit);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, reference_funcs[pass]);
            }
            glViewport(0, (GLint)pass * 64, 64, 64);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        unsigned char pixels[64 * 256 * 4];
        glReadPixels(0, 0, 64, 256, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        GLenum error = glGetError();
        if (error) glc_fail("Shadow reference %s binding %u GL error 0x%x", reference_names[glc_variant], binding, error);
        int first_fixed = fixed != (int)binding;
        const unsigned char *lequal_one = pixels + ((6 * 8 + 4) * 64 + 4 * 8 + 4) * 4;
        const unsigned char *gequal_zero = pixels + ((64 + 4) * 64 + 4) * 4;
        fprintf(stderr, "shadow_reference %s binding=%u signature lequal(ref=1.25,depth=1)=%u/%u/%u "
                        "gequal(ref=-.25,depth=0)=%u/%u/%u expected=%u/%u/%u\n",
                reference_names[glc_variant], binding, lequal_one[0], lequal_one[1], lequal_one[2],
                gequal_zero[0], gequal_zero[1], gequal_zero[2], first_fixed ? 255 : 0,
                first_fixed ? 255 : 0, first_fixed ? 0 : 255);
        for (unsigned pass = 0; pass < 4; ++pass)
            for (unsigned row = 0; row < 8; ++row)
                for (unsigned column = 0; column < 8; ++column) {
                    const unsigned char *pixel = pixels + (((pass * 64 + row * 8 + 4) * 64) + column * 8 + 4) * 4;
                    for (unsigned channel = 0; channel < 3; ++channel) {
                        int channel_fixed = channel == 2 ? !first_fixed : first_fixed;
                        int expected = (int)lroundf(255 * reference_expected(column, reference_values[row],
                                                                           reference_funcs[pass], channel_fixed, linear));
                        if (abs((int)pixel[channel] - expected) > 2)
                            glc_fail("Shadow reference %s binding %u %s cell %u,%u ref %.9g channel %u "
                                     "first-direct/first-proj/second-lod=%u/%u/%u expected=%d",
                                     reference_names[glc_variant], binding, reference_func_names[pass], column, row,
                                     reference_values[row], channel, pixel[0], pixel[1], pixel[2], expected);
                    }
                    if (pixel[3] != 255) glc_fail("Shadow reference alpha %u expected 255", pixel[3]);
                }
    }
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(0);
    glDeleteProgram(program);
    glDeleteTextures(2, textures);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteVertexArrays(1, &vao);
}
