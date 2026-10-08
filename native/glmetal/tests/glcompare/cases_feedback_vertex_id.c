#include "glc_gl_core.h"
#include "glcompare.h"
#include <math.h>
#include <stdio.h>
#include <stdint.h>

static const char *const id_modes[] = {"line_loop", "line_strip"};

GLC_CASE_VARIANTS(core_feedback_vertex_id, id_modes, .profile = GLC_CORE)
{
    const char *vs = "#version 410 core\nout int original_id;out vec4 original_position;"
        "void main(){original_id=gl_VertexID;original_position=vec4(float(gl_VertexID&1)*1.5-.75,"
        "float((gl_VertexID>>1)&1)*1.5-.75,0,1);gl_Position=original_position;}";
    GLuint shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(shader, 1, &vs, NULL);
    glCompileShader(shader);
    GLuint program = glCreateProgram();
    glAttachShader(program, shader);
    const char *varyings[] = {"original_id", "original_position", "gl_Position"};
    glTransformFeedbackVaryings(program, 3, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[2048] = {0};
        glGetProgramInfoLog(program, sizeof log, NULL, log);
        glc_fail("Indexed feedback ID link failed: %s", log);
    }
    GLuint vao, buffers[2], query;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(2, buffers);
    const uint32_t indices[] = {0, 1, 3, 2};
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, indices, GL_STATIC_DRAW);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[1]);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 8 * 9 * sizeof(float), NULL, GL_STREAM_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffers[1]);
    glGenQueries(1, &query);
    glUseProgram(program);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, query);
    glBeginTransformFeedback(GL_LINES);
    glDrawElements(glc_variant ? GL_LINE_STRIP : GL_LINE_LOOP, 4, GL_UNSIGNED_INT, NULL);
    glEndTransformFeedback();
    glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
    glDisable(GL_RASTERIZER_DISCARD);
    GLuint primitives = 0;
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, &primitives);
    struct record { int32_t id; float position[4], builtin_position[4]; } records[8];
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof records, records);
    GLenum error = glGetError();
    const int expected[] = {0, 1, 1, 3, 3, 2, 2, 0};
    int vertices = glc_variant ? 6 : 8;
    fprintf(stderr, "Indexed feedback %s primitives=%u expected=%d error=0x%x IDs:",
            id_modes[glc_variant], primitives, vertices / 2, error);
    for (int i = 0; i < vertices; ++i) fprintf(stderr, " %d", records[i].id);
    fprintf(stderr, "\n");
    if (error || primitives != (GLuint)(vertices / 2)) glc_fail("Indexed feedback primitive count differs");
    for (int i = 0; i < vertices; ++i) {
        float position[] = {(expected[i] & 1) * 1.5f - .75f, ((expected[i] >> 1) & 1) * 1.5f - .75f, 0, 1};
        if (records[i].id != expected[i]) glc_fail("Feedback record %d ID %d, expected %d", i, records[i].id, expected[i]);
        for (int k = 0; k < 4; ++k)
            if (fabsf(records[i].position[k] - position[k]) > 1e-7f ||
                fabsf(records[i].builtin_position[k] - position[k]) > 1e-7f)
                glc_fail("Feedback record %d position component %d differs", i, k);
    }
    glUseProgram(0);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
    glDeleteQueries(1, &query);
    glDeleteBuffers(2, buffers);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
    glDeleteShader(shader);
    glClearColor(.125f, .25f, .5f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
