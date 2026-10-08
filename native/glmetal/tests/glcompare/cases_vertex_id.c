#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdio.h>
#include <string.h>

static const char *const vertex_id_modes[] = {
    "arrays_first", "arrays_double_first", "indexed", "indexed_first_vertex",
    "positive_base", "negative_base", "reset_base", "instanced", "global_initializer"
};

GLC_CASE_VARIANTS(core_vertex_id_origin, vertex_id_modes, .profile = GLC_CORE)
{
    int mode = glc_variant;
    char vertex[512];
    snprintf(vertex, sizeof vertex,
        "#version 410 core\nlayout(location=0) in vec2 position;flat out int vertex;flat out int instance;"
        "%svoid main(){gl_Position=vec4(position,0,1);vertex=%s;instance=gl_InstanceID;}",
        mode == 8 ? "int original_id=gl_VertexID;" : "", mode == 8 ? "original_id" : "gl_VertexID");
    GLuint program = glc_program(vertex,
        "#version 410 core\nflat in int vertex;flat in int instance;out vec4 frag;"
        "void main(){frag=vec4(float(vertex)/255,float(instance)/255,.5,1);}", NULL);
    const float triangle[3][2] = {{-.9f,-.8f},{.8f,-.9f},{.1f,.9f}};
    float vertices[16][2] = {{0}};
    double doubles[16][2] = {{0}};
    GLuint indices[3] = {5,2,7};
    int first = 4, base = mode == 4 ? 3 : mode == 5 ? -2 : 0;
    int indexed = mode >= 2 && mode <= 5;
    for (int i = 0; i < 3; ++i) {
        int slot = indexed ? (int)indices[i] + base : first + i;
        memcpy(vertices[slot], triangle[i], sizeof triangle[i]);
        for (int c = 0; c < 2; ++c) doubles[slot][c] = triangle[i][c];
        if (mode == 6) memcpy(vertices[i], triangle[i], sizeof triangle[i]);
    }
    GLuint vao, buffer, elements = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, mode == 1 ? sizeof doubles : sizeof vertices,
                 mode == 1 ? (const void *)doubles : vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, mode == 1 ? GL_DOUBLE : GL_FLOAT, GL_FALSE, 0, NULL);
    glEnableVertexAttribArray(0);
    if (mode == 3) glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
    if (indexed) {
        glGenBuffers(1, &elements);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elements);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, indices, GL_STATIC_DRAW);
        glDrawElementsBaseVertex(GL_TRIANGLES, 3, GL_UNSIGNED_INT, NULL, base);
    } else if (mode == 7) glDrawArraysInstanced(GL_TRIANGLES, first, 3, 3);
    else glDrawArrays(GL_TRIANGLES, first, 3);
    if (mode == 6) glDrawArrays(GL_TRIANGLES, 0, 3);
    unsigned char pixel[4];
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    int expected = indexed ? (int)indices[mode == 3 ? 0 : 2] + base : mode == 6 ? 2 : first + 2;
    int expected_instance = mode == 7 ? 2 : 0;
    if (pixel[0] != expected || pixel[1] != expected_instance)
        glc_fail("%s lost original IDs: got vertex %u instance %u, expected %d %d",
                 vertex_id_modes[mode], pixel[0], pixel[1], expected, expected_instance);
    GLenum error = glGetError();
    if (error) glc_fail("Vertex ID %s GL error %x", vertex_id_modes[mode], error);
    if (elements) glDeleteBuffers(1, &elements);
    glDeleteBuffers(1, &buffer);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
}
