#include "glc_gl_core.h"
#include "glcompare.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { MAX_RECORDS = 128, GUARD_WORDS = 16 };
struct tess_record { float coord[3], position[4], color[4]; int32_t patch; };
_Static_assert(sizeof(struct tess_record) == 48, "TF record must be tightly packed");
static uint32_t tess_bits(float value) { uint32_t bits; memcpy(&bits, &value, 4); return bits; }
static int tess_compare(const void *lhs, const void *rhs)
{
    const struct tess_record *a = lhs, *b = rhs;
    if (a->patch != b->patch) return a->patch < b->patch ? -1 : 1;
    for (int c = 0; c < 3; ++c) if (a->coord[c] != b->coord[c]) return a->coord[c] < b->coord[c] ? -1 : 1;
    return 0;
}
static void tess_log(const char *order, unsigned index, const struct tess_record *r)
{
    fprintf(stderr, "TessCapture %s record%u patch%d coord%08x,%08x,%08x position%08x,%08x,%08x,%08x ec%08x,%08x,%08x,%08x values%a,%a,%a\n",
        order, index, r->patch, tess_bits(r->coord[0]), tess_bits(r->coord[1]), tess_bits(r->coord[2]),
        tess_bits(r->position[0]), tess_bits(r->position[1]), tess_bits(r->position[2]), tess_bits(r->position[3]),
        tess_bits(r->color[0]), tess_bits(r->color[1]), tess_bits(r->color[2]), tess_bits(r->color[3]),
        r->coord[0], r->coord[1], r->coord[2]);
}

