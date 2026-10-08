#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdio.h>
#include <string.h>

static const char *const feedback_names[] = {
    "input", "uniform", "missing", "duplicate", "array_range", "array_and_element", "separate_components", "skip_limit"
};

GLC_CASE_VARIANTS(driver_feedback_output_validation, feedback_names, .profile = GLC_CORE)
{
    const char *source = "#version 410 core\nin float data;uniform float value;out vec3 result[2];"
        "void main(){result[0]=vec3(data+value);result[1]=vec3(1);gl_Position=vec4(0,0,0,1);}";
    GLuint shader = glCreateShader(GL_VERTEX_SHADER), program = glCreateProgram();
    glShaderSource(shader, 1, &source, NULL); glCompileShader(shader);
    GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) glc_fail("Feedback output validation fixture did not compile");
    glAttachShader(program, shader);
    const char *names[17] = {"data"};
    GLsizei count = 1; GLenum mode = GL_INTERLEAVED_ATTRIBS;
    if (glc_variant == 1) names[0] = "value";
    if (glc_variant == 2) names[0] = "missing";
    if (glc_variant == 3) { names[0] = names[1] = "result[0]"; count = 2; }
    if (glc_variant == 4) names[0] = "result[2]";
    if (glc_variant == 5) { names[0] = "result"; names[1] = "result[1]"; count = 2; }
    if (glc_variant == 6) { names[0] = "result"; mode = GL_SEPARATE_ATTRIBS; }
    if (glc_variant == 7) { for (int i = 0; i < 17; ++i) names[i] = "gl_SkipComponents4"; count = 17; }
    glTransformFeedbackVaryings(program, count, names, mode); glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (glc_variant == 5) {
        if (!ok) glc_fail("Whole array and individual element must be distinct capture requests");
        GLuint vao, buffer;
        glGenVertexArrays(1, &vao); glBindVertexArray(vao);
        glGenBuffers(1, &buffer); glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
        float values[9] = {-1,-1,-1,-1,-1,-1,-1,-1,-1};
        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, sizeof values, values, GL_STREAM_READ);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
        glUseProgram(program); glEnable(GL_RASTERIZER_DISCARD);
        glBeginTransformFeedback(GL_POINTS); glDrawArrays(GL_POINTS, 0, 1); glEndTransformFeedback();
        glDisable(GL_RASTERIZER_DISCARD);
        glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof values, values);
        for (int i = 0; i < 9; ++i)
            if (values[i] != (i < 3 ? 0 : 1))
                glc_fail("Whole array and element capture word %d was %g", i, values[i]);
        glUseProgram(0); glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
        glDeleteBuffers(1, &buffer); glDeleteVertexArrays(1, &vao);
    } else if (ok) glc_fail("Invalid feedback output request linked: %s", feedback_names[glc_variant]);
    if (glGetError()) glc_fail("Feedback link failure raised an API error");
    const char *valid[] = {"result[0]", "result[1]"};
    glTransformFeedbackVaryings(program, 2, valid, GL_INTERLEAVED_ATTRIBS); glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) glc_fail("Disjoint valid output elements did not relink");
    glDeleteProgram(program); glDeleteShader(shader);
    glClearColor(.25f, .5f, .75f, 1); glClear(GL_COLOR_BUFFER_BIT);
}

GLC_CASE(driver_feedback_link_requires_vertex_stage, .profile = GLC_CORE)
{
    const char *source = "#version 410 core\nout vec4 color;void main(){color=vec4(1);}";
    GLuint shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) glc_fail("Fragment-only fixture did not compile");
    GLuint program = glCreateProgram();
    glAttachShader(program, shader);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) glc_fail("Fragment-only program without feedback must link");
    const char *varyings[] = {"color"};
    glTransformFeedbackVaryings(program, 1, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    char log[1024] = {0};
    glGetProgramInfoLog(program, sizeof log, NULL, log);
    fprintf(stderr, "Fragment-only feedback link=%d log=%s\n", ok, log);
    if (ok) glc_fail("Fragment outputs must not satisfy transform feedback capture");
    if (glGetError()) glc_fail("Link-status validation must not raise an API error");
    glTransformFeedbackVaryings(program, 0, NULL, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) glc_fail("Removing feedback must permit fragment-only relink");
    glDeleteProgram(program);
    glDeleteShader(shader);
    glClearColor(.25f, .5f, .75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLC_CASE(driver_feedback_invalid_request_preserves_state, .profile = GLC_CORE)
{
    GLuint program = glCreateProgram();
    const char *names[] = {"result", "a", "b", "c", "d"};
    glTransformFeedbackVaryings(program, 1, names, GL_INTERLEAVED_ATTRIBS);
    glTransformFeedbackVaryings(program, -1, names, GL_INTERLEAVED_ATTRIBS);
    if (glGetError() != GL_INVALID_VALUE) glc_fail("Negative count must raise INVALID_VALUE");
    glTransformFeedbackVaryings(program, 5, names, GL_SEPARATE_ATTRIBS);
    if (glGetError() != GL_INVALID_VALUE) glc_fail("Excess separate count must raise INVALID_VALUE");
    glTransformFeedbackVaryings(program, 1, names, GL_POINTS);
    if (glGetError() != GL_INVALID_ENUM) glc_fail("Invalid mode must raise INVALID_ENUM");
    const char *source = "#version 410 core\nout float result;"
        "void main(){result=7;gl_Position=vec4(0,0,0,1);}";
    GLuint shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(shader, 1, &source, NULL); glCompileShader(shader);
    glAttachShader(program, shader); glLinkProgram(program);
    GLint ok = 0, count = 0, mode = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    glGetProgramiv(program, GL_TRANSFORM_FEEDBACK_VARYINGS, &count);
    glGetProgramiv(program, GL_TRANSFORM_FEEDBACK_BUFFER_MODE, &mode);
    if (!ok || count != 1 || mode != GL_INTERLEAVED_ATTRIBS)
        glc_fail("Invalid requests changed the previous capture declaration");
    char name[32] = {0}; GLsizei length = 0, size = 0; GLenum type = 0;
    glGetTransformFeedbackVarying(program, 0, sizeof name, &length, &size, &type, name);
    if (length != 6 || size != 1 || type != GL_FLOAT || strcmp(name, "result"))
        glc_fail("Invalid requests changed the previous capture output");
    glDeleteProgram(program); glDeleteShader(shader);
    glClearColor(.25f, .5f, .75f, 1); glClear(GL_COLOR_BUFFER_BIT);
}
