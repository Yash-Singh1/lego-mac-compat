#include "glc_gl_legacy.h"
#include "glcompare.h"
#include <stdio.h>

static GLuint precision_target(GLint *original, GLuint *texture)
{
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, original);
    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glGenTextures(1, texture);
    glBindTexture(GL_TEXTURE_2D, *texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F_ARB, 64, 64, 0, GL_RGBA, GL_FLOAT, NULL);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        glc_fail("Precision float framebuffer is incomplete");
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    return fb;
}

static void precision_finish(GLint original, GLuint fb, GLuint texture)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)original);
    glBlitFramebuffer(0, 0, 64, 64, 0, 0, 64, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)original);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &texture);
}

static const char *const clip_precision_names[] = {
    "ff_unclipped", "ff_clipped", "shader_unclipped", "shader_clipped",
    "cpu_clipped_float", "cpu_clipped_double"
};
GLC_CASE_VARIANTS(legacy_clip_interpolation_precision, clip_precision_names, .profile = GLC_LEGACY)
{
    GLint original;
    GLuint texture, fb = precision_target(&original, &texture);
    const double plane[] = {1, 1, 0, .2};
    glClipPlane(GL_CLIP_PLANE0, plane);
    if (glc_variant == 1 || glc_variant == 3) glEnable(GL_CLIP_PLANE0);
    if (glc_variant == 2 || glc_variant == 3) {
        GLuint program = glc_program(
            "#version 120\nvarying vec4 color;void main(){gl_Position=gl_Vertex;"
            "gl_ClipVertex=gl_Vertex;color=gl_Color;}",
            "#version 120\nvarying vec4 color;void main(){gl_FragColor=color;}", NULL);
        glUseProgram(program);
    }
    /* Original triangle, including its nontrivial z and alpha values. */
    const float vertices[3][8] = {
        {-.9f, -.8f, -.5f, 1, 1, 0, 0, .75f},
        {.8f, -.9f, .2f, 1, 0, 1, 0, .5f},
        {.1f, .9f, .6f, 1, 0, 0, 1, 1}
    };
    if (glc_variant < 4) {
        glBegin(GL_TRIANGLES);
        for (int i = 0; i < 3; ++i) {
            glColor4fv(vertices[i] + 4);
            glVertex4fv(vertices[i]);
        }
        glEnd();
    } else {
        float polygon[4][8];
        /* The single outside vertex produces AB, B, C, CA. Keep the
           clipping arithmetic independently selectable at each precision. */
        for (int edge = 0; edge < 2; ++edge) {
            int a = edge == 0 ? 0 : 2, b = edge == 0 ? 1 : 0;
            double da = (double)vertices[a][0] + vertices[a][1] + .2;
            double db = (double)vertices[b][0] + vertices[b][1] + .2;
            if (glc_variant == 4) {
                da = (float)((float)(vertices[a][0] + vertices[a][1]) + .2f);
                db = (float)((float)(vertices[b][0] + vertices[b][1]) + .2f);
            }
            double t = da / (da - db);
            if (glc_variant == 4) t = (float)((float)da / (float)((float)da - (float)db));
            for (int c = 0; c < 8; ++c) {
                double value = vertices[a][c] + t * ((double)vertices[b][c] - vertices[a][c]);
                if (glc_variant == 4) {
                    volatile float delta = vertices[b][c] - vertices[a][c];
                    volatile float product = (float)t * delta;
                    value = (float)(vertices[a][c] + product);
                }
                polygon[edge == 0 ? 0 : 3][c] = (float)value;
            }
        }
        for (int c = 0; c < 8; ++c) { polygon[1][c] = vertices[1][c]; polygon[2][c] = vertices[2][c]; }
        glBegin(GL_TRIANGLE_FAN);
        for (int i = 0; i < 4; ++i) { glColor4fv(polygon[i] + 4); glVertex4fv(polygon[i]); }
        glEnd();
    }
    const int x[] = {45, 47};
    const int y[] = {18, 15};
    for (int i = 0; i < 2; ++i) {
        float color[4];
        glReadPixels(x[i], y[i], 1, 1, GL_RGBA, GL_FLOAT, color);
        fprintf(stderr, "Clip precision %s (%d,%d)=%a/%a/%a/%a red255=%a\n",
                clip_precision_names[glc_variant], x[i], y[i], color[0], color[1], color[2], color[3], color[0] * 255);
    }
    precision_finish(original, fb, texture);
}

