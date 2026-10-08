#include "glc_gl_core.h"
#include "glcompare.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static uint32_t bits(float v)
{
    uint32_t result;
    memcpy(&result, &v, sizeof result);
    return result;
}

static void direction(int x, int y, float out[3], int normalize)
{
    out[0] = ((x + .5f) / 64) * 4 - 2;
    out[1] = ((y + .5f) / 64 - .5f) * .3f;
    out[2] = .6f;
    if (normalize) {
        float inv = 1 / sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
        for (int i = 0; i < 3; ++i) out[i] *= inv;
    }
}

static const char *names[] = {"colour_clamp_off", "colour_clamp_on", "shadow_clamp_off", "shadow_clamp_on"};

GLC_CASE_VARIANTS(driver_cube_precision, names, .profile = GLC_CORE)
{
    int shadow = glc_variant >= 2;
    const char *vs = "#version 410 core\nout vec2 uv;void main(){"
        "vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);uv=p;gl_Position=vec4(p*2-1,0,1);}";
    const char *sample = shadow ?
        "vec4 sampleOriginal(vec3 d,vec3 dx,vec3 dy){float r=uv.y*.7+.15;"
        "return vec4(texture(t,vec4(d,r)),texture(t,vec4(d.zxy,r),1.25),"
        "textureGrad(t,vec4(-d.yzx,r),-dx.yzx*2,-dy.yzx*.5),1);}"
        "vec4 sampleGrad(vec3 d,vec3 dx,vec3 dy){float r=reference_value;"
        "return vec4(textureGrad(t,vec4(d,r),dx,dy),"
        "textureGrad(t,vec4(d.zxy,r),dx.zxy*exp2(1.25),dy.zxy*exp2(1.25)),"
        "textureGrad(t,vec4(-d.yzx,r),-dx.yzx*2,-dy.yzx*.5),1);}" :
        "vec4 sampleOriginal(vec3 d,vec3 dx,vec3 dy){return vec4(texture(t,d).r,"
        "texture(t,d.zxy,1.25).g,textureGrad(t,-d.yzx,-dx.yzx*2,-dy.yzx*.5).b,1);}"
        "vec4 sampleGrad(vec3 d,vec3 dx,vec3 dy){return vec4(textureGrad(t,d,dx,dy).r,"
        "textureGrad(t,d.zxy,dx.zxy*exp2(1.25),dy.zxy*exp2(1.25)).g,"
        "textureGrad(t,-d.yzx,-dx.yzx*2,-dy.yzx*.5).b,1);}";
    char fs[4096];
    snprintf(fs, sizeof fs, "#version 410 core\nuniform %s t;in vec2 uv;"
        "uniform int mode;uniform vec3 fixed_d,fixed_dx,fixed_dy;uniform float reference_value;"
        "layout(location=0)out vec4 value;layout(location=1)out vec4 direction_out;"
        "layout(location=2)out vec4 dx_out;layout(location=3)out vec4 dy_out;"
        "layout(location=4)out vec4 byte_out;%s"
        "void main(){vec3 raw=vec3(uv.x*4-2,(uv.y-.5)*.3,.6);"
        "vec3 d=mode==0?normalize(raw):mode==1?raw:fixed_d;"
        "vec3 dx=dFdx(d),dy=dFdy(d);if(mode==4){dx=fixed_dx;dy=fixed_dy;}"
        "value=mode==4?sampleGrad(d,dx,dy):sampleOriginal(d,dx,dy);"
        "direction_out=vec4(d,mode==4?reference_value:uv.y*.7+.15);"
        "dx_out=vec4(dx,uv.x);dy_out=vec4(dy,uv.y);byte_out=value;}",
        shadow ? "samplerCubeShadow" : "samplerCube", sample);
    GLuint program = glc_program(vs, fs, NULL);
    glUseProgram(program);
    GLuint vao, cube, sampler, fbo, attachments[5];
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenTextures(1, &cube);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
    for (int level = 0, size = 32; level < 6; ++level, size >>= 1) {
        for (int face = 0; face < 6; ++face) {
            float values[32 * 32 * 4];
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    float u = size > 1 ? (float)x / (size - 1) : .5f;
                    float v = size > 1 ? (float)y / (size - 1) : .5f;
                    if (shadow) values[y * size + x] = u * .65f + v * .08f + face * .025f + level * .012f;
                    else {
                        int i = (y * size + x) * 4;
                        values[i] = u * .65f + face * .025f + level * .012f;
                        values[i + 1] = v * .65f + face * .025f + level * .012f;
                        values[i + 2] = (float)((x + 3 * y + face) & 3) / 4 + level * .018f;
                        values[i + 3] = 1;
                    }
                }
            }
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, level,
                shadow ? GL_DEPTH_COMPONENT32F : GL_RGBA32F, size, size, 0,
                shadow ? GL_DEPTH_COMPONENT : GL_RGBA, GL_FLOAT, values);
        }
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, 5);
    glGenSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    GLfloat anisotropy = 1;
    glGetFloatv(0x84FF, &anisotropy);
    if (anisotropy < 2) glc_fail("Cube anisotropy unavailable");
    glSamplerParameterf(sampler, 0x84FE, fminf(anisotropy, 8));
    if (shadow) {
        glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    }
    glBindSampler(0, sampler);
    if (glc_variant & 1) glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    else glDisable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    GLint old_draw, old_read;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(5, attachments);
    GLenum draw_buffers[5];
    for (int i = 0; i < 5; ++i) {
        glBindTexture(GL_TEXTURE_2D, attachments[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, i == 4 ? GL_RGBA8 : GL_RGBA32F, 64, 64, 0, GL_RGBA, GL_FLOAT, NULL);
        draw_buffers[i] = GL_COLOR_ATTACHMENT0 + i;
        glFramebufferTexture2D(GL_FRAMEBUFFER, draw_buffers[i], GL_TEXTURE_2D, attachments[i], 0);
    }
    glDrawBuffers(5, draw_buffers);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        glc_fail("Cube precision framebuffer incomplete");
    glViewport(0, 0, 64, 64);
    const int pixels[3][2] = {{37, 43}, {41, 20}, {41, 43}};
    const char *modes[] = {"normalized", "raw", "uniform_raw", "uniform_normalized", "uniform_grad"};
    for (int mode = 0; mode < 5; ++mode) {
        int draws = mode < 2 ? 1 : 3;
        for (int p = 0; p < draws; ++p) {
            float d[3], left[3], right[3], bottom[3], top[3], dx[3], dy[3];
            int x = pixels[p][0], y = pixels[p][1];
            direction(x, y, d, mode != 2);
            direction(x & ~1, y, left, 1);
            direction((x & ~1) + 1, y, right, 1);
            direction(x, y & ~1, bottom, 1);
            direction(x, (y & ~1) + 1, top, 1);
            for (int i = 0; i < 3; ++i) { dx[i] = right[i] - left[i]; dy[i] = top[i] - bottom[i]; }
            glUniform1i(glGetUniformLocation(program, "mode"), mode);
            glUniform3fv(glGetUniformLocation(program, "fixed_d"), 1, d);
            glUniform3fv(glGetUniformLocation(program, "fixed_dx"), 1, dx);
            glUniform3fv(glGetUniformLocation(program, "fixed_dy"), 1, dy);
            glUniform1f(glGetUniformLocation(program, "reference_value"), ((y + .5f) / 64) * .7f + .15f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            for (int sample_id = 0; sample_id < (mode < 2 ? 3 : 1); ++sample_id) {
                int point = mode < 2 ? sample_id : p;
                fprintf(stderr, "cube_precision %s mode=%s point=(%d,%d)\n", names[glc_variant], modes[mode], pixels[point][0], pixels[point][1]);
                for (int a = 0; a < 5; ++a) {
                    glReadBuffer(GL_COLOR_ATTACHMENT0 + a);
                    if (a == 4) {
                        unsigned char bytes[4];
                        glReadPixels(pixels[point][0], pixels[point][1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bytes);
                        fprintf(stderr, "  rgba8 %u %u %u %u\n", bytes[0], bytes[1], bytes[2], bytes[3]);
                    } else {
                        float values[4];
                        glReadPixels(pixels[point][0], pixels[point][1], 1, 1, GL_RGBA, GL_FLOAT, values);
                        fprintf(stderr, "  attachment%d bits=%08x,%08x,%08x,%08x hex=%a,%a,%a,%a\n", a,
                            bits(values[0]), bits(values[1]), bits(values[2]), bits(values[3]),
                            values[0], values[1], values[2], values[3]);
                    }
                }
            }
            GLenum error = glGetError();
            if (error) glc_fail("Cube precision error %x", error);
            usleep(50000);
        }
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)old_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)old_read);
    glViewport(0, 0, glc_width, glc_height);
    glDisable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    glBindSampler(0, 0);
    glDeleteSamplers(1, &sampler);
    glDeleteTextures(5, attachments);
    glDeleteTextures(1, &cube);
    glDeleteFramebuffers(1, &fbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
    glClearColor(.25f, .5f, .75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
