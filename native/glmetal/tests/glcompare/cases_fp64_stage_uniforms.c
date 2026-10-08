#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *const stage_uniform_modes[] = {"geometry_double", "geometry_dvec4", "geometry_dmat2x3",
    "tess_control_double", "tess_control_dvec4", "tess_control_dmat2x3",
    "tess_eval_double", "tess_eval_dvec4", "tess_eval_dmat2x3"};

static void stage_uniform_attach(GLuint program, GLenum stage, const char *source)
{
    GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &source, NULL); glCompileShader(shader);
    GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0}; glGetShaderInfoLog(shader, sizeof log, NULL, log);
        glc_fail("FP64 stage %x compile: %s", stage, log);
    }
    glAttachShader(program, shader); glDeleteShader(shader);
}

GLC_CASE_VARIANTS(core_fp64_stage_uniforms, stage_uniform_modes, .profile = GLC_CORE)
{
    int stage = glc_variant / 3, shape = glc_variant % 3;
    const char *types[] = {"double", "dvec4", "dmat2x3"};
    char declaration[256], check[512], gs[2048], tc[2048], te[2048];
    snprintf(declaration, sizeof declaration, "layout(std140) uniform Values {%s values[4];};", types[shape]);
    snprintf(check, sizeof check, "int good=1;for(int i=0;i<4;++i)if(values[i]!=%s(9007199254740994.0lf+double(i*2)))good=0;", types[shape]);
    const char *vs = "#version 410 core\nlayout(location=0)in vec4 position;void main(){gl_Position=position;}";
    const char *fs = "#version 410 core\nflat in int verdict;out vec4 color;void main(){color=verdict==1?vec4(0,1,0,1):vec4(1,0,0,1);}";
    GLuint program = glCreateProgram();
    stage_uniform_attach(program, GL_VERTEX_SHADER, vs); stage_uniform_attach(program, GL_FRAGMENT_SHADER, fs);
    if (stage == 0) {
        snprintf(gs, sizeof gs, "#version 410 core\nlayout(triangles)in;layout(triangle_strip,max_vertices=3)out;"
            "flat out int verdict;%s void main(){%s for(int j=0;j<3;++j){verdict=good;"
            "gl_Position=gl_in[j].gl_Position;EmitVertex();}EndPrimitive();}", declaration, check);
        stage_uniform_attach(program, GL_GEOMETRY_SHADER, gs);
    } else {
        snprintf(tc, sizeof tc, "#version 410 core\nlayout(vertices=3)out;out int verdict_cp[];%s void main(){%s"
            "verdict_cp[gl_InvocationID]=good;gl_out[gl_InvocationID].gl_Position=gl_in[gl_InvocationID].gl_Position;"
            "if(gl_InvocationID==0){gl_TessLevelOuter[0]=1;gl_TessLevelOuter[1]=1;gl_TessLevelOuter[2]=1;gl_TessLevelInner[0]=1;}}",
            stage == 1 ? declaration : "", stage == 1 ? check : "int good=1;");
        snprintf(te, sizeof te, "#version 410 core\nlayout(triangles,equal_spacing,ccw)in;in int verdict_cp[];"
            "flat out int verdict;%s void main(){%s verdict=good;"
            "gl_Position=gl_TessCoord.x*gl_in[0].gl_Position+gl_TessCoord.y*gl_in[1].gl_Position+gl_TessCoord.z*gl_in[2].gl_Position;}",
            stage == 2 ? declaration : "", stage == 2 ? check : "int good=verdict_cp[0];");
        stage_uniform_attach(program, GL_TESS_CONTROL_SHADER, tc); stage_uniform_attach(program, GL_TESS_EVALUATION_SHADER, te);
    }
    if (stage == 0) {
        const char *varying = "verdict";
        glTransformFeedbackVaryings(program, 1, &varying, GL_INTERLEAVED_ATTRIBS);
    }
    glLinkProgram(program);
    GLint ok = 0; glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0}; glGetProgramInfoLog(program, sizeof log, NULL, log);
        glc_fail("Exact stage uniform link: %s", log);
    }
    glUseProgram(program);
    const char *name = "values[0]";
    GLuint index = GL_INVALID_INDEX; GLint offset = -1, array_stride = 0, matrix_stride = 0, type = 0, row_major = 1, block_size = 0;
    glGetUniformIndices(program, 1, &name, &index);
    glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_OFFSET, &offset);
    glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_ARRAY_STRIDE, &array_stride);
    glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_MATRIX_STRIDE, &matrix_stride);
    glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_TYPE, &type);
    glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_IS_ROW_MAJOR, &row_major);
    GLuint block = glGetUniformBlockIndex(program, "Values");
    glGetActiveUniformBlockiv(program, block, GL_UNIFORM_BLOCK_DATA_SIZE, &block_size);
    const GLenum gltypes[] = {GL_DOUBLE, GL_DOUBLE_VEC4, GL_DOUBLE_MAT2x3};
    const int strides[] = {16, 32, 64};
    if (type != (GLint)gltypes[shape] || offset != 0 || array_stride != strides[shape] ||
        matrix_stride != (shape == 2 ? 32 : 0) || row_major || block_size != strides[shape] * 4)
        glc_fail("FP64 stage layout: %s type=%x offset=%d stride=%d matrix=%d row_major=%d size=%d",
                 stage_uniform_modes[glc_variant], type, offset, array_stride, matrix_stride, row_major, block_size);
    unsigned char storage[512]; memset(storage, 0xa5, sizeof storage);
    for (int i = 0; i < 4; ++i) {
        double value = 9007199254740994.0 + (double)(i * 2);
        if (shape == 0) memcpy(storage + 256 + i * array_stride, &value, 8);
        if (shape == 1) for (int lane = 0; lane < 4; ++lane) memcpy(storage + 256 + i * array_stride + lane * 8, &value, 8);
        if (shape == 2) for (int column = 0; column < 2; ++column) for (int row = 0; row < 3; ++row) {
            double element = row == column ? value : 0.0;
            memcpy(storage + 256 + i * array_stride + column * matrix_stride + row * 8, &element, 8);
        }
    }
    GLuint uniform, vertices, vao;
    glGenBuffers(1, &uniform); glBindBuffer(GL_UNIFORM_BUFFER, uniform);
    glBufferData(GL_UNIFORM_BUFFER, sizeof storage, storage, GL_STATIC_DRAW);
    glUniformBlockBinding(program, block, 5); glBindBufferRange(GL_UNIFORM_BUFFER, 5, uniform, 256, block_size);
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    const float positions[] = {-1,-1,0,1, 3,-1,0,1, -1,3,0,1};
    glGenBuffers(1, &vertices); glBindBuffer(GL_ARRAY_BUFFER, vertices);
    glBufferData(GL_ARRAY_BUFFER, sizeof positions, positions, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, NULL); glEnableVertexAttribArray(0);
    glViewport(0, 0, 64, 64); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    if (stage) glPatchParameteri(GL_PATCH_VERTICES, 3);
    GLuint feedback = 0;
    if (stage == 0) {
        GLint empty[3] = {0,0,0};
        glGenBuffers(1, &feedback);
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, feedback);
        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, sizeof empty, empty, GL_STREAM_READ);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, feedback);
        glBeginTransformFeedback(GL_TRIANGLES);
    }
    glDrawArrays(stage ? GL_PATCHES : GL_TRIANGLES, 0, 3);
    if (stage == 0) {
        GLint values[3] = {0,0,0};
        glEndTransformFeedback();
        glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof values, values);
        if (values[0] != 1 || values[1] != 1 || values[2] != 1)
            glc_fail("Exact geometry feedback values: %d/%d/%d", values[0], values[1], values[2]);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
        glDeleteBuffers(1, &feedback);
    }
    unsigned char pixel[4] = {0}; glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    GLenum error = glGetError();
    glBindBufferBase(GL_UNIFORM_BUFFER, 5, 0); glDeleteBuffers(1, &uniform); glDeleteBuffers(1, &vertices);
    glUseProgram(0); glDeleteProgram(program); glDeleteVertexArrays(1, &vao);
    if (error || pixel[0] || pixel[1] != 255 || pixel[2] || pixel[3] != 255)
        glc_fail("Exact FP64 stage arithmetic: %s pixel=%u/%u/%u/%u error=%x",
                 stage_uniform_modes[glc_variant], pixel[0], pixel[1], pixel[2], pixel[3], error);
}
