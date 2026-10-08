/* Depth-comparison borders. Apple establishes GL_CLAMP's exact filtering
 * behavior; these cases deliberately do not assert an inferred edge formula.
 * Build: make -j2 glcompare. Run later with --filter shadow_border. */
#include "glc_gl_legacy.h"
#include "glcompare.h"
#include <stdio.h>

static const char *const clamp_names[] = {"nearest", "linear"};
static const char *const border_names[] = {
    "quarter_nearest", "quarter_linear", "three_quarters_nearest", "three_quarters_linear"
};

static void shadow_border(GLenum wrap, int linear, float border, int legacy)
{
    GLuint texture;
    glGenTextures(1, &texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    /* A lit/unlit split distinguishes border comparisons from edge texels
     * without equal-depth quantization ambiguity. */
    float depths[16];
    for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x)
        depths[y * 4 + x] = x < 2 ? .0625f : .9375f;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 4, 4, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depths);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    float border_color[] = {border, border, border, border};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border_color);
    const char *vertex = legacy ?
        "#version 120\nvarying vec2 uv;void main(){gl_Position=gl_Vertex;uv=gl_MultiTexCoord0.xy;}" :
        "#version 150 core\nin vec2 position;out vec2 uv;void main(){gl_Position=vec4(position,0,1);uv=position*.5+.5;}";
    const char *fragment = legacy ?
        "#version 120\n#extension GL_ARB_shader_texture_lod : require\n"
        "uniform sampler2DShadow atlas;varying vec2 uv;void main(){"
        "float reference=uv.y<.333333?.125:(uv.y<.666667?.5:.875);vec2 p=vec2(uv.x*1.5-.25,.5);"
        "float q=uv.y<.333333?.5:(uv.y<.666667?1.:2.);"
        "float explicit_lod=shadow2DLod(atlas,vec3(p,reference),0.).r;"
        "float projected=shadow2DProj(atlas,vec4(p*q,reference*q,q)).r;"
        "gl_FragColor=vec4(explicit_lod,projected,0,1);}" :
        "#version 150 core\nuniform sampler2DShadow atlas;in vec2 uv;out vec4 color;void main(){"
        "float reference=uv.y<.333333?.125:(uv.y<.666667?.5:.875);vec2 p=vec2(uv.x*1.5-.25,.5);"
        "float q=uv.y<.333333?.5:(uv.y<.666667?1.:2.);"
        "color=vec4(textureLod(atlas,vec3(p,reference),0.),"
        "textureProj(atlas,vec4(p*q,reference*q,q)),0,1);}";
    const char *attributes[] = {"position", NULL};
    GLuint program = glc_program(vertex, fragment, legacy ? NULL : attributes);
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "atlas"), 0);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DITHER);
    glViewport(0, 0, glc_width, glc_height);
    GLuint buffer = 0, vao = 0;
    if (legacy) {
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(-1, -1);
        glTexCoord2f(1, 0); glVertex2f(1, -1);
        glTexCoord2f(1, 1); glVertex2f(1, 1);
        glTexCoord2f(0, 1); glVertex2f(-1, 1);
        glEnd();
    } else {
        /* Legacy SDK has the APPLE spelling; resolve the core entry points
         * using glc_lookup, which does not depend on header-generated IDs. */
        extern void *glc_lookup(const char *name);
        void (*gen_vao)(GLsizei, GLuint *) = glc_lookup("glGenVertexArrays");
        void (*bind_vao)(GLuint) = glc_lookup("glBindVertexArray");
        if (!gen_vao || !bind_vao) glc_fail("Shadow border core VAO entry points unavailable");
        gen_vao(1, &vao); bind_vao(vao);
        const GLfloat quad[] = {-1,-1, 1,-1, -1,1, 1,1};
        glGenBuffers(1, &buffer);
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    GLenum error = glGetError();
    if (error) glc_fail("Shadow border wrap=%x filter=%s depth=%.2f error=%x", wrap, linear ? "linear" : "nearest", border, error);
    const GLint probes[] = {1, 10, 11, 12, 15, 16, 47, 48, 51, 52, 53, 62};
    for (unsigned row = 0; row < 3; ++row) {
        fprintf(stderr, "shadow_border wrap=%x filter=%s border=%.2f reference=%.3f:", wrap,
                linear ? "linear" : "nearest", border, row == 0 ? .125 : row == 1 ? .5 : .875);
        for (unsigned i = 0; i < sizeof probes / sizeof *probes; ++i) {
            unsigned char pixel[4];
            glReadPixels(probes[i], row == 0 ? 10 : row == 1 ? 32 : 54, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            fprintf(stderr, " x%d=%u/%u", probes[i], pixel[0], pixel[1]);
        }
        fputc('\n', stderr);
    }
    glUseProgram(0);
    glDeleteProgram(program);
    glDeleteTextures(1, &texture);
    if (buffer) glDeleteBuffers(1, &buffer);
    if (vao) {
        extern void *glc_lookup(const char *name);
        void (*delete_vao)(GLsizei, const GLuint *) = glc_lookup("glDeleteVertexArrays");
        delete_vao(1, &vao);
    }
}
GLC_CASE_VARIANTS(legacy_shadow_border_clamp, clamp_names, .profile = GLC_LEGACY, .tolerance = 2)
{
    shadow_border(GL_CLAMP, glc_variant, .25f, 1);
}
GLC_CASE_VARIANTS(core_shadow_border_custom, border_names, .profile = GLC_CORE, .tolerance = 2)
{
    shadow_border(GL_CLAMP_TO_BORDER, glc_variant & 1, glc_variant < 2 ? .25f : .75f, 0);
}
