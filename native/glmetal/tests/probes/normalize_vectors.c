#include "glc_gl_core.h"
#include "glcompare.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *names[] = {"vec2", "vec3", "vec4", "fragment_vec2", "fragment_vec3", "fragment_vec4"};
static const float vectors[][4] = {
    {.34375f, .05390625f, .6f, .125f},
    {.59375f, -.05390625f, .6f, .125f},
    {.59375f, .05390625f, .6f, -.125f},
    {.28125f, .05390625f, .6f, .125f},
    {.65625f, -.05390625f, .6f, .125f},
    {.59375f, -.05859375f, .6f, .125f},
    {.59375f, -.04921875f, .6f, .125f},
    {1, 1, 1, 1}, {1, 0, 1, 0}, {-1, 1, -1, 1},
    {.3f, .4f, .6f, .8f}, {0x1.fp-4f, 0x1.2p-1f, 0x1.5p-5f, .5f},
    {1e10f, 1e-10f, 3e9f, -2e8f}, {1e-10f, 3e-10f, -2e-10f, 4e-10f},
    {1.00000011920928955078125f, .000244140625f, 1, .0001220703125f},
    {.000244140625f, 1, .0001220703125f, 1.00000011920928955078125f},
    {1, .0001220703125f, 1.00000011920928955078125f, .000244140625f},
    {-.0f, 1, -.0f, -1}
};
struct captured { float original[4], value[4]; GLint tag; };

static uint32_t bits(float value)
{
    uint32_t result;
    memcpy(&result, &value, sizeof result);
    return result;
}

static void fragment_normalize(int lanes)
{
    enum { count = sizeof vectors / sizeof vectors[0] };
    const char *vertex = "#version 410 core\nvoid main(){"
        "vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
        "gl_Position=vec4(p*2-1,0,1);}";
    char fragment[512];
    snprintf(fragment, sizeof fragment,
        "#version 410 core\nuniform vec4 inputs[%d];out vec4 result;"
        "void main(){result=vec4(0);result.%s=normalize(vec%d(inputs[int(gl_FragCoord.x)]));}",
        count, lanes == 2 ? "xy" : lanes == 3 ? "xyz" : "xyzw", lanes);
    GLint draw_framebuffer, read_framebuffer, viewport[4];
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
    glGetIntegerv(GL_VIEWPORT, viewport);
    GLuint program = glc_program(vertex, fragment, NULL);
    GLuint vao, texture, framebuffer;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, count, 1, 0, GL_RGBA, GL_FLOAT, NULL);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        glc_fail("Fragment normalize framebuffer incomplete");
    glViewport(0, 0, count, 1);
    glUseProgram(program);
    GLint location = glGetUniformLocation(program, "inputs[0]");
    if (location < 0) glc_fail("Fragment normalize inputs inactive");
    glUniform4fv(location, count, &vectors[0][0]);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    float output[count][4];
    glReadPixels(0, 0, count, 1, GL_RGBA, GL_FLOAT, output);
    for (int i = 0; i < count; ++i) {
        fprintf(stderr, "normalize_fragment_vec%d vector=%d input=%08x,%08x,%08x,%08x\n",
            lanes, i, bits(vectors[i][0]), bits(vectors[i][1]), bits(vectors[i][2]), bits(vectors[i][3]));
        for (int j = 0; j < lanes; ++j)
            fprintf(stderr, "  component=%d actual=%08x hex=%a\n", j, bits(output[i][j]), output[i][j]);
    }
    if (glGetError()) glc_fail("Fragment normalize diagnostic GL error");
    glUseProgram(0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_framebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_framebuffer);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
    usleep(50000);
    glClearColor(.25f, .5f, .75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLC_CASE_VARIANTS(driver_normalize_vectors, names, .profile = GLC_CORE)
{
    int lanes = glc_variant % 3 + 2;
    if (glc_variant >= 3) {
        fragment_normalize(lanes);
        return;
    }
    char source[512];
    snprintf(source, sizeof source, "#version 410 core\nlayout(location=0)in vec4 input_value;"
        "out vec4 original;out vec4 value;flat out int tag;void main(){"
        "original=input_value;value=vec4(0);value.%s=normalize(vec%d(input_value));"
        "tag=gl_VertexID;gl_Position=vec4(0,0,0,1);}",
        lanes == 2 ? "xy" : lanes == 3 ? "xyz" : "xyzw", lanes);
    GLuint shader = glCreateShader(GL_VERTEX_SHADER);
    const char *pointer = source;
    glShaderSource(shader, 1, &pointer, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        glGetShaderInfoLog(shader, sizeof log, NULL, log);
        glc_fail("Normalize vector shader: %s", log);
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, shader);
    const char *varyings[] = {"original", "value", "tag"};
    glTransformFeedbackVaryings(program, 3, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        glGetProgramInfoLog(program, sizeof log, NULL, log);
        glc_fail("Normalize vector program: %s", log);
    }
    glUseProgram(program);
    GLuint vao, buffers[2], query;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(2, buffers);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof vectors, vectors, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, NULL);
    glEnableVertexAttribArray(0);
    struct captured output[sizeof vectors / sizeof vectors[0]];
    memset(output, 0xa5, sizeof output);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[1]);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, sizeof output, output, GL_STREAM_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffers[1]);
    glGenQueries(1, &query);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, query);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, sizeof vectors / sizeof vectors[0]);
    glEndTransformFeedback();
    glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
    GLuint count = 0;
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, &count);
    if (count != sizeof vectors / sizeof vectors[0]) glc_fail("Normalize feedback count differs");
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof output, output);
    for (GLuint i = 0; i < count; ++i) {
        if (output[i].tag != (GLint)i || memcmp(output[i].original, vectors[i], sizeof vectors[i]))
            glc_fail("Normalize feedback changed input or vertex order");
        float length2 = vectors[i][0] * vectors[i][0];
        for (int j = 1; j < lanes; ++j) {
            volatile float product = vectors[i][j] * vectors[i][j];
            length2 += product;
        }
        float inverse = (float)(1.0 / sqrt((double)length2));
        fprintf(stderr, "normalize_%s vector=%u input=%08x,%08x,%08x,%08x length2=%08x inverse=%08x\n",
            names[glc_variant], i, bits(vectors[i][0]), bits(vectors[i][1]), bits(vectors[i][2]), bits(vectors[i][3]),
            bits(length2), bits(inverse));
        for (int j = 0; j < lanes; ++j) {
            float candidate = vectors[i][j] * inverse;
            fprintf(stderr, "  component=%d actual=%08x hex=%a scalar_model=%08x model_hex=%a\n",
                j, bits(output[i].value[j]), output[i].value[j], bits(candidate), candidate);
        }
    }
    if (glGetError()) glc_fail("Normalize diagnostic GL error");
    glDisable(GL_RASTERIZER_DISCARD);
    glUseProgram(0);
    glDeleteQueries(1, &query);
    glDeleteBuffers(2, buffers);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
    glDeleteShader(shader);
    usleep(50000);
    glClearColor(.25f, .5f, .75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
