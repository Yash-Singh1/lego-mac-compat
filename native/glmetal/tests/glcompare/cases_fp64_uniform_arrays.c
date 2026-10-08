#include "glc_gl_core.h"
#include "glcompare.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static GLuint fp64_array_program(const char *source, const char *const *varyings, int count, GLuint *vao)
{
    GLuint shader = glCreateShader(GL_VERTEX_SHADER), program = glCreateProgram();
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0}; glGetShaderInfoLog(shader, sizeof log, NULL, log);
        glc_fail("Exact FP64 array compile: %s", log);
    }
    glAttachShader(program, shader);
    glTransformFeedbackVaryings(program, count, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0}; glGetProgramInfoLog(program, sizeof log, NULL, log);
        glc_fail("Exact FP64 array link: %s", log);
    }
    glDeleteShader(shader);
    glUseProgram(program);
    glGenVertexArrays(1, vao); glBindVertexArray(*vao);
    return program;
}

static GLuint fp64_array_capture(size_t size, int vertices)
{
    GLuint buffer;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, (GLsizeiptr)size, NULL, GL_STREAM_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS); glDrawArrays(GL_POINTS, 0, vertices); glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    return buffer;
}

static const char *const fp64_array_modes[] = {"double", "dvec2", "dvec3", "dvec4"};
GLC_CASE_VARIANTS(core_fp64_std140_arrays, fp64_array_modes, .profile = GLC_CORE)
{
    int lanes = glc_variant + 1;
    char source[2048];
    const char *comparison = lanes == 1 ? "values[i]==-result" : "all(equal(values[i],-result))";
    snprintf(source, sizeof source, "#version 410 core\nlayout(std140) uniform Params {float before;%s values[4];float after;};"
        "flat out %s result;flat out uint valid;void main(){int i=gl_VertexID;result=abs(values[i]);"
        "valid=(before==0.25 && after==0.75 && %s)?1u:0u;gl_Position=vec4(0);}",
        fp64_array_modes[glc_variant], fp64_array_modes[glc_variant], comparison);
    const char *varyings[] = {"result", "valid", "gl_SkipComponents1"};
    GLuint vao, program = fp64_array_program(source, varyings, 3, &vao);
    const char *names[] = {"before", "values[0]", "after"};
    GLuint indices[3]; GLint offsets[3], stride = 0, block_size = 0, type = 0;
    glGetUniformIndices(program, 3, names, indices);
    glGetActiveUniformsiv(program, 3, indices, GL_UNIFORM_OFFSET, offsets);
    glGetActiveUniformsiv(program, 1, indices + 1, GL_UNIFORM_ARRAY_STRIDE, &stride);
    glGetActiveUniformsiv(program, 1, indices + 1, GL_UNIFORM_TYPE, &type);
    GLuint block = glGetUniformBlockIndex(program, "Params");
    glGetActiveUniformBlockiv(program, block, GL_UNIFORM_BLOCK_DATA_SIZE, &block_size);
    GLenum types[] = {GL_DOUBLE, GL_DOUBLE_VEC2, GL_DOUBLE_VEC3, GL_DOUBLE_VEC4};
    int expected_stride = lanes <= 2 ? 16 : 32;
    if (stride != expected_stride || type != (GLint)types[glc_variant] || offsets[0] != 0 ||
        offsets[1] != expected_stride || offsets[2] != expected_stride * 5 || block_size > 256 || block_size <= offsets[2])
        glc_fail("Exact std140 reflection: lanes=%d offsets=%d/%d/%d stride=%d type=%x size=%d",
                 lanes, offsets[0], offsets[1], offsets[2], stride, type, block_size);
    unsigned char storage[256]; memset(storage, 0xa5, sizeof storage);
    float before = .25f, after = .75f;
    memcpy(storage + offsets[0], &before, 4); memcpy(storage + offsets[2], &after, 4);
    double expected[4][4];
    for (int vertex = 0; vertex < 4; ++vertex) for (int lane = 0; lane < lanes; ++lane) {
        double value = -(0x1.0000000000001p0 + vertex + lane * .25);
        if (vertex == 0 && lane == 0) value = -0.0;
        memcpy(storage + offsets[1] + vertex * stride + lane * 8, &value, 8);
        expected[vertex][lane] = fabs(value);
    }
    GLuint uniform;
    glGenBuffers(1, &uniform); glBindBuffer(GL_UNIFORM_BUFFER, uniform);
    glBufferData(GL_UNIFORM_BUFFER, block_size, storage, GL_STATIC_DRAW);
    glUniformBlockBinding(program, block, 0); glBindBufferBase(GL_UNIFORM_BUFFER, 0, uniform);
    int record = lanes * 8 + 8;
    GLuint capture = fp64_array_capture((size_t)record * 4, 4);
    unsigned char output[160];
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, record * 4, output);
    for (int vertex = 0; vertex < 4; ++vertex) {
        uint32_t valid; memcpy(&valid, output + vertex * record + lanes * 8, 4);
        if (memcmp(output + vertex * record, expected[vertex], lanes * 8) || valid != 1)
            glc_fail("Exact std140 value/comparison mismatch: lanes=%d vertex=%d valid=%u", lanes, vertex, valid);
    }
    GLenum error = glGetError();
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, 0); glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
    glDeleteBuffers(1, &uniform); glDeleteBuffers(1, &capture);
    glUseProgram(0); glDeleteProgram(program); glDeleteVertexArrays(1, &vao);
    if (error) glc_fail("Exact std140 array API error: %x", error);
    glClearColor(.25f, .5f, .75f, 1); glClear(GL_COLOR_BUFFER_BIT);
}

