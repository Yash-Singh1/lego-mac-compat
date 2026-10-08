#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *const generated_binding_modes[] = {
    "geometry_block", "geometry_dvec4", "geometry_dmat2x3",
    "tess_control_block", "tess_control_dvec4", "tess_control_dmat2x3",
    "tess_eval_block", "tess_eval_dvec4", "tess_eval_dmat2x3",
    "clip_fragment_block", "clip_fragment_dvec4", "clip_fragment_dmat2x3"
};

static void generated_binding_attach(GLuint program, GLenum stage, const char *source)
{
    GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &source, NULL); glCompileShader(shader);
    GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0}; glGetShaderInfoLog(shader, sizeof log, NULL, log);
        glc_fail("Generated binding stage %x compile: %s", stage, log);
    }
    glAttachShader(program, shader); glDeleteShader(shader);
}

GLC_CASE_VARIANTS(core_fp64_generated_bindings, generated_binding_modes, .profile = GLC_CORE)
{
    int path = glc_variant / 3, storage_mode = glc_variant % 3;
    const char *declaration = storage_mode == 0 ? "layout(std140) uniform ZValues {dvec4 values[2];};" :
        storage_mode == 1 ? "uniform dvec4 value;" : "uniform dmat2x3 value;";
    const char *check = storage_mode == 0 ? "int good=(values[0]==dvec4(1.0000000000000002lf) && values[1]==dvec4(9007199254740994.0lf))?1:0;" :
        storage_mode == 1 ? "int good=value==dvec4(1.0000000000000002lf,-1.0000000000000002lf,9007199254740994.0lf,16777217.0lf)?1:0;" :
        "int good=value==dmat2x3(1.0000000000000002lf,-1.0000000000000002lf,9007199254740994.0lf,16777217.0lf,-16777217.0lf,4294967295.0lf)?1:0;";
    char vs[1024], fs[2048], gs[2048], tc[2048], te[2048];
    snprintf(vs, sizeof vs, "#version 410 core\nlayout(location=0)in vec4 position;"
        "layout(std140)uniform ABefore{vec4 marker;};uniform vec4 before_value;"
        "void main(){gl_Position=position;gl_Position.x+=marker.x+before_value.x;%s}",
        path == 3 ? "gl_ClipDistance[0]=position.x+0.2;" : "");
    snprintf(fs, sizeof fs, "#version 410 core\n%s out vec4 color;%s void main(){%s color=good==1?vec4(0,1,0,1):vec4(1,0,0,1);}",
        path == 3 ? "" : "flat in int verdict;", path == 3 ? declaration : "", path == 3 ? check : "int good=verdict;");
    GLuint program = glCreateProgram();
    generated_binding_attach(program, GL_VERTEX_SHADER, vs); generated_binding_attach(program, GL_FRAGMENT_SHADER, fs);
    if (path == 0) {
        snprintf(gs, sizeof gs, "#version 410 core\nlayout(triangles)in;layout(triangle_strip,max_vertices=3)out;"
            "flat out int verdict;%s void main(){%s for(int j=0;j<3;++j){verdict=good;gl_Position=gl_in[j].gl_Position;EmitVertex();}EndPrimitive();}", declaration, check);
        generated_binding_attach(program, GL_GEOMETRY_SHADER, gs);
    } else if (path < 3) {
        snprintf(tc, sizeof tc, "#version 410 core\nlayout(vertices=3)out;out int verdict_cp[];%s void main(){%s"
            "verdict_cp[gl_InvocationID]=good;gl_out[gl_InvocationID].gl_Position=gl_in[gl_InvocationID].gl_Position;"
            "if(gl_InvocationID==0){gl_TessLevelOuter[0]=1;gl_TessLevelOuter[1]=1;gl_TessLevelOuter[2]=1;gl_TessLevelInner[0]=1;}}",
            path == 1 ? declaration : "", path == 1 ? check : "int good=1;");
        snprintf(te, sizeof te, "#version 410 core\nlayout(triangles,equal_spacing,ccw)in;in int verdict_cp[];flat out int verdict;"
            "%s void main(){%s verdict=good;gl_Position=gl_TessCoord.x*gl_in[0].gl_Position+gl_TessCoord.y*gl_in[1].gl_Position+gl_TessCoord.z*gl_in[2].gl_Position;}",
            path == 2 ? declaration : "", path == 2 ? check : "int good=verdict_cp[0];");
        generated_binding_attach(program, GL_TESS_CONTROL_SHADER, tc); generated_binding_attach(program, GL_TESS_EVALUATION_SHADER, te);
    }
    glLinkProgram(program); GLint ok = 0; glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0}; glGetProgramInfoLog(program, sizeof log, NULL, log);
        glc_fail("Generated uniform binding link: %s", log);
    }
    glUseProgram(program);
    glUniform4f(glGetUniformLocation(program, "before_value"), 0, .25f, .5f, 1);
    GLuint blocks[2]; int block_count = 1;
    unsigned char data[320]; memset(data, 0xa5, sizeof data);
    const float marker[] = {0, .25f, .5f, 1}; memcpy(data + 256, marker, sizeof marker);
    glGenBuffers(1, blocks); glBindBuffer(GL_UNIFORM_BUFFER, blocks[0]);
    glBufferData(GL_UNIFORM_BUFFER, sizeof data, data, GL_STATIC_DRAW);
    glUniformBlockBinding(program, glGetUniformBlockIndex(program, "ABefore"), 4);
    glBindBufferRange(GL_UNIFORM_BUFFER, 4, blocks[0], 256, 16);
    if (storage_mode == 0) {
        GLuint block = glGetUniformBlockIndex(program, "ZValues");
        const char *name = "values[0]"; GLuint index = GL_INVALID_INDEX; GLint type = 0, stride = 0;
        glGetUniformIndices(program, 1, &name, &index);
        glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_TYPE, &type);
        glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_ARRAY_STRIDE, &stride);
        if (type != GL_DOUBLE_VEC4 || stride != 32) glc_fail("Generated block lost exact dvec4 layout: %x/%d", type, stride);
        for (int i = 0; i < 8; ++i) { double value = i < 4 ? 0x1.0000000000001p0 : 9007199254740994.0; memcpy(data + 256 + i * 8, &value, 8); }
        glGenBuffers(1, blocks + 1); glBindBuffer(GL_UNIFORM_BUFFER, blocks[1]);
        glBufferData(GL_UNIFORM_BUFFER, sizeof data, data, GL_STATIC_DRAW);
        glUniformBlockBinding(program, block, 6); glBindBufferRange(GL_UNIFORM_BUFFER, 6, blocks[1], 256, 64);
        block_count = 2;
    } else {
        const double values[] = {0x1.0000000000001p0, -0x1.0000000000001p0, 9007199254740994.0, 16777217.0, -16777217.0, 4294967295.0};
        GLint location = glGetUniformLocation(program, "value");
        if (storage_mode == 1) glUniform4dv(location, 1, values);
        else glUniformMatrix2x3dv(location, 1, GL_FALSE, values);
        const char *name = "value"; GLuint index = GL_INVALID_INDEX; GLint type = 0;
        glGetUniformIndices(program, 1, &name, &index);
        glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_TYPE, &type);
        double queried[6] = {0}; glGetUniformdv(program, location, queried);
        if (type != (storage_mode == 1 ? GL_DOUBLE_VEC4 : GL_DOUBLE_MAT2x3) ||
            memcmp(queried, values, (storage_mode == 1 ? 4 : 6) * sizeof(double)))
            glc_fail("Generated default uniform lost exact type/getter: %x", type);
    }
    GLuint vao, vertices; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    const float positions[] = {-1,-1,0,1, 3,-1,0,1, -1,3,0,1};
    glGenBuffers(1, &vertices); glBindBuffer(GL_ARRAY_BUFFER, vertices);
    glBufferData(GL_ARRAY_BUFFER, sizeof positions, positions, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, NULL); glEnableVertexAttribArray(0);
    glViewport(0, 0, 64, 64); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    if (path == 3) glEnable(GL_CLIP_DISTANCE0);
    if (path == 1 || path == 2) glPatchParameteri(GL_PATCH_VERTICES, 3);
    glDrawArrays(path == 1 || path == 2 ? GL_PATCHES : GL_TRIANGLES, 0, 3);
    unsigned char pixel[4] = {0}; glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    GLenum error = glGetError();
    if (path == 3) glDisable(GL_CLIP_DISTANCE0);
    glBindBufferBase(GL_UNIFORM_BUFFER, 4, 0); glBindBufferBase(GL_UNIFORM_BUFFER, 6, 0);
    glDeleteBuffers(block_count, blocks); glDeleteBuffers(1, &vertices);
    glUseProgram(0); glDeleteProgram(program); glDeleteVertexArrays(1, &vao);
    if (error || pixel[0] || pixel[1] != 255 || pixel[2] || pixel[3] != 255)
        glc_fail("Generated exact uniform binding: %s pixel=%u/%u/%u/%u error=%x", generated_binding_modes[glc_variant], pixel[0], pixel[1], pixel[2], pixel[3], error);
}
