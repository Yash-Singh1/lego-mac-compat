#include "glc_gl_core.h"
#include "glcompare.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static GLuint fp64_program(const char *source, const char *const *varyings, int count)
{
    GLuint shader = glCreateShader(GL_VERTEX_SHADER), program = glCreateProgram();
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glAttachShader(program, shader);
    glTransformFeedbackVaryings(program, count, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(program);
    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[2048] = {0};
        glGetProgramInfoLog(program, sizeof log, NULL, log);
        glc_fail("FP64 link: %s", log);
    }
    glUseProgram(program);
    GLuint vao, buffer;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 256, NULL, GL_STREAM_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
    return program;
}

static void fp64_capture(double *values, int count)
{
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, 1);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, (GLsizeiptr)count * sizeof *values, values);
}

static void fp64_equal(double actual, double expected, const char *operation, int element)
{
    uint64_t a, e;
    memcpy(&a, &actual, 8);
    memcpy(&e, &expected, 8);
    if (a != e) glc_fail("%s[%d]: %016llx != %016llx", operation, element,
                        (unsigned long long)a, (unsigned long long)e);
}

GLC_CASE(core_fp64_exact_abs, .profile = GLC_CORE)
{
    const char *varyings[] = {"result"};
    GLuint program = fp64_program("#version 410 core\nuniform dvec4 input_value; flat out dvec4 result;\n"
                                 "void main(){ result = abs(input_value); gl_Position = vec4(0); }", varyings, 1);
    const double inputs[][4] = {
        {-9007199254740994.0, -16777217.125, -0x1.0000000000001p0, -0.0},
        {-0x1p-1022, -0x1p-1074, -0x1.fffffffffffffp1023, -0x1.0000000000001p-1022},
    };
    GLint location = glGetUniformLocation(program, "input_value");
    for (int set = 0; set < 2; ++set) {
        glUniform4dv(location, 1, inputs[set]);
        double uniform[4], actual[4];
        glGetUniformdv(program, location, uniform);
        for (int i = 0; i < 4; ++i) fp64_equal(uniform[i], inputs[set][i], "uniform", i);
        fp64_capture(actual, 4);
        for (int i = 0; i < 4; ++i) fp64_equal(actual[i], fabs(inputs[set][i]), "abs", i);
    }
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLC_CASE(core_fp64_exact_rounding, .profile = GLC_CORE)
{
    const char *varyings[] = {"r_floor", "r_ceil", "r_trunc", "r_round"};
    GLuint program = fp64_program("#version 410 core\nuniform dvec4 input_value;\n"
        "flat out dvec4 r_floor, r_ceil, r_trunc, r_round; void main(){\n"
        "r_floor=floor(input_value); r_ceil=ceil(input_value); r_trunc=trunc(input_value);"
        "r_round=round(input_value); gl_Position=vec4(0); }", varyings, 4);
    const double inputs[][4] = {{16777217.25, -16777217.25, 4503599627370495.5, -0.25},
                                {-2.5, -1.5, 0.5, 1.5}, {0x1p-1074, -0x1p-1074, 0x1p53, -0x1p53}};
    GLint location = glGetUniformLocation(program, "input_value");
    double (*functions[])(double) = {floor, ceil, trunc, round};
    for (int set = 0; set < 3; ++set) {
        glUniform4dv(location, 1, inputs[set]);
        double actual[16];
        fp64_capture(actual, 16);
        for (int op = 0; op < 4; ++op)
            for (int i = 0; i < 4; ++i) fp64_equal(actual[op * 4 + i], functions[op](inputs[set][i]), "rounding", op * 4 + i);
    }
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLC_CASE(core_fp64_exact_pack, .profile = GLC_CORE)
{
    const char *varyings[] = {"result"};
    GLuint program = fp64_program("#version 410 core\nuniform uvec2 words; flat out double result;\n"
        "void main(){ result=packDouble2x32(words); gl_Position=vec4(0); }", varyings, 1);
    const uint64_t bits[] = {0x3ff0000000000001ull, 0x8000000000000000ull, 0x0000000000000001ull,
                             0x7fefffffffffffffull, 0xbff123456789abcdull};
    GLint location = glGetUniformLocation(program, "words");
    for (int i = 0; i < (int)(sizeof bits / sizeof *bits); ++i) {
        GLuint words[2] = {(GLuint)bits[i], (GLuint)(bits[i] >> 32)};
        glUniform2uiv(location, 1, words);
        double actual, expected;
        memcpy(&expected, &bits[i], sizeof expected);
        fp64_capture(&actual, 1);
        fp64_equal(actual, expected, "pack", i);
    }
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLC_CASE(core_fp64_exact_step, .profile = GLC_CORE)
{
    const char *varyings[] = {"vector_result", "scalar_result"};
    GLuint program = fp64_program("#version 410 core\n"
        "uniform dvec4 edges, values; uniform double scalar_edge;\n"
        "flat out dvec4 vector_result, scalar_result; void main(){\n"
        "vector_result=step(edges,values); scalar_result=step(scalar_edge,values); gl_Position=vec4(0); }",
        varyings, 2);
    const double values[][4] = {{16777217.125, 16777217.0, -0.0, 0.0},
                                {-0x1p-1074, 0x1p-1074, -0x1.fffffffffffffp1023, 0x1.fffffffffffffp1023}};
    const double edges[][4] = {{16777217.0, 16777217.125, 0.0, -0.0}, {0.0, 0.0, -0x1p1023, 0x1p1023}};
    for (int set = 0; set < 2; ++set) {
        glUniform4dv(glGetUniformLocation(program, "values"), 1, values[set]);
        glUniform4dv(glGetUniformLocation(program, "edges"), 1, edges[set]);
        glUniform1d(glGetUniformLocation(program, "scalar_edge"), edges[set][0]);
        double actual[8];
        fp64_capture(actual, 8);
        for (int i = 0; i < 4; ++i) {
            fp64_equal(actual[i], values[set][i] < edges[set][i] ? 0.0 : 1.0, "step vector", i);
            fp64_equal(actual[i + 4], values[set][i] < edges[set][0] ? 0.0 : 1.0, "step scalar", i);
        }
    }
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLC_CASE(core_fp64_exact_frexp, .profile = GLC_CORE)
{
    const char *varyings[] = {"fraction", "exponent"};
    GLuint program = fp64_program("#version 410 core\n"
        "uniform dvec4 values; flat out dvec4 fraction; flat out ivec4 exponent;\n"
        "void main(){ fraction=frexp(values,exponent); gl_Position=vec4(0); }", varyings, 2);
    const double values[][4] = {{0x1.0000000000001p0, -16777217.125, -0.0, 0.0},
                                {0x1p-1074, -0x1p-1022, 0x1.fffffffffffffp1023, -0x1.0000000000001p-1022}};
    for (int set = 0; set < 2; ++set) {
        glUniform4dv(glGetUniformLocation(program, "values"), 1, values[set]);
        double actual[6];
        GLint exponents[4];
        fp64_capture(actual, 6);
        memcpy(exponents, (const unsigned char *)actual + 32, sizeof exponents);
        for (int i = 0; i < 4; ++i) {
            int expected_exponent;
            double expected = frexp(values[set][i], &expected_exponent);
            fp64_equal(actual[i], expected, "frexp fraction", i);
            if (exponents[i] != expected_exponent)
                glc_fail("frexp exponent[%d]: %d != %d", i, exponents[i], expected_exponent);
        }
    }
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLC_CASE(core_fp64_exact_ldexp, .profile = GLC_CORE)
{
    const char *varyings[] = {"result"};
    GLuint program = fp64_program("#version 410 core\n"
        "uniform dvec4 values; uniform ivec4 exponents; flat out dvec4 result;\n"
        "void main(){ result=ldexp(values,exponents); gl_Position=vec4(0); }", varyings, 1);
    const double values[][4] = {{0x1.0000000000001p0, -16777217.125, -0.0, 0.0},
                                {0x1p-1074, -0x1p-1022, 0x1.fffffffffffffp1023, -0x1.0000000000001p-1022}};
    const GLint exponents[][4] = {{512, -512, 100, -100}, {1022, 1022, -1023, 1000}};
    for (int set = 0; set < 2; ++set) {
        glUniform4dv(glGetUniformLocation(program, "values"), 1, values[set]);
        glUniform4iv(glGetUniformLocation(program, "exponents"), 1, exponents[set]);
        double actual[4];
        fp64_capture(actual, 4);
        for (int i = 0; i < 4; ++i)
            fp64_equal(actual[i], ldexp(values[set][i], exponents[set][i]), "ldexp", i);
    }
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLC_CASE(core_fp64_exact_modf, .profile = GLC_CORE)
{
    const char *varyings[] = {"fraction", "integral"};
    GLuint program = fp64_program("#version 410 core\n"
        "uniform dvec4 values; flat out dvec4 fraction, integral;\n"
        "void main(){ fraction=modf(values,integral); gl_Position=vec4(0); }", varyings, 2);
    const double values[][4] = {{16777217.125, -16777217.125, 4503599627370495.5, -0.25},
                                {0x1p-1074, -0x1p-1074, -0.0, -0x1.fffffffffffffp1023}};
    for (int set = 0; set < 2; ++set) {
        glUniform4dv(glGetUniformLocation(program, "values"), 1, values[set]);
        double actual[8];
        fp64_capture(actual, 8);
        for (int i = 0; i < 4; ++i) {
            double integral;
            double fraction = modf(values[set][i], &integral);
            /* Apple's GLSL modf computes a positive fractional zero for
               negative integral inputs, including -0. Its integral output
               still retains the sign. */
            if (fraction == 0.0) fraction = 0.0;
            fp64_equal(actual[i], fraction, "modf fraction", i);
            fp64_equal(actual[i + 4], integral, "modf integral", i);
        }
    }
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLC_CASE(core_fp64_exact_arithmetic, .profile = GLC_CORE)
{
    const char *varyings[] = {"sum", "difference", "product"};
    GLuint program = fp64_program("#version 410 core\n"
        "uniform dvec4 a,b; flat out dvec4 sum,difference,product;\n"
        "void main(){sum=a+b;difference=a-b;product=a*b;gl_Position=vec4(0);}", varyings, 3);
    const double a[][4] = {
        {9007199254740994.0, 16777217.125, 0x1.0000000000001p0, -0x1.0000000000001p0},
        {0x1p-1074, -0x1p-1074, 0x1.fffffffffffffp-1023, 0x1p-1022},
        {-0.0, 0.0, -0.0, 0x1.fffffffffffffp1023},
        {0x1.0000000000001p500, 0x1.fffffffffffffp-500, -0x1p53, 0x1.0000000000001p0},
    };
    const double b[][4] = {
        {1.0, -16777217.0, 0x1.fffffffffffffp-1, -0x1.0000000000001p0},
        {2.0, 0.5, 0.5, -0x1.ffffffffffffep-1023},
        {-0.0, -0.0, 1.0, 0.5},
        {0x1.fffffffffffffp-500, 0x1.0000000000001p500, -1.0, -1.0},
    };
    for (int set = 0; set < 4; ++set) {
        glUniform4dv(glGetUniformLocation(program, "a"), 1, a[set]);
        glUniform4dv(glGetUniformLocation(program, "b"), 1, b[set]);
        double actual[12];
        fp64_capture(actual, 12);
        for (int i = 0; i < 4; ++i) {
            fp64_equal(actual[i], a[set][i] + b[set][i], "add", i);
            fp64_equal(actual[i + 4], a[set][i] - b[set][i], "subtract", i);
            fp64_equal(actual[i + 8], a[set][i] * b[set][i], "multiply", i);
        }
    }
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLC_CASE(core_fp64_exact_division_and_sqrt, .profile = GLC_CORE)
{
    const char *varyings[] = {"quotient", "root", "inverse_root"};
    GLuint program = fp64_program("#version 410 core\n"
        "uniform dvec4 a,b; flat out dvec4 quotient,root,inverse_root;\n"
        "void main(){quotient=a/b;root=sqrt(abs(a));inverse_root=inversesqrt(abs(a));gl_Position=vec4(0);}",
        varyings, 3);
    const double a[][4] = {{2.0, 0x1.0000000000001p0, -16777217.125, 0x1.fffffffffffffp1023},
                           {0x1p-1074, -0x1p-1074, 0x1p-1022, 0x1.fffffffffffffp-1}};
    const double b[][4] = {{3.0, 0x1.fffffffffffffp-1, 7.0, 0x1p1023}, {0.5, 2.0, -0x1p-52, 0x1.0000000000001p0}};
    for (int set = 0; set < 2; ++set) {
        glUniform4dv(glGetUniformLocation(program, "a"), 1, a[set]);
        glUniform4dv(glGetUniformLocation(program, "b"), 1, b[set]);
        double actual[12];
        fp64_capture(actual, 12);
        for (int i = 0; i < 4; ++i) {
            double root = sqrt(fabs(a[set][i]));
            fp64_equal(actual[i], a[set][i] / b[set][i], "divide", i);
            fp64_equal(actual[i + 4], root, "sqrt", i);
            fp64_equal(actual[i + 8], 1.0 / root, "inversesqrt", i);
        }
    }
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLC_CASE(core_fp64_exact_fma, .profile = GLC_CORE)
{
    const char *varyings[] = {"result"};
    GLuint program = fp64_program("#version 410 core\n"
        "uniform dvec4 a,b,c; flat out dvec4 result;\n"
        "void main(){result=fma(a,b,c);gl_Position=vec4(0);}", varyings, 1);
    const double a[] = {0x1.0000000000001p0, 0x1.fffffffffffffp1023, 0x1p-1022, 9007199254740994.0};
    const double b[] = {0x1.ffffffffffffep-1, 2.0, 0.5, 0.5};
    const double c[] = {-1.0, -0x1.fffffffffffffp1023, -0x1p-1023, -0x1p52};
    glUniform4dv(glGetUniformLocation(program, "a"), 1, a);
    glUniform4dv(glGetUniformLocation(program, "b"), 1, b);
    glUniform4dv(glGetUniformLocation(program, "c"), 1, c);
    double actual[4];
    fp64_capture(actual, 4);
    for (int i = 0; i < 4; ++i) fp64_equal(actual[i], fma(a[i], b[i], c[i]), "fma", i);
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void fp64_matrix_uniform(GLint location, int columns, int rows, GLboolean transpose, const double *values)
{
    switch (columns * 10 + rows) {
    case 22: glUniformMatrix2dv(location, 1, transpose, values); break;
    case 23: glUniformMatrix2x3dv(location, 1, transpose, values); break;
    case 24: glUniformMatrix2x4dv(location, 1, transpose, values); break;
    case 32: glUniformMatrix3x2dv(location, 1, transpose, values); break;
    case 33: glUniformMatrix3dv(location, 1, transpose, values); break;
    case 34: glUniformMatrix3x4dv(location, 1, transpose, values); break;
    case 42: glUniformMatrix4x2dv(location, 1, transpose, values); break;
    case 43: glUniformMatrix4x3dv(location, 1, transpose, values); break;
    case 44: glUniformMatrix4dv(location, 1, transpose, values); break;
    }
}

static void fp64_vector_uniform(GLint location, int count, const double *values)
{
    if (count == 2) glUniform2dv(location, 1, values);
    if (count == 3) glUniform3dv(location, 1, values);
    if (count == 4) glUniform4dv(location, 1, values);
}

static const char *const fp64_matrix_shapes[] = {"2x2", "2x3", "2x4", "3x2", "3x3", "3x4", "4x2", "4x3", "4x4"};
GLC_CASE_VARIANTS(core_fp64_exact_matrix_products, fp64_matrix_shapes, .profile = GLC_CORE)
{
    int columns = glc_variant / 3 + 2, rows = glc_variant % 3 + 2, elements = columns * rows;
    char source[1024];
    snprintf(source, sizeof source, "#version 410 core\n"
        "uniform dmat%dx%d a,b; uniform dvec%d column_value; uniform dvec%d row_value;\n"
        "flat out dmat%dx%d product, outer_result; void main(){product=matrixCompMult(a,b);"
        "outer_result=outerProduct(column_value,row_value);gl_Position=vec4(0);}",
        columns, rows, rows, columns, columns, rows);
    const char *varyings[] = {"product", "outer_result"};
    GLuint program = fp64_program(source, varyings, 2);
    double a[16], b[16], transposed[16];
    const double column_value[] = {0x1.0000000000001p0, -16777217.125, 0x1p-1022, 9007199254740994.0};
    const double row_value[] = {0.5, 1.5, -2.0, 0x1.0000000000001p0};
    for (int c = 0; c < columns; ++c)
        for (int r = 0; r < rows; ++r) {
            int i = c * rows + r;
            a[i] = 1.0 + (i + 1) * 0x1p-40;
            b[i] = (i + 1) * 0.125;
            transposed[r * columns + c] = b[i];
        }
    GLint a_location = glGetUniformLocation(program, "a"), b_location = glGetUniformLocation(program, "b");
    fp64_matrix_uniform(a_location, columns, rows, GL_FALSE, a);
    fp64_matrix_uniform(b_location, columns, rows, GL_TRUE, transposed);
    fp64_vector_uniform(glGetUniformLocation(program, "column_value"), rows, column_value);
    fp64_vector_uniform(glGetUniformLocation(program, "row_value"), columns, row_value);
    double queried[16], actual[32];
    glGetUniformdv(program, b_location, queried);
    for (int i = 0; i < elements; ++i) fp64_equal(queried[i], b[i], "matrix uniform", i);
    fp64_capture(actual, 2 * elements);
    for (int c = 0; c < columns; ++c)
        for (int r = 0; r < rows; ++r) {
            int i = c * rows + r;
            fp64_equal(actual[i], a[i] * b[i], "matrixCompMult", i);
            fp64_equal(actual[elements + i], column_value[r] * row_value[c], "outerProduct", i);
        }
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLC_CASE(core_fp64_exact_matrix_inverse, .profile = GLC_CORE)
{
    const char *varyings[] = {"result", "det"};
    GLuint program = fp64_program("#version 410 core\nuniform dmat2 a;flat out dmat2 result;flat out double det;"
        "void main(){result=inverse(a);det=determinant(a);gl_Position=vec4(0);}", varyings, 2);
    const double input[] = {16777217.0, 1.0, 16777216.0, 1.0};
    const double expected[] = {1.0, -1.0, -16777216.0, 16777217.0, 1.0};
    glUniformMatrix2dv(glGetUniformLocation(program, "a"), 1, GL_FALSE, input);
    double actual[5];
    fp64_capture(actual, 5);
    for (int i = 0; i < 5; ++i) fp64_equal(actual[i], expected[i], "inverse/determinant", i);
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
