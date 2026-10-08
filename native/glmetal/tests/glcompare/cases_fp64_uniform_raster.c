#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *const raster_modes[] = {"vertex_scalar", "fragment_scalar", "vertex_dvec4", "fragment_dvec4",
    "vertex_scalar_clip", "fragment_scalar_clip", "vertex_dvec4_clip", "fragment_dvec4_clip"};

GLC_CASE_VARIANTS(core_fp64_uniform_raster, raster_modes, .profile = GLC_CORE)
{
    bool fragment = glc_variant & 1, vector = (glc_variant & 2) != 0, clipped = glc_variant >= 4;
    const char *position = clipped ? "gl_Position=vec4(position,0,1);gl_ClipDistance[0]=position.x+0.2;" :
        "vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);";
    const char *input = clipped ? "layout(location=0)in vec2 position;" : "";
    const char *declaration = vector ? "layout(std140)uniform Values{dvec4 values[2];};" :
                                      "layout(std140)uniform Values{double values[4];};";
    const char *check = vector ?
        "bool good=values[0]==dvec4(9007199254740994.0lf,-2147483648.0lf,4294967295.0lf,1.0000000000000002lf)"
        "&&values[1]==dvec4(-9007199254740994.0lf,2147483647.0lf,16777217.0lf,-1.0000000000000002lf);" :
        "bool good=values[0]==9007199254740994.0lf&&values[1]==-2147483648.0lf"
        "&&values[2]==4294967295.0lf&&values[3]==1.0000000000000002lf;";
    char vs[2048], fs[2048];
    if (fragment) {
        snprintf(vs, sizeof vs, "#version 410 core\n%s void main(){%s}", input, position);
        snprintf(fs, sizeof fs, "#version 410 core\n%s out vec4 color;void main(){%s color=good?vec4(0,1,0,1):vec4(1,0,0,1);}", declaration, check);
    } else {
        snprintf(vs, sizeof vs, "#version 410 core\n%s %s flat out int verdict;void main(){%s %s verdict=good?1:0;}", input, declaration, position, check);
        snprintf(fs, sizeof fs, "#version 410 core\nflat in int verdict;out vec4 color;void main(){color=verdict==1?vec4(0,1,0,1):vec4(1,0,0,1);}");
    }
    GLuint program = glc_program(vs, fs, NULL);
    const char *name = "values[0]";
    GLuint index = GL_INVALID_INDEX;
    glGetUniformIndices(program, 1, &name, &index);
    GLint type = 0, stride = 0, offset = -1, size = 0;
    glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_TYPE, &type);
    glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_ARRAY_STRIDE, &stride);
    glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_OFFSET, &offset);
    GLuint block = glGetUniformBlockIndex(program, "Values");
    glGetActiveUniformBlockiv(program, block, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
    if (type != (vector ? GL_DOUBLE_VEC4 : GL_DOUBLE) || stride != (vector ? 32 : 16) || offset != 0 || size != 64)
        glc_fail("FP64 raster block reflected type=%x stride=%d offset=%d bytes=%d", type, stride, offset, size);
    const double values[8] = {9007199254740994.0, -2147483648.0, 4294967295.0, 0x1.0000000000001p0,
                             -9007199254740994.0, 2147483647.0, 16777217.0, -0x1.0000000000001p0};
    unsigned char storage[64];
    memset(storage, 0x5a, sizeof storage);
    if (vector) memcpy(storage, values, sizeof storage);
    else for (int i = 0; i < 4; ++i) memcpy(storage + 16 * i, values + i, sizeof(double));
    GLuint buffer, vao;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_UNIFORM_BUFFER, buffer);
    glBufferData(GL_UNIFORM_BUFFER, sizeof storage, storage, GL_STATIC_DRAW);
    glUniformBlockBinding(program, block, 0);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, buffer);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    GLuint vertices = 0;
    if (clipped) {
        const float positions[] = {-1,-1, 3,-1, -1,3};
        glGenBuffers(1, &vertices);
        glBindBuffer(GL_ARRAY_BUFFER, vertices);
        glBufferData(GL_ARRAY_BUFFER, sizeof positions, positions, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
        glEnableVertexAttribArray(0);
        glEnable(GL_CLIP_DISTANCE0);
    }
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    unsigned char pixel[4];
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (glGetError() || pixel[0] || pixel[1] != 255 || pixel[2] || pixel[3] != 255)
        glc_fail("FP64 raster UBO validation failed: %u/%u/%u/%u", pixel[0], pixel[1], pixel[2], pixel[3]);
    if (clipped) {
        glReadPixels(4, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        if (pixel[0] || pixel[1] || pixel[2] || pixel[3] != 255)
            glc_fail("FP64 raster clip left a covered sample");
        glDisable(GL_CLIP_DISTANCE0);
        glDeleteBuffers(1, &vertices);
    }
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, 0);
    glDeleteBuffers(1, &buffer);
    glDeleteVertexArrays(1, &vao);
    glUseProgram(0);
    glDeleteProgram(program);
}