GLC_CASE(driver_tess_patch_feedback, .profile = GLC_CORE)
{
    /* Original uniformless patch_outputs shader, with two diagnostic outputs
     * appended after its original position and ec calculations. */
    const char *sources[] = {
        "#version 410 core\nlayout(location=0)in vec2 position;layout(location=1)in vec4 color;"
        "out vec4 vc;void main(){gl_Position=vec4(position,0,1);vc=color;}",
        "#version 410 core\nlayout(vertices = 3) out; in vec4 vc[]; out vec4 tc[]; patch out vec4 pc; patch out float shade;\n"
        "void main(){ gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position; tc[gl_InvocationID] = vc[gl_InvocationID];\n"
        " pc = (vc[0] + vc[1] + vc[2]) / 3.0; shade = float(gl_PrimitiveID) * 0.5 + 0.25;\n"
        " gl_TessLevelOuter[0] = 2.0 + float(gl_PrimitiveID); gl_TessLevelOuter[1] = 3.0; gl_TessLevelOuter[2] = 4.0; gl_TessLevelInner[0] = 3.0; }",
        "#version 410 core\nlayout(triangles) in; in vec4 tc[]; patch in vec4 pc; patch in float shade; out vec4 ec;\n"
        "out vec3 probeCoord;flat out int probePatch;\n"
        "void main(){ vec3 b = gl_TessCoord; gl_Position = b.x * gl_in[0].gl_Position + b.y * gl_in[1].gl_Position + b.z * gl_in[2].gl_Position;\n"
        " ec = vec4(mix(pc.rgb, tc[0].rgb * b.x + tc[1].rgb * b.y + tc[2].rgb * b.z, shade),\n"
        "  gl_TessLevelOuter[0] / 8.0 + gl_TessLevelInner[0] / 8.0 + float(gl_PrimitiveID) * 0.1);"
        "probeCoord=gl_TessCoord;probePatch=gl_PrimitiveID;}",
        "#version 410 core\nin vec4 ec;out vec4 result;void main(){result=ec;}"
    };
    const GLenum stages[] = {GL_VERTEX_SHADER, GL_TESS_CONTROL_SHADER, GL_TESS_EVALUATION_SHADER, GL_FRAGMENT_SHADER};
    GLuint program = glCreateProgram();
    for (int stage = 0; stage < 4; ++stage) {
        GLuint shader = glCreateShader(stages[stage]);
        glShaderSource(shader, 1, &sources[stage], NULL); glCompileShader(shader);
        GLint ok; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[2048]; glGetShaderInfoLog(shader, sizeof log, NULL, log); glc_fail("Tess capture stage%d: %s", stage, log); }
        glAttachShader(program, shader); glDeleteShader(shader);
    }
    const char *varyings[] = {"probeCoord", "gl_Position", "ec", "probePatch"};
    glTransformFeedbackVaryings(program, 4, varyings, GL_INTERLEAVED_ATTRIBS); glLinkProgram(program);
    GLint linked; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) { char log[2048]; glGetProgramInfoLog(program, sizeof log, NULL, log); glc_fail("Tess capture link: %s", log); }
    const float vertices[][6] = {
        {-.9f,-.9f,1,0,0,1}, {.9f,-.85f,0,1,0,.8f},
        {.85f,.9f,0,0,1,.6f}, {-.85f,.85f,1,1,0,.4f}
    };
    const GLushort indices[] = {0,1,2,0,2,3};
    GLuint vao, buffers[3], queries[2];
    glGenVertexArrays(1, &vao); glBindVertexArray(vao); glGenBuffers(3, buffers);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]); glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof vertices[0], NULL);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof vertices[0], (void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]); glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, indices, GL_STATIC_DRAW);
    unsigned char initial[MAX_RECORDS * sizeof(struct tess_record) + GUARD_WORDS * sizeof(uint32_t)];
    memset(initial, 0xcd, sizeof initial);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffers[2]);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, sizeof initial, initial, GL_STATIC_READ);
    /* Guard bytes are outside the bound capture range. */
    glBindBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffers[2], 0, MAX_RECORDS * sizeof(struct tess_record));
    glGenQueries(2, queries); glUseProgram(program);
    glViewport(0, 0, 64, 64); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_MULTISAMPLE);
    glPatchParameteri(GL_PATCH_VERTICES, 3); glClearColor(.1f, .1f, .1f, 1); glClear(GL_COLOR_BUFFER_BIT);
    glBeginQuery(GL_PRIMITIVES_GENERATED, queries[0]);
    glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, queries[1]);
    glBeginTransformFeedback(GL_TRIANGLES);
    GLenum step_error = glGetError();
    fprintf(stderr, "TessCapture API begin error%04x\n", step_error);
    if (step_error) glc_fail("Tess capture Begin GL error %x", step_error);
    glDrawElements(GL_PATCHES, 6, GL_UNSIGNED_SHORT, NULL);
    step_error = glGetError();
    fprintf(stderr, "TessCapture API draw error%04x\n", step_error);
    if (step_error) glc_fail("Tess capture draw GL error %x", step_error);
    glEndTransformFeedback();
    step_error = glGetError();
    fprintf(stderr, "TessCapture API end error%04x\n", step_error);
    if (step_error) glc_fail("Tess capture End GL error %x", step_error);
    glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN); glEndQuery(GL_PRIMITIVES_GENERATED);
    const struct timespec pause = {0, 50000000}; nanosleep(&pause, NULL);
    GLuint generated, written; glGetQueryObjectuiv(queries[0], GL_QUERY_RESULT, &generated);
    glGetQueryObjectuiv(queries[1], GL_QUERY_RESULT, &written);
    fprintf(stderr, "TessCapture counts generated%u written%u capacity%u\n", generated, written, MAX_RECORDS);
    if (!generated || !written || generated > MAX_RECORDS / 3 || written > MAX_RECORDS / 3)
        glc_fail("Tess capture primitive count invalid or exceeds bounded capacity");
    uint32_t guard[GUARD_WORDS];
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, MAX_RECORDS * sizeof(struct tess_record), sizeof guard, guard);
    for (int i = 0; i < GUARD_WORDS; ++i) if (guard[i] != 0xcdcdcdcd) glc_fail("Tess capture guard overwritten");
    struct tess_record records[MAX_RECORDS]; unsigned count = written * 3;
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, count * sizeof records[0], records);
    for (unsigned i = 0; i < count; ++i) {
        if (records[i].patch < 0 || records[i].patch > 1) glc_fail("Tess capture invalid patch ID");
        for (int c = 0; c < 3; ++c) if (!isfinite(records[i].coord[c])) glc_fail("Tess capture nonfinite coordinate");
        for (int c = 0; c < 4; ++c) if (!isfinite(records[i].position[c]) || !isfinite(records[i].color[c])) glc_fail("Tess capture nonfinite output");
        tess_log("emission", i, &records[i]);
    }
    qsort(records, count, sizeof records[0], tess_compare);
    for (unsigned i = 0; i < count; ++i) tess_log("sorted", i, &records[i]);
    if (generated != written) glc_fail("Tess capture generated/written primitive counts disagree");
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
    glUseProgram(0); glDeleteQueries(2, queries); glDeleteBuffers(3, buffers);
    glDeleteVertexArrays(1, &vao); glDeleteProgram(program);
    GLenum error = glGetError(); if (error) glc_fail("Tess capture GL error %x", error);
}
