/* Check host and transform-feedback storage independently of FP64 arithmetic. */
#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

static GLuint storage_program(const char *source, const char *const *varyings, int count, GLenum mode)
{
    GLuint shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        glGetShaderInfoLog(shader, sizeof log, NULL, log);
        glc_fail("FP64 storage compile: %s", log);
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, shader);
    glTransformFeedbackVaryings(program, count, varyings, mode);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        glGetProgramInfoLog(program, sizeof log, NULL, log);
        glc_fail("FP64 storage link: %s", log);
    }
    glUseProgram(program);
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    return program;
}

static void storage_capture(int vertices)
{
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, vertices);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    GLenum error = glGetError();
    if (error) glc_fail("FP64 storage capture error: %x", error);
}

static void storage_equal(const void *actual, const void *expected, size_t size, const char *what)
{
    if (!memcmp(actual, expected, size)) return;
    const unsigned char *a = actual, *e = expected;
    size_t first = 0;
    while (first < size && a[first] == e[first]) ++first;
    char actual_hex[97] = {0}, expected_hex[97] = {0};
    size_t shown = size < 32 ? size : 32;
    for (size_t i = 0; i < shown; ++i) {
        snprintf(actual_hex + i * 3, 4, "%02x ", a[i]);
        snprintf(expected_hex + i * 3, 4, "%02x ", e[i]);
    }
    glc_fail("FP64 storage %s differs at byte %zu of %zu; actual [%s] expected [%s]",
             what, first, size, actual_hex, expected_hex);
}

static void storage_success(void)
{
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLC_CASE(core_fp64_array_and_mixed_uniform_storage, .profile = GLC_CORE)
{
    const char *varyings[] = {"d0", "d1", "f0", "f1"};
    GLuint program = storage_program("#version 410 core\n"
        "uniform float before; uniform dvec3 values[2]; uniform float after;\n"
        "flat out dvec3 d0, d1; out float f0, f1;\n"
        "void main(){ d0=values[0]; d1=values[1]; f0=before; f1=after; gl_Position=vec4(0); }",
        varyings, 4, GL_INTERLEAVED_ATTRIBS);
    double values[6] = {0x1.0000000000001p0, -16777217.125, -0.0, 0x1p-1074, 0x1p53, -0x1.fffffffffffffp1023};
    GLint first = glGetUniformLocation(program, "values[0]");
    GLint second = glGetUniformLocation(program, "values[1]");
    glUniform1f(glGetUniformLocation(program, "before"), 0.375f);
    glUniform3dv(first, 2, values);
    glUniform1f(glGetUniformLocation(program, "after"), 0.625f);
    /* Exercise the direct-program setter and an individual array element. */
    glProgramUniform3dv(program, second, 1, values + 3);
    double queried[5] = {0, 0, 0, 1234.5, -1234.5};
    glGetUniformdv(program, second, queried);
    storage_equal(queried, values + 3, 24, "array getter");
    if (queried[3] != 1234.5 || queried[4] != -1234.5) glc_fail("FP64 array getter overwrote its result");
    GLuint buffer;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 112, NULL, GL_STREAM_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
    storage_capture(2);
    unsigned char captured[112];
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof captured, captured);
    const float floats[] = {0.375f, 0.625f};
    for (int vertex = 0; vertex < 2; ++vertex) {
        storage_equal(captured + vertex * 56, values, 48, "array feedback");
        storage_equal(captured + vertex * 56 + 48, floats, 8, "mixed float globals");
    }
    storage_success();
}

static const char *const storage_modes[] = {"interleaved", "separate"};
GLC_CASE_VARIANTS(core_fp64_feedback_storage_alignment, storage_modes, .profile = GLC_CORE)
{
    const char *separate[] = {"f0", "d0", "f1"};
    /* FP64 TF does not insert alignment padding automatically. Explicit
       skips align d0 and make the interleaved per-vertex stride a multiple of eight. */
    const char *interleaved[] = {"f0", "gl_SkipComponents1", "d0", "f1", "gl_SkipComponents1"};
    GLuint program = storage_program("#version 410 core\n"
        "uniform double input_value; out float f0, f1; flat out double d0;\n"
        "void main(){ f0=0.25; d0=input_value; f1=0.75; gl_Position=vec4(0); }",
        glc_variant ? separate : interleaved, glc_variant ? 3 : 5,
        glc_variant ? GL_SEPARATE_ATTRIBS : GL_INTERLEAVED_ATTRIBS);
    const double value = -0x1.0000000000001p0;
    glUniform1d(glGetUniformLocation(program, "input_value"), value);
    GLuint buffers[3];
    int count = glc_variant ? 3 : 1;
    glGenBuffers(count, buffers);
    unsigned char guard[64];
    memset(guard, 0xa5, sizeof guard);
    for (int i = 0; i < count; ++i) {
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[i]);
        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, sizeof guard, guard, GL_STREAM_READ);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, (GLuint)i, buffers[i]);
    }
    storage_capture(2);
    const float floats[] = {0.25f, 0.75f};
    for (int i = 0; i < count; ++i) {
        unsigned char captured[64];
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[i]);
        glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof captured, captured);
        if (!glc_variant) {
            for (int vertex = 0; vertex < 2; ++vertex) {
                storage_equal(captured + vertex * 24, floats, 4, "leading float");
                storage_equal(captured + vertex * 24 + 8, &value, 8, "aligned double");
                storage_equal(captured + vertex * 24 + 16, floats + 1, 4, "trailing float");
            }
            storage_equal(captured + 48, guard + 48, 16, "interleaved buffer guard");
        } else {
            int stride = i == 1 ? 8 : 4;
            const void *expected = i == 1 ? (const void *)&value : (const void *)(floats + (i == 2));
            for (int vertex = 0; vertex < 2; ++vertex)
                storage_equal(captured + vertex * stride, expected, (size_t)stride, "separate feedback");
            storage_equal(captured + stride * 2, guard + stride * 2, (size_t)(64 - stride * 2), "separate buffer guard");
        }
    }
    storage_success();
}

