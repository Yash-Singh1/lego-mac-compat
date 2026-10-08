/* Apple uses the native trailing-zero count for runtime zero, but folds
   constant findLSB(0) to -1. Check both stages and integer signedness. */
#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdio.h>

static const char *const modes[] = {"vertex", "fragment"};
GLC_CASE_VARIANTS(core_findlsb_zero_semantics, modes, .profile = GLC_CORE)
{
    GLint original_fb;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &original_fb);
    const char *expr = "uniform uint value; void main(){ result=ivec4(findLSB(value),findLSB(int(value)),findLSB(0u),findLSB(0));";
    char source[1024];
    GLuint p, vao, buffer = 0;
    if (!glc_variant) {
        snprintf(source, sizeof source, "#version 410 core\nflat out ivec4 result;%s gl_Position=vec4(0,0,0,1);}", expr);
        GLuint shader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(shader, 1, (const char *[]){source}, NULL);
        glCompileShader(shader);
        p = glCreateProgram();
        glAttachShader(p, shader);
        const char *varying = "result";
        glTransformFeedbackVaryings(p, 1, &varying, GL_INTERLEAVED_ATTRIBS);
        glLinkProgram(p);
        glGenBuffers(1, &buffer);
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 16, NULL, GL_STREAM_READ);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
    } else {
        snprintf(source, sizeof source, "#version 410 core\nout ivec4 result;%s}", expr);
        p = glc_program("#version 410 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2)*2.-1.;gl_Position=vec4(p,0,1);}", source, NULL);
        GLuint fb, tex;
        glGenFramebuffers(1, &fb);
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32I, 64, 64, 0, GL_RGBA_INTEGER, GL_INT, NULL);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    }
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        glc_fail("link %s", log);
    }
    glUseProgram(p);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    const GLuint values[] = {0, 1, 0x80000000u, 0x80008000u, 0xffffffffu};
    for (int i = 0; i < 5; ++i) {
        GLint result[4] = {-999, -999, -999, -999};
        glUniform1ui(glGetUniformLocation(p, "value"), values[i]);
        if (!glc_variant) {
            glEnable(GL_RASTERIZER_DISCARD);
            glBeginTransformFeedback(GL_POINTS);
            glDrawArrays(GL_POINTS, 0, 1);
            glEndTransformFeedback();
            glDisable(GL_RASTERIZER_DISCARD);
            glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof result, result);
        } else {
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glReadPixels(0, 0, 1, 1, GL_RGBA_INTEGER, GL_INT, result);
        }
        GLint expected = values[i] ? __builtin_ctz(values[i]) : 32;
        if (result[0] != expected || result[1] != expected || result[2] != -1 || result[3] != -1)
            glc_fail("findLSB %s input %08x: %d/%d/%d/%d, expected %d/%d/-1/-1",
                     modes[glc_variant], values[i], result[0], result[1], result[2], result[3], expected, expected);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)original_fb);
    glClearColor(.25f, .5f, .75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
