#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdio.h>

static const char *const cube_lod_states[] = {
    "linear_off", "linear_on", "nearest_mip_off", "nearest_mip_on",
    "nonmip_off", "nonmip_on", "min_lod_off", "min_lod_on",
    "max_lod_off", "max_lod_on", "base_level_off", "base_level_on",
    "sampler_override_off", "sampler_override_on"
};
GLC_CASE_VARIANTS(core_cube_lod_state, cube_lod_states, .profile = GLC_CORE, .tolerance = 2)
{
    int state = glc_variant / 2;
    GLuint program = glc_program(
        "#version 410 core\nout vec2 uv;void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
        "uv=p;gl_Position=vec4(p*2-1,0,1);}",
        "#version 410 core\nuniform samplerCube t;in vec2 uv;out vec4 frag;"
        "void main(){vec3 d=normalize(vec3(uv*2-1,.6));vec2 q=textureQueryLod(t,d);"
        "frag=vec4((q.x+4)/8,(q.y+4)/8,texture(t,d,2.0).r,"
        "textureGrad(t,d,dFdx(d)*4,dFdy(d)*4).r);}", NULL);
    glUseProgram(program);
    GLuint vao, cube;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenTextures(1, &cube);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
    for (int level = 0, size = 16; level < 5; ++level, size >>= 1) {
        float values[16 * 16 * 4];
        for (int i = 0; i < size * size; ++i) {
            values[4 * i] = values[4 * i + 1] = values[4 * i + 2] = level / 8.0f;
            values[4 * i + 3] = 1;
        }
        for (int face = 0; face < 6; ++face)
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, level, GL_RGBA32F,
                         size, size, 0, GL_RGBA, GL_FLOAT, values);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, 4);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER,
                    state == 1 ? GL_LINEAR_MIPMAP_NEAREST : state == 2 ? GL_LINEAR : GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (state == 3) glTexParameterf(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_LOD, .75f);
    if (state == 4) glTexParameterf(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LOD, 1.25f);
    if (state == 5) glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL, 2);
    GLuint sampler = 0;
    if (state == 6) {
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameterf(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LOD, .25f);
        glGenSamplers(1, &sampler);
        glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, .75f);
        glSamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, 1.75f);
        glBindSampler(0, sampler);
    }
    GLfloat min_lod, max_lod;
    GLint base_level, min_filter;
    glGetTexParameterfv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_LOD, &min_lod);
    glGetTexParameterfv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LOD, &max_lod);
    glGetTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL, &base_level);
    glGetTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, &min_filter);
    if (min_lod != (state == 3 ? .75f : -1000.0f) ||
        max_lod != (state == 4 ? 1.25f : state == 6 ? .25f : 1000.0f) ||
        base_level != (state == 5 ? 2 : 0) ||
        min_filter != (state == 6 ? GL_NEAREST : state == 1 ? GL_LINEAR_MIPMAP_NEAREST :
                       state == 2 ? GL_LINEAR : GL_LINEAR_MIPMAP_LINEAR))
        glc_fail("Cube LOD texture query differs from assigned state");
    if (sampler) {
        glGetSamplerParameterfv(sampler, GL_TEXTURE_MIN_LOD, &min_lod);
        glGetSamplerParameterfv(sampler, GL_TEXTURE_MAX_LOD, &max_lod);
        glGetSamplerParameteriv(sampler, GL_TEXTURE_MIN_FILTER, &min_filter);
        if (min_lod != .75f || max_lod != 1.75f || min_filter != GL_LINEAR_MIPMAP_LINEAR)
            glc_fail("Cube LOD sampler query differs from assigned state");
    }
    if (glc_variant & 1) glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    else glDisable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    glUniform1i(glGetUniformLocation(program, "t"), 0);
    GLint original;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &original);
    GLuint fb, colour;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glGenTextures(1, &colour);
    glBindTexture(GL_TEXTURE_2D, colour);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 64, 64, 0, GL_RGBA, GL_FLOAT, NULL);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        glc_fail("Cube LOD state float target incomplete");
    glDrawArrays(GL_TRIANGLES, 0, 3);
    const int x[] = {8, 32, 56};
    for (int i = 0; i < 3; ++i) {
        float result[4];
        glReadPixels(x[i], 32, 1, 1, GL_RGBA, GL_FLOAT, result);
        fprintf(stderr, "Cube LOD state %s x%d q=%a/%a bias=%a grad=%a\n",
                cube_lod_states[glc_variant], x[i], result[0] * 8 - 4, result[1] * 8 - 4, result[2], result[3]);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)original);
    glBlitFramebuffer(0, 0, 64, 64, 0, 0, 64, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)original);
    /* Keep the image opaque while retaining gradient sampling in the log. */
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    GLenum error = glGetError();
    if (error) glc_fail("Cube LOD state %s GL error %x", cube_lod_states[glc_variant], error);
    glBindSampler(0, 0);
    if (sampler) glDeleteSamplers(1, &sampler);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &colour);
    glDeleteTextures(1, &cube);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
}
