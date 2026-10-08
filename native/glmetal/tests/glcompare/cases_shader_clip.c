#include "glc_gl_core.h"
#include "glcompare.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const shader_clip_modes[] = {
    "smooth", "noperspective", "flat_integer", "perspective",
    "multiple_planes", "inside", "outside", "strip", "fan", "indexed",
    "feedback", "bounded_batch", "eye_plane_fallback", "vertex_id_fallback",
    "primitive_id_batch", "first_vertex", "instance_id_fallback",
    "feedback_flat", "feedback_first", "feedback_no_planes",
    "fan_flat", "fan_first", "strip_flat", "strip_first"
};

/* The nonlinear vertex colour calculation makes clipping input attributes
   before the vertex shader observably different from clipping its outputs. */
GLC_CASE_VARIANTS(core_shader_clip, shader_clip_modes, .profile = GLC_CORE, .tolerance = 1)
{
    int mode = glc_variant;
    int capture_feedback = mode == 10 || mode == 17 || mode == 18 || mode == 19;
    int fan = mode == 8 || mode == 20 || mode == 21;
    int strip = mode == 7 || mode == 22 || mode == 23;
    const char *qualifier = mode == 1 ? "noperspective " : "";
    char vertex[1024], fragment[512];
    snprintf(vertex, sizeof vertex,
        "#version 410 core\nlayout(location=0) in vec4 position;"
        "layout(location=1) in vec4 colour;uniform vec4 planes[2];"
        "%sout vec4 shade;out vec2 coord;flat out int tag;out float clipProbe;"
        "void main(){gl_Position=position;shade=vec4(colour.rgb*colour.rgb+.05*colour.aaa,colour.a);"
        "coord=position.xy*.25+.5;tag=%s;"
        "clipProbe=dot(position,planes[0]);gl_ClipDistance[0]=clipProbe;"
        "gl_ClipDistance[1]=dot(position,planes[1]);}",
        qualifier, mode == 13 ? "gl_VertexID" : mode == 16 ? "gl_InstanceID" : "int(colour.a*10)");
    snprintf(fragment, sizeof fragment,
        "#version 410 core\n%sin vec4 shade;in vec2 coord;flat in int tag;out vec4 frag;"
        "void main(){frag=vec4(shade.rgb*(.75+.25*coord.x),shade.a);%s}",
        qualifier, mode == 14 ? "frag.b=float(gl_PrimitiveID%17)/16;" :
        mode == 2 || mode == 13 || mode == 15 || mode >= 16
            ? "frag.b=float(tag)/10;" : "");
    GLuint program = glc_program(vertex, fragment, NULL);
    if (capture_feedback) {
        const char *varyings[] = {"shade", "clipProbe"};
        glTransformFeedbackVaryings(program, 2, varyings, GL_INTERLEAVED_ATTRIBS);
        glLinkProgram(program);
        GLint linked;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[2048];
            glGetProgramInfoLog(program, sizeof log, NULL, log);
            glc_fail("Clip feedback link: %s", log);
        }
    }
    glUseProgram(program);
    float planes[2][4] = {{1, 1, 0, .2f}, {-1, 0, 0, .4f}};
    if (mode == 5) planes[0][3] = 3;
    if (mode == 6) planes[0][3] = -3;
    glUniform4fv(glGetUniformLocation(program, "planes[0]"), 2, planes[0]);
    const float source[3][8] = {
        {-.9f, -.8f, -.5f, 1, .9f, .2f, .1f, .75f},
        {.8f, -.9f, .2f, 1, .1f, .8f, .3f, .5f},
        {.1f, .9f, .6f, 1, .2f, .1f, .9f, 1}
    };
    int triangles = mode == 11 || mode == 14 ? 8193 : 1;
    int count = fan || strip ? 4 : mode == 9 || mode == 13 ? 10 : 3 * triangles;
    float (*vertices)[8] = calloc((size_t)count, sizeof *vertices);
    if (!vertices) glc_fail("Clip vertex allocation failed");
    for (int i = 0; i < count; ++i) memcpy(vertices[i], source[i % 3], sizeof source[0]);
    if (mode == 3 || mode == 1) {
        const float w[] = {.75f, 1.4f, 1.0f};
        for (int i = 0; i < 3; ++i) {
            for (int c = 0; c < 3; ++c) vertices[i][c] *= w[i];
            vertices[i][3] = w[i];
        }
    }
    if (fan || strip) {
        vertices[3][0] = .85f;
        vertices[3][1] = .8f;
        vertices[3][2] = 0;
    }
    if (mode == 11)
        for (int i = 0; i < count; ++i) vertices[i][4] = (float)((i / 3) % 17) / 16;
    if (mode == 12) vertices[0][3] = -.5f;
    const GLuint indices[] = {5, 2, 7};
    if (mode == 9 || mode == 13)
        for (int i = 0; i < 3; ++i) memcpy(vertices[indices[i]], source[i], sizeof source[i]);
    GLuint vao, buffer, elements = 0, feedback = 0, queries[2] = {0};
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)count * sizeof *vertices, vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof *vertices, NULL);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof *vertices, (void *)(4 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    if (mode == 9 || mode == 13) {
        glGenBuffers(1, &elements);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elements);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, indices, GL_STATIC_DRAW);
    }
    if (mode == 15 || mode == 18 || mode == 21 || mode == 23)
        glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
    if (mode != 19) glEnable(GL_CLIP_DISTANCE0);
    if (mode == 4) glEnable(GL_CLIP_DISTANCE1);
    if (capture_feedback) {
        glGenBuffers(1, &feedback);
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, feedback);
        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 15 * sizeof(float), NULL, GL_STATIC_READ);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, feedback);
        glGenQueries(2, queries);
        glBeginQuery(GL_PRIMITIVES_GENERATED, queries[0]);
        glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, queries[1]);
        glBeginTransformFeedback(GL_TRIANGLES);
    }
    if (mode == 16) glDrawArraysInstanced(GL_TRIANGLES, 0, 3, 3);
    else if (elements) glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_INT, NULL);
    else glDrawArrays(strip ? GL_TRIANGLE_STRIP : fan ? GL_TRIANGLE_FAN : GL_TRIANGLES, 0, count);
    if (capture_feedback) {
        glEndTransformFeedback();
        glEndQuery(GL_PRIMITIVES_GENERATED);
        glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
        GLuint generated = 0, written = 0;
        glGetQueryObjectuiv(queries[0], GL_QUERY_RESULT, &generated);
        glGetQueryObjectuiv(queries[1], GL_QUERY_RESULT, &written);
        if (generated != 1 || written != 1)
            glc_fail("Clipping changed original primitive counts: %u generated, %u written", generated, written);
        float captured[15];
        glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof captured, captured);
        for (int i = 0; i < 3; ++i)
            fprintf(stderr, "Clip feedback vertex %d: %a/%a/%a/%a distance %a\n", i,
                    captured[i * 5], captured[i * 5 + 1], captured[i * 5 + 2],
                    captured[i * 5 + 3], captured[i * 5 + 4]);
        for (int i = 0; i < 3; ++i) {
            for (int c = 0; c < 4; ++c) {
                float value = source[i][4 + c];
                float expected = c == 3 ? value : value * value + .05f * source[i][7];
                if (fabsf(captured[i * 5 + c] - expected) > 1e-6f)
                    glc_fail("Clipping changed original feedback colour at vertex %d component %d: got %a expected %a",
                             i, c, captured[i * 5 + c], expected);
            }
            float expected = source[i][0] + source[i][1] + .2f;
            if (fabsf(captured[i * 5 + 4] - expected) > 1e-6f)
                glc_fail("Clipping changed original feedback distance at vertex %d", i);
        }
    }
    glDisable(GL_CLIP_DISTANCE0);
    glDisable(GL_CLIP_DISTANCE1);
    GLenum error = glGetError();
    if (error) glc_fail("Shader clipping %s GL error %x", shader_clip_modes[mode], error);
    if (feedback) {
        glDeleteQueries(2, queries);
        glDeleteBuffers(1, &feedback);
    }
    if (elements) glDeleteBuffers(1, &elements);
    glDeleteBuffers(1, &buffer);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
    free(vertices);
}