static const char *const fp64_integer_modes[] = {"signed_scalar", "signed_vec2", "signed_vec3", "signed_vec4",
                                               "unsigned_scalar", "unsigned_vec2", "unsigned_vec3", "unsigned_vec4"};
GLC_CASE_VARIANTS(core_fp64_integer_conversion, fp64_integer_modes, .profile = GLC_CORE)
{
    int lanes = glc_variant % 4 + 1;
    bool is_unsigned = glc_variant >= 4;
    char type[16], input_type[16], source[1024];
    if (lanes == 1) { strcpy(type, "double"); strcpy(input_type, is_unsigned ? "uint" : "int"); }
    else { snprintf(type, sizeof type, "dvec%d", lanes); snprintf(input_type, sizeof input_type, "%svec%d", is_unsigned ? "u" : "i", lanes); }
    snprintf(source, sizeof source, "#version 410 core\nuniform %s values[8];flat out %s result;"
        "void main(){result=%s(values[gl_VertexID]);gl_Position=vec4(0);}", input_type, type, type);
    const char *varying = "result";
    GLuint vao, program = fp64_array_program(source, &varying, 1, &vao);
    const int32_t signed_values[] = {INT32_MIN, -16777217, -1, 0, 1, 16777217, INT32_MAX, -1073741825};
    const uint32_t unsigned_values[] = {0, 1, 16777217u, UINT32_MAX, 2147483648u, 2147483649u, 33554431u, 1073741825u};
    int32_t signed_input[32]; uint32_t unsigned_input[32]; double expected[32];
    for (int i = 0; i < lanes * 8; ++i) {
        signed_input[i] = signed_values[i % 8]; unsigned_input[i] = unsigned_values[i % 8];
        expected[i] = is_unsigned ? (double)unsigned_input[i] : (double)signed_input[i];
    }
    GLint location = glGetUniformLocation(program, "values[0]");
    if (is_unsigned) {
        if (lanes == 1) glUniform1uiv(location, 8, unsigned_input);
        if (lanes == 2) glUniform2uiv(location, 8, unsigned_input);
        if (lanes == 3) glUniform3uiv(location, 8, unsigned_input);
        if (lanes == 4) glUniform4uiv(location, 8, unsigned_input);
    } else {
        if (lanes == 1) glUniform1iv(location, 8, signed_input);
        if (lanes == 2) glUniform2iv(location, 8, signed_input);
        if (lanes == 3) glUniform3iv(location, 8, signed_input);
        if (lanes == 4) glUniform4iv(location, 8, signed_input);
    }
    GLuint capture = fp64_array_capture((size_t)lanes * 8 * 8, 8);
    double output[32]; glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, lanes * 8 * 8, output);
    bool equal = memcmp(output, expected, lanes * 8 * 8) == 0;
    GLenum error = glGetError();
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0); glDeleteBuffers(1, &capture);
    glUseProgram(0); glDeleteProgram(program); glDeleteVertexArrays(1, &vao);
    if (!equal || error) glc_fail("Exact integer-to-double conversion mismatch: lanes=%d unsigned=%d error=%x", lanes, is_unsigned, error);
    glClearColor(.25f, .5f, .75f, 1); glClear(GL_COLOR_BUFFER_BIT);
}
