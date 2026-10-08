/* Standalone GLMetal conformance probe. Apple's core component query is broken. */
#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdio.h>

GLC_CASE(driver_varying_limits, .profile = GLC_CORE)
{
    GLint components = 0, vectors = 0, outputs = 0;
    glGetIntegerv(GL_MAX_VARYING_COMPONENTS, &components);
    GLenum component_error = glGetError();
    glGetIntegerv(GL_MAX_VARYING_VECTORS, &vectors);
    GLenum vector_error = glGetError();
    glGetIntegerv(GL_MAX_VERTEX_OUTPUT_COMPONENTS, &outputs);
    fprintf(stderr, "Varying limits: components=%d error=0x%x vectors=%d error=0x%x vertex_outputs=%d\n",
            components, component_error, vectors, vector_error, outputs);
    if (component_error || vector_error || components < 60 || vectors != components / 4)
        glc_fail("Required core varying limits are invalid");
    if (components != 60 || outputs != 64) glc_fail("Unexpected configured compiler capacity");
    const char *vs = "#version 410 core\nout vec4 values[15];float proof[(gl_MaxVaryingVectors==15 && gl_MaxVaryingComponents==60)?1:-1];\n"
        "void main(){gl_Position=vec4(0,0,0,1);for(int i=0;i<15;++i)values[i]=vec4(i+1,i+2,gl_MaxVaryingVectors,gl_MaxVaryingComponents);}";
    const char *fs = "#version 410 core\nin vec4 values[15];out vec4 color;\n"
        "void main(){color=vec4(0);for(int i=0;i<15;++i)color+=values[i];}";
    GLuint vertex = glCreateShader(GL_VERTEX_SHADER), fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vertex, 1, &vs, NULL);
    glShaderSource(fragment, 1, &fs, NULL);
    glCompileShader(vertex);
    glCompileShader(fragment);
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    const char *varying = "values";
    glTransformFeedbackVaryings(program, 1, &varying, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[2048] = {0};
        glGetProgramInfoLog(program, sizeof log, NULL, log);
        glc_fail("60-component varying interface link failed: %s", log);
    }
    GLuint buffer, vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 60 * sizeof(float), NULL, GL_STREAM_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
    glUseProgram(program);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, 1);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    float values[60] = {0};
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof values, values);
    if (glGetError()) glc_fail("Limit-capacity capture failed");
    for (int i = 0; i < 60; ++i)
        if (values[i] != (float)(i % 4 == 2 ? 15 : i % 4 == 3 ? 60 : i / 4 + i % 4 + 1))
            glc_fail("Limit-capacity capture word %d differs: %g", i, values[i]);
    glUseProgram(0);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
    glDeleteBuffers(1, &buffer);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
