#include "glc_gl_legacy.h"
#include "glcompare.h"

static const char *const boundary_names[] = {"edges", "blend", "stencil", "scissor"};

GLC_CASE_VARIANTS(legacy_wide_boundary, boundary_names, .profile = GLC_LEGACY)
{
    GLint original;
    GLuint texture, framebuffer, stencil;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &original);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    glGenRenderbuffers(1, &stencil);
    glBindRenderbuffer(GL_RENDERBUFFER, stencil);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 64, 64);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stencil);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        glc_fail("Wide boundary target incomplete");
    glViewport(0, 0, 64, 64);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glUseProgram(0);
    glDisable(GL_DEPTH_TEST); glDisable(GL_MULTISAMPLE);
    glDisable(GL_LINE_SMOOTH); glDisable(GL_FOG);
    glDisable(GL_LIGHTING); glDisable(GL_TEXTURE_2D);
    glClearColor(0, 0, 0, 1); glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glLineWidth(2);
    if (glc_variant == 1) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    if (glc_variant == 2) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_ALWAYS, 0, 255);
        glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
    }
    if (glc_variant == 3) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(8, 5, 48, 54);
    }
    /* Each row tests one slanted edge. Mirroring the approach above/below
     * tests both slope directions. Width-two replicas put the endpoints
     * exactly on adjacent diamonds without sharing samples between lines. */
    for (int edge = 0; edge < 4; ++edge) {
        for (int slope = 0; slope < 2; ++slope) {
            float end_x = edge & 1 ? 44.75f : 20.25f;
            float start_x = edge & 1 ? 20.25f : 44.75f;
            float row = 8.0f + edge * 12.0f + slope * 4.0f;
            float end_y = row + (edge < 2 ? .25f : -.25f);
            float start_y = end_y + (slope ? 2.0f : -2.0f);
            glColor4f(.25f + edge * .125f, .75f, .25f, glc_variant == 1 ? .5f : 1.0f);
            glBegin(GL_LINES);
            glVertex2f(start_x / 32 - 1, start_y / 32 - 1);
            glVertex2f(end_x / 32 - 1, end_y / 32 - 1);
            glEnd();
        }
    }
    if (glc_variant == 2) {
        unsigned char values[64 * 64];
        glReadPixels(0, 0, 64, 64, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, values);
        for (int i = 0; i < 64 * 64; ++i)
            if (values[i] > 1) glc_fail("Wide slanted boundary duplicate stencil write");
        glDisable(GL_STENCIL_TEST);
    }
    if (glc_variant == 3) {
        GLint box[4];
        glGetIntegerv(GL_SCISSOR_BOX, box);
        if (!glIsEnabled(GL_SCISSOR_TEST) || box[0] != 8 || box[1] != 5 || box[2] != 48 || box[3] != 54)
            glc_fail("Wide boundary changed application scissor");
        glDisable(GL_SCISSOR_TEST);
    }
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)original);
    glBlitFramebuffer(0, 0, 64, 64, 0, 0, 64, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)original);
    GLenum error = glGetError();
    if (error) glc_fail("Wide boundary GL error %x", error);
    glDeleteRenderbuffers(1, &stencil);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
}
