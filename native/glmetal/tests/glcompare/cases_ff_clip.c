#include "glc_gl_legacy.h"
#include "glcompare.h"
#include <stdlib.h>

static const char *const clip_modes[] = {
    "smooth", "flat", "arrays", "elements", "matrices", "textured", "multiple_planes", "inside"
};
GLC_CASE_VARIANTS(legacy_linear_user_clip, clip_modes, .profile = GLC_LEGACY, .tolerance = 1)
{
    const GLfloat vertices[][8] = {
        {-.9f, -.8f, -.5f, 1, 1, 0, 0, .75f},
        {.8f, -.9f, .2f, 1, 0, 1, 0, .5f},
        {.1f, .9f, .6f, 1, 0, 0, 1, 1}
    };
    const GLdouble plane[] = {1, 1, 0, .2};
    glClipPlane(GL_CLIP_PLANE0, plane);
    glEnable(GL_CLIP_PLANE0);
    if (glc_variant == 1) glShadeModel(GL_FLAT);
    if (glc_variant == 4) {
        glMatrixMode(GL_MODELVIEW);
        glTranslatef(.1f, -.05f, 0);
        glRotatef(17, 0, 0, 1);
        glScalef(.8f, 1.1f, 1);
        glMatrixMode(GL_PROJECTION);
        glScalef(.9f, .85f, 1);
        glMatrixMode(GL_MODELVIEW);
    }
    GLuint texture = 0;
    if (glc_variant == 5) {
        GLubyte pixels[8 * 8 * 4];
        glc_pattern_rgba8(pixels, 8, 8, 37);
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glMatrixMode(GL_TEXTURE);
        glTranslatef(.125f, .25f, 0);
        glScalef(.75f, .5f, 1);
        glMatrixMode(GL_MODELVIEW);
    }
    if (glc_variant == 6) {
        const GLdouble second[] = {-1, .3, 0, .35};
        const GLdouble third[] = {0, -1, 0, .65};
        glClipPlane(GL_CLIP_PLANE1, second);
        glClipPlane(GL_CLIP_PLANE2, third);
        glEnable(GL_CLIP_PLANE1);
        glEnable(GL_CLIP_PLANE2);
    }
    if (glc_variant == 7) {
        const GLdouble inside[] = {0, 0, 0, 1};
        glClipPlane(GL_CLIP_PLANE0, inside);
    }
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    if (glc_variant == 2 || glc_variant == 3) {
        GLuint buffers[2];
        glGenBuffers(2, buffers);
        glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
        glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glVertexPointer(4, GL_FLOAT, sizeof vertices[0], NULL);
        glColorPointer(4, GL_FLOAT, sizeof vertices[0], (const void *)(4 * sizeof(GLfloat)));
        if (glc_variant == 2) glDrawArrays(GL_TRIANGLES, 0, 3);
        else {
            const GLushort indices[] = {0, 1, 2};
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, indices, GL_STATIC_DRAW);
            glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, NULL);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        }
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDeleteBuffers(2, buffers);
    } else {
        glBegin(GL_TRIANGLES);
        for (int i = 0; i < 3; ++i) {
            glColor4fv(vertices[i] + 4);
            glTexCoord2f(i == 1 ? 1 : 0, i == 2 ? 1 : 0);
            glVertex4fv(vertices[i]);
        }
        glEnd();
    }
    GLenum error = glGetError();
    if (error) glc_fail("Linear user clip %s GL error %x", clip_modes[glc_variant], error);
    if (texture) glDeleteTextures(1, &texture);
}

GLC_CASE(legacy_linear_clip_batches, .profile = GLC_LEGACY, .tolerance = 1)
{
    enum { TRIANGLES = 7000, STRIDE = 8 };
    GLfloat *vertices = malloc((size_t)TRIANGLES * 3 * STRIDE * sizeof(GLfloat));
    if (!vertices) glc_fail("Clip batch allocation failed");
    for (int t = 0; t < TRIANGLES; ++t) {
        float x = -.95f + (t % 64) * .03f;
        float y = -.95f + ((t / 64) % 64) * .03f;
        for (int v = 0; v < 3; ++v) {
            GLfloat *p = vertices + (size_t)(t * 3 + v) * STRIDE;
            p[0] = x + (v == 1 ? .025f : 0);
            p[1] = y + (v == 2 ? .025f : 0);
            p[2] = 0; p[3] = 1;
            p[4] = t >= 4096 ? .9f : .2f;
            p[5] = .5f; p[6] = .7f; p[7] = 1;
        }
    }
    const GLdouble plane[] = {1, .5, 0, .1};
    glClipPlane(GL_CLIP_PLANE0, plane);
    glEnable(GL_CLIP_PLANE0);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(4, GL_FLOAT, STRIDE * sizeof(GLfloat), vertices);
    glColorPointer(4, GL_FLOAT, STRIDE * sizeof(GLfloat), vertices + 4);
    glDrawArrays(GL_TRIANGLES, 0, TRIANGLES * 3);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    free(vertices);
    GLenum error = glGetError();
    if (error) glc_fail("Linear clip batch GL error %x", error);
}