static const char *const texture_precision_names[] = {"ff", "shader", "tex0", "tex1", "modulate"};
GLC_CASE_VARIANTS(legacy_multitexture_precision, texture_precision_names, .profile = GLC_LEGACY)
{
    GLint original;
    GLuint target, fb = precision_target(&original, &target);
    GLuint textures[2];
    glGenTextures(2, textures);
    for (int unit = 0; unit < 2; ++unit) {
        int size = unit ? 4 : 16;
        unsigned char pixels[16 * 16 * 4];
        glc_pattern_rgba8(pixels, size, size, unit ? 12 : 11);
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, textures[unit]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, unit ? GL_LINEAR : GL_NEAREST);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, unit ? GL_ADD : GL_MODULATE);
    }
    if (glc_variant) {
        const char *outputs[] = {
            "", "gl_FragColor=vec4(clamp(m.rgb+b.rgb,0.0,1.0),m.a*b.a);",
            "gl_FragColor=a;", "gl_FragColor=b;", "gl_FragColor=m;"
        };
        char fragment[1024];
        snprintf(fragment, sizeof fragment,
                 "#version 120\nuniform sampler2D t0,t1;varying vec2 uv0,uv1;varying vec4 color;"
                 "void main(){vec4 a=texture2D(t0,uv0),b=texture2D(t1,uv1);vec4 m=a*color;%s}",
                 outputs[glc_variant]);
        GLuint program = glc_program(
            "#version 120\nvarying vec2 uv0,uv1;varying vec4 color;void main(){"
            "gl_Position=gl_Vertex;uv0=gl_MultiTexCoord0.xy;uv1=gl_MultiTexCoord1.xy;color=gl_Color;}",
            fragment, NULL);
        glUseProgram(program);
        glUniform1i(glGetUniformLocation(program, "t0"), 0);
        glUniform1i(glGetUniformLocation(program, "t1"), 1);
    }
    glBegin(GL_QUADS);
    glColor3f(.7f, .7f, .7f);
    glMultiTexCoord2f(GL_TEXTURE0, 0, 0); glMultiTexCoord2f(GL_TEXTURE1, .5f, .5f); glVertex2f(-.9f, -.9f);
    glMultiTexCoord2f(GL_TEXTURE0, 2, 0); glMultiTexCoord2f(GL_TEXTURE1, 1, .5f); glVertex2f(.9f, -.9f);
    glMultiTexCoord2f(GL_TEXTURE0, 2, 2); glMultiTexCoord2f(GL_TEXTURE1, 1, 1); glVertex2f(.9f, .9f);
    glMultiTexCoord2f(GL_TEXTURE0, 0, 2); glMultiTexCoord2f(GL_TEXTURE1, .5f, 1); glVertex2f(-.9f, .9f);
    glEnd();
    const int x[] = {51, 53, 40, 30};
    const int y[] = {41, 28, 26, 25};
    for (int i = 0; i < 4; ++i) {
        float color[4];
        glReadPixels(x[i], y[i], 1, 1, GL_RGBA, GL_FLOAT, color);
        fprintf(stderr, "Texture precision %s (%d,%d)=%a/%a/%a/%a blue255=%a\n",
                texture_precision_names[glc_variant], x[i], y[i], color[0], color[1], color[2], color[3], color[2] * 255);
    }
    /* Disable texture state before displaying the diagnostic target. */
    glUseProgram(0);
    for (int unit = 0; unit < 2; ++unit) { glActiveTexture(GL_TEXTURE0 + unit); glDisable(GL_TEXTURE_2D); }
    precision_finish(original, fb, target);
    glDeleteTextures(2, textures);
}
