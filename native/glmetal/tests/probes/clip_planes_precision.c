#include "glc_gl_core.h"
#include "glcompare.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum { WORDS = 12, MAX_POLYGON = 8 };
struct clip_vertex { double word[WORDS]; };
static const char *const arithmetic_names[] = {"double", "float_split", "float_fused", "float_reciprocal"};

/* Volatile intermediates keep the split mode separate from contraction. */
static double clip_parameter(double a, double b, int mode)
{
    if (!mode) return a / (a - b);
    volatile float denominator = (float)a - (float)b;
    if (mode == 3) {
        volatile float reciprocal = 1.0f / denominator;
        volatile float result = (float)a * reciprocal;
        return result;
    }
    volatile float result = (float)a / denominator;
    return result;
}
static double clip_interpolate(double a, double b, double t, int mode)
{
    if (!mode) {
        volatile double difference = b - a;
        volatile double product = t * difference;
        volatile double result = a + product;
        return result;
    }
    volatile float difference = (float)b - (float)a;
    if (mode == 2) return fmaf((float)t, difference, (float)a);
    volatile float product = (float)t * difference;
    volatile float result = (float)a + product;
    return result;
}
static int clip_polygon(const float captured[3][WORDS], int order, int mode,
                        struct clip_vertex output[MAX_POLYGON])
{
    struct clip_vertex polygon[2][MAX_POLYGON];
    for (int v = 0; v < 3; ++v) for (int c = 0; c < WORDS; ++c) polygon[0][v].word[c] = captured[v][c];
    int count = 3, buffer = 0;
    for (int step = 0; step < 2; ++step) {
        int plane = order ? 1 - step : step, produced = 0, next = 1 - buffer;
        for (int i = 0; i < count; ++i) {
            int j = (i + 1) % count;
            struct clip_vertex *a = &polygon[buffer][i], *b = &polygon[buffer][j];
            double da = a->word[10 + plane], db = b->word[10 + plane];
            int inside_a = da >= 0, inside_b = db >= 0;
            if (inside_a) {
                if (produced >= MAX_POLYGON) glc_fail("Precision probe polygon overflow");
                polygon[next][produced++] = *a;
            }
            if (inside_a != inside_b) {
                if (produced >= MAX_POLYGON) glc_fail("Precision probe polygon overflow");
                double t = clip_parameter(da, db, mode);
                if (!isfinite(t) || t < 0 || t > 1) glc_fail("Precision probe invalid intersection");
                for (int c = 0; c < WORDS; ++c)
                    polygon[next][produced].word[c] = clip_interpolate(a->word[c], b->word[c], t, mode);
                ++produced;
            }
        }
        count = produced; buffer = next;
    }
    memcpy(output, polygon[buffer], (size_t)count * sizeof *output);
    return count;
}

