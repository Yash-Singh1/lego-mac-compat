#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *const matrix_array_modes[] = {"dmat2x2", "dmat2x3", "dmat2x4", "dmat3x2", "dmat3x3",
                                               "dmat3x4", "dmat4x2", "dmat4x3", "dmat4x4"};
GLC_CASE_VARIANTS(core_fp64_matrix_arrays, matrix_array_modes, .profile = GLC_CORE)
{
    int columns = glc_variant / 3 + 2, rows = glc_variant % 3 + 2, elements = columns * rows;
    const char *type = matrix_array_modes[glc_variant];
    char source[4096], constructor[512] = {0}, expected_constructor[1024] = {0};
    for (int c = 0; c < columns; ++c) {
        size_t used = strlen(constructor);
        snprintf(constructor + used, sizeof constructor - used, "%scopied[%d]", c ? "," : "", c);
    }
    for (int e = 0; e < elements; ++e) {
        size_t used = strlen(expected_constructor);
        snprintf(expected_constructor + used, sizeof expected_constructor - used, "%s16777217+i*%d+%d", e ? "," : "", elements, e);
    }
    snprintf(source, sizeof source,
        "#version 410 core\nlayout(std140) uniform Params {float before;%s values[3];float after;};"
        "flat out %s result;flat out uint valid;"
        "%s copyValue(int i){%s local[3]=values;%s copied=local[i];return %s(%s);}"
        "void main(){int i=gl_VertexID;result=copyValue(i);"
        "valid=(before==0.25 && after==0.75 && result==%s(%s))?1u:0u;gl_Position=vec4(0);}",
        type, type, type, type, type, type, constructor, type, expected_constructor);
    GLuint shader = glCreateShader(GL_VERTEX_SHADER), program = glCreateProgram();
    glShaderSource(shader, 1, (const char *const[]){source}, NULL); glCompileShader(shader);
    GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0}; glGetShaderInfoLog(shader, sizeof log, NULL, log);
        glc_fail("Exact matrix array compile: %s", log);
    }
    glAttachShader(program, shader);
    const char *varyings[] = {"result", "valid", "gl_SkipComponents1"};
    glTransformFeedbackVaryings(program, 3, varyings, GL_INTERLEAVED_ATTRIBS); glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0}; glGetProgramInfoLog(program, sizeof log, NULL, log);
        glc_fail("Exact matrix array link: %s", log);
    }
    glDeleteShader(shader); glUseProgram(program);
    const char *names[] = {"before", "values[0]", "after"};
    GLuint indices[3]; GLint offsets[3], array_stride = 0, matrix_stride = 0, uniform_type = 0, row_major = 1, block_size = 0;
    glGetUniformIndices(program, 3, names, indices);
    glGetActiveUniformsiv(program, 3, indices, GL_UNIFORM_OFFSET, offsets);
    glGetActiveUniformsiv(program, 1, indices + 1, GL_UNIFORM_ARRAY_STRIDE, &array_stride);
    glGetActiveUniformsiv(program, 1, indices + 1, GL_UNIFORM_MATRIX_STRIDE, &matrix_stride);
    glGetActiveUniformsiv(program, 1, indices + 1, GL_UNIFORM_TYPE, &uniform_type);
    glGetActiveUniformsiv(program, 1, indices + 1, GL_UNIFORM_IS_ROW_MAJOR, &row_major);
    GLuint block = glGetUniformBlockIndex(program, "Params");
    glGetActiveUniformBlockiv(program, block, GL_UNIFORM_BLOCK_DATA_SIZE, &block_size);
    const GLenum types[] = {GL_DOUBLE_MAT2, GL_DOUBLE_MAT2x3, GL_DOUBLE_MAT2x4, GL_DOUBLE_MAT3x2, GL_DOUBLE_MAT3,
                            GL_DOUBLE_MAT3x4, GL_DOUBLE_MAT4x2, GL_DOUBLE_MAT4x3, GL_DOUBLE_MAT4};
    int column_stride = rows == 2 ? 16 : 32;
    if (uniform_type != (GLint)types[glc_variant] || row_major || offsets[0] != 0 || offsets[1] != column_stride ||
        offsets[2] != column_stride + 3 * columns * column_stride || matrix_stride != column_stride ||
        array_stride != columns * column_stride || block_size > 512 || block_size <= offsets[2])
        glc_fail("Exact matrix layout: %s offsets=%d/%d/%d strides=%d/%d type=%x row_major=%d size=%d",
                 type, offsets[0], offsets[1], offsets[2], matrix_stride, array_stride, uniform_type, row_major, block_size);
    unsigned char storage[512]; memset(storage, 0xa5, sizeof storage);
    float before = .25f, after = .75f;
    memcpy(storage + offsets[0], &before, 4); memcpy(storage + offsets[2], &after, 4);
    double expected[3][16];
    for (int vertex = 0; vertex < 3; ++vertex) for (int c = 0; c < columns; ++c) for (int r = 0; r < rows; ++r) {
        double value = 16777217.0 + vertex * elements + c * rows + r;
        expected[vertex][c * rows + r] = value;
        memcpy(storage + offsets[1] + vertex * array_stride + c * matrix_stride + r * 8, &value, 8);
    }
    GLuint uniform, capture, vao;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &uniform); glBindBuffer(GL_UNIFORM_BUFFER, uniform);
    glBufferData(GL_UNIFORM_BUFFER, block_size, storage, GL_STATIC_DRAW);
    glUniformBlockBinding(program, block, 0); glBindBufferBase(GL_UNIFORM_BUFFER, 0, uniform);
    int record = elements * 8 + 8;
    glGenBuffers(1, &capture); glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, capture);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, record * 3, NULL, GL_STREAM_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, capture);
    glEnable(GL_RASTERIZER_DISCARD); glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, 3); glEndTransformFeedback(); glDisable(GL_RASTERIZER_DISCARD);
    unsigned char output[408];
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, record * 3, output);
    bool equal = true;
    for (int vertex = 0; vertex < 3; ++vertex) {
        uint32_t valid; memcpy(&valid, output + vertex * record + elements * 8, 4);
        if (memcmp(output + vertex * record, expected[vertex], elements * 8) || valid != 1) equal = false;
    }
    GLenum error = glGetError();
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, 0); glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
    glDeleteBuffers(1, &uniform); glDeleteBuffers(1, &capture); glUseProgram(0);
    glDeleteProgram(program); glDeleteVertexArrays(1, &vao);
    if (!equal || error) glc_fail("Exact matrix array copy/constructor/capture mismatch: %s error=%x", type, error);
    glClearColor(.25f, .5f, .75f, 1); glClear(GL_COLOR_BUFFER_BIT);
}
