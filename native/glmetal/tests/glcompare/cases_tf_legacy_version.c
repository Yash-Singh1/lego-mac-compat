/* Internal SSBO capture must not require an SSBO-capable user GLSL version. */
#include "glc_gl_core.h"
#include "glcompare.h"
#include <math.h>

static const char *const tf_version_modes[] = {"vertex140", "geometry150"};
GLC_CASE_VARIANTS(core_tf_legacy_version, tf_version_modes, .profile = GLC_CORE)
{
    const char *vs140 = "#version 140\nout vec4 value;void main(){gl_Position=vec4(0,0,0,1);value=vec4(float(gl_VertexID)+.25,.5,.75,1);}";
    const char *vs150 = "#version 150\nout vec4 inputValue;void main(){gl_Position=vec4(0,0,0,1);inputValue=vec4(float(gl_VertexID)+.25,.5,.75,1);}";
    const char *gs150 = "#version 150\nlayout(points) in;layout(points,max_vertices=1) out;in vec4 inputValue[];out vec4 value;void main(){gl_Position=gl_in[0].gl_Position;value=inputValue[0]+vec4(.125,0,0,0);EmitVertex();EndPrimitive();}";
    GLuint program = glCreateProgram();
    const GLenum stages[] = {GL_VERTEX_SHADER, GL_GEOMETRY_SHADER};
    const char *sources[] = {glc_variant ? vs150 : vs140, gs150};
    for (int i = 0; i < (glc_variant ? 2 : 1); ++i) {
        GLuint shader = glCreateShader(stages[i]);
        glShaderSource(shader, 1, &sources[i], NULL);
        glCompileShader(shader);
        GLint ok;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[2048]; glGetShaderInfoLog(shader, sizeof log, NULL, log); glc_fail("TF version compile: %s", log); }
        glAttachShader(program, shader);
        glDeleteShader(shader);
    }
    const char *varying = "value";
    glTransformFeedbackVaryings(program, 1, &varying, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(program);
    GLint ok;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(program, sizeof log, NULL, log); glc_fail("TF version link: %s", log); }
    GLuint vao, buffer, query;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &buffer); glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 3 * 4 * sizeof(float), NULL, GL_STREAM_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
    glGenQueries(1, &query);
    glUseProgram(program); glEnable(GL_RASTERIZER_DISCARD);
    glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, query);
    glBeginTransformFeedback(GL_POINTS); glDrawArrays(GL_POINTS, 0, 3); glEndTransformFeedback();
    glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
    glDisable(GL_RASTERIZER_DISCARD);
    GLuint count; glGetQueryObjectuiv(query, GL_QUERY_RESULT, &count);
    if (count != 3) glc_fail("TF version primitive count %u", count);
    float values[12]; glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof values, values);
    for (int i = 0; i < 3; ++i) {
        const float expected[] = {i + .25f + (glc_variant ? .125f : 0), .5f, .75f, 1};
        for (int j = 0; j < 4; ++j)
            if (values[4*i+j] != expected[j]) glc_fail("TF version value %d/%d: %g != %g", i, j, values[4*i+j], expected[j]);
    }
    if (glGetError()) glc_fail("TF version GL error");
    glDeleteQueries(1, &query); glDeleteBuffers(1, &buffer); glDeleteVertexArrays(1, &vao); glDeleteProgram(program);
}