static const char *const clip_orders[] = {
    "abc_planes01", "bca_planes01", "cab_planes01",
    "abc_planes10", "bca_planes10", "cab_planes10",
    "abc_plane0_only", "bca_plane0_only", "cab_plane0_only"
};
GLC_CASE_VARIANTS(core_clip_planes_precision, clip_orders, .profile = GLC_CORE)
{
    const struct timespec pause = {0, 50000000};
    const float original_source[3][8] = {
        {-.9f,-.8f,-.5f,1,.9f,.2f,.1f,.75f},
        {.8f,-.9f,.2f,1,.1f,.8f,.3f,.5f},
        {.1f,.9f,.6f,1,.2f,.1f,.9f,1}
    };
    float source[3][8];
    for (int v = 0; v < 3; ++v)
        memcpy(source[v], original_source[(v + glc_variant % 3) % 3], sizeof source[v]);
    fprintf(stderr, "ClipFixture %s\n", clip_orders[glc_variant]);
    const char *fragment = "#version 410 core\nin vec4 shade;in vec2 coord;out vec4 frag;"
        "void main(){frag=vec4(shade.rgb*(.75+.25*coord.x),shade.a);}";
    GLuint native = glc_program(
        "#version 410 core\nlayout(location=0) in vec4 position;layout(location=1) in vec4 colour;"
        "uniform vec4 planes[2];out vec4 shade;out vec2 coord;out vec4 rawPosition;out vec2 rawDistances;"
        "void main(){gl_Position=position;rawPosition=position;"
        "shade=vec4(colour.rgb*colour.rgb+.05*colour.aaa,colour.a);coord=position.xy*.25+.5;"
        "rawDistances=vec2(dot(position,planes[0]),dot(position,planes[1]));"
        "gl_ClipDistance[0]=rawDistances.x;gl_ClipDistance[1]=rawDistances.y;}", fragment, NULL);
    const char *varyings[] = {"rawPosition", "shade", "coord", "rawDistances"};
    glTransformFeedbackVaryings(native, 4, varyings, GL_INTERLEAVED_ATTRIBS); glLinkProgram(native);
    GLint linked; glGetProgramiv(native, GL_LINK_STATUS, &linked);
    if (!linked) glc_fail("Precision probe native TF link failed");
    GLuint passthrough = glc_program(
        "#version 410 core\nlayout(location=0) in vec4 position;layout(location=1) in vec4 colour;"
        "layout(location=2) in vec2 uv;out vec4 shade;out vec2 coord;"
        "void main(){gl_Position=position;shade=colour;coord=uv;}", fragment, NULL);
    GLuint vao, buffer, feedback;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &buffer); glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof source, source, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof source[0], NULL);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof source[0], (void *)(4 * sizeof(float)));
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
    glGenBuffers(1, &feedback); glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, feedback);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 3 * WORDS * sizeof(float), NULL, GL_STATIC_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, feedback);
    glViewport(0, 0, 64, 64); glDisable(GL_DEPTH_TEST); glDisable(GL_MULTISAMPLE);
    glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(native);
    float planes[2][4] = {{1,1,0,.2f},{-1,0,0,.4f}};
    if (glc_variant >= 6) {
        planes[1][0] = planes[1][1] = planes[1][2] = 0;
        planes[1][3] = 1;
    } else if (glc_variant >= 3) {
        float temporary[4]; memcpy(temporary, planes[0], sizeof temporary);
        memcpy(planes[0], planes[1], sizeof temporary);
        memcpy(planes[1], temporary, sizeof temporary);
    }
    glUniform4fv(glGetUniformLocation(native, "planes[0]"), 2, planes[0]);
    glEnable(GL_CLIP_DISTANCE0); glEnable(GL_CLIP_DISTANCE1);
    glBeginTransformFeedback(GL_TRIANGLES); glDrawArrays(GL_TRIANGLES, 0, 3); glEndTransformFeedback();
    float captured[3][WORDS]; glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof captured, captured);
    unsigned char reference[64 * 64 * 4], pixels[sizeof reference];
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, reference);
    for (int v = 0; v < 3; ++v) {
        fprintf(stderr, "ClipOriginal vertex%d", v);
        for (int c = 0; c < WORDS; ++c) {
            if (!isfinite(captured[v][c])) glc_fail("Precision probe nonfinite original capture");
            fprintf(stderr, " %a", captured[v][c]);
        }
        fprintf(stderr, "\n");
    }
    nanosleep(&pause, NULL);
    glDisable(GL_CLIP_DISTANCE0); glDisable(GL_CLIP_DISTANCE1);
    glUseProgram(passthrough); glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, WORDS * sizeof(float), NULL);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, WORDS * sizeof(float), (void *)(4 * sizeof(float)));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, WORDS * sizeof(float), (void *)(8 * sizeof(float)));
    for (int order = 0; order < 2; ++order) for (int mode = 0; mode < 4; ++mode) {
        struct clip_vertex polygon[MAX_POLYGON]; int count = clip_polygon(captured, order, mode, polygon);
        if (count < 3) glc_fail("Precision probe unexpectedly empty polygon");
        for (int rotation = 0; rotation < 3; ++rotation) {
            float triangles[(MAX_POLYGON - 2) * 3][WORDS]; int written = 0;
            for (int t = 1; t + 1 < count; ++t) {
                int indices[] = {rotation % count, (rotation + t) % count, (rotation + t + 1) % count};
                for (int v = 0; v < 3; ++v) {
                    for (int c = 0; c < WORDS; ++c) triangles[written][c] = (float)polygon[indices[v]].word[c];
                    ++written;
                }
            }
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)written * sizeof triangles[0], triangles, GL_STREAM_DRAW);
            glClear(GL_COLOR_BUFFER_BIT); glDrawArrays(GL_TRIANGLES, 0, written);
            glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            unsigned bad_pixels = 0, max_delta = 0, sum = 0;
            for (unsigned p = 0; p < 64 * 64; ++p) {
                int bad = 0;
                for (int c = 0; c < 4; ++c) {
                    int delta = (int)pixels[p * 4 + c] - reference[p * 4 + c];
                    if (delta < 0) delta = -delta;
                    if (delta) bad = 1;
                    sum += (unsigned)delta; if ((unsigned)delta > max_delta) max_delta = (unsigned)delta;
                }
                if (bad) {
                    if (bad_pixels < 8) fprintf(stderr,
                        "ClipDifference order%d arithmetic%s rotation%d pixel%u,%u native%u,%u,%u,%u candidate%u,%u,%u,%u\n",
                        order, arithmetic_names[mode], rotation, p % 64, p / 64,
                        reference[p*4], reference[p*4+1], reference[p*4+2], reference[p*4+3],
                        pixels[p*4], pixels[p*4+1], pixels[p*4+2], pixels[p*4+3]);
                    ++bad_pixels;
                }
            }
            fprintf(stderr, "ClipPrecision order%d arithmetic%s rotation%d vertices%d bad%u max%u sum%u\n",
                order, arithmetic_names[mode], rotation, count, bad_pixels, max_delta, sum);
            if (!rotation) for (int v = 0; v < count; ++v) {
                fprintf(stderr, "ClipPolygon order%d arithmetic%s vertex%d", order, arithmetic_names[mode], v);
                for (int c = 0; c < WORDS; ++c) fprintf(stderr, " %a", (float)polygon[v].word[c]);
                fprintf(stderr, "\n");
            }
            nanosleep(&pause, NULL);
        }
    }
    /* Leave the original native image as the standalone result. */
    glUseProgram(native); glDisableVertexAttribArray(2);
    glBufferData(GL_ARRAY_BUFFER, sizeof source, source, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof source[0], NULL);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof source[0], (void *)(4 * sizeof(float)));
    glEnable(GL_CLIP_DISTANCE0); glEnable(GL_CLIP_DISTANCE1);
    glClear(GL_COLOR_BUFFER_BIT); glDrawArrays(GL_TRIANGLES, 0, 3);
    nanosleep(&pause, NULL);
    glDisable(GL_CLIP_DISTANCE0); glDisable(GL_CLIP_DISTANCE1);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
    glDeleteBuffers(1, &feedback); glDeleteBuffers(1, &buffer);
    glDeleteVertexArrays(1, &vao); glDeleteProgram(native); glDeleteProgram(passthrough);
    GLenum error = glGetError(); if (error) glc_fail("Clip precision diagnostic GL error %x", error);
}