GLC_CASE(core_fp64_vector_compare_and_bounds_storage, .profile = GLC_CORE)
{
    const char *varyings[] = {"eq", "mn", "mx", "cl"};
    GLuint program = storage_program("#version 410 core\n"
        "uniform dvec4 a,b,low,high; flat out uvec2 eq; flat out dvec4 mn,mx,cl;\n"
        "void main(){ eq=uvec2(equal(a.xy,b.xy)); mn=min(a,b); mx=max(a,b);"
        " cl=clamp(a,low,high); gl_Position=vec4(0); }", varyings, 4, GL_INTERLEAVED_ATTRIBS);
    const double a[] = {-0x1.0000000000001p0, 16777217.125, 9007199254740994.0, -9007199254740994.0};
    const double b[] = {-0x1.0000000000001p0, 16777217.25, 9007199254740992.0, -9007199254740992.0};
    const double low[] = {-2.0, 16777217.1875, 9007199254740992.0, -9007199254740992.0};
    const double high[] = {-1.0, 16777217.5, 9007199254740996.0, -9007199254740990.0};
    glUniform4dv(glGetUniformLocation(program, "a"), 1, a);
    glUniform4dv(glGetUniformLocation(program, "b"), 1, b);
    glUniform4dv(glGetUniformLocation(program, "low"), 1, low);
    glUniform4dv(glGetUniformLocation(program, "high"), 1, high);
    GLuint buffer;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 104, NULL, GL_STREAM_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
    storage_capture(1);
    unsigned char captured[104];
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof captured, captured);
    const GLuint expected_equal[] = {1, 0};
    storage_equal(captured, expected_equal, 8, "vector equality");
    double expected[12];
    for (int i = 0; i < 4; ++i) {
        expected[i] = a[i] < b[i] ? a[i] : b[i];
        expected[4 + i] = a[i] > b[i] ? a[i] : b[i];
        expected[8 + i] = a[i] < low[i] ? low[i] : a[i] > high[i] ? high[i] : a[i];
    }
    storage_equal(captured + 8, expected, 96, "vector min max clamp");
    storage_success();
}

static void storage_error(GLenum expected, const char *what)
{
    GLenum actual = glGetError();
    if (actual != expected) glc_fail("FP64 storage %s error: %x != %x", what, actual, expected);
}

GLC_CASE(core_fp64_uniform_storage_validation, .profile = GLC_CORE)
{
    const char *varyings[] = {"result"};
    GLuint program = storage_program("#version 410 core\nuniform double value; flat out double result;\n"
        "void main(){ result=value; gl_Position=vec4(0); }", varyings, 1, GL_INTERLEAVED_ATTRIBS);
    GLint location = glGetUniformLocation(program, "value");
    const double inputs[] = {0x1.0000000000001p0, 0x1.0000000000002p0};
    glUniform1d(location, inputs[0]);
    storage_error(GL_NO_ERROR, "initial scalar setter");
    glUniform1dv(location, 2, inputs);
    storage_error(GL_INVALID_OPERATION, "nonarray count");
    glUniform1dv(location, -1, inputs);
    storage_error(GL_INVALID_VALUE, "negative count");
    glUniform2dv(location, 1, inputs);
    storage_error(GL_INVALID_OPERATION, "component count");
    glUniform1f(location, 2.0f);
    storage_error(GL_INVALID_OPERATION, "float setter for double");
    glUniform1dv(location, 0, NULL);
    storage_error(GL_NO_ERROR, "zero count");
    glUniform1dv(-1, 1, inputs);
    storage_error(GL_NO_ERROR, "inactive location");
    double queried[] = {0, 1234.5};
    glGetUniformdv(program, location, queried);
    storage_equal(queried, inputs, 8, "unchanged uniform after invalid setters");
    if (queried[1] != 1234.5) glc_fail("FP64 scalar getter overwrote its result");
    storage_success();
}
