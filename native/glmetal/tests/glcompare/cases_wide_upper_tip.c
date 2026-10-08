#include "glc_gl_legacy.h"
#include "glcompare.h"

static const char *const upper_tip_names[] = {"original_diagonal", "steep", "above_45", "reverse", "stencil"};

GLC_CASE_VARIANTS(legacy_wide_upper_tip, upper_tip_names, .profile = GLC_LEGACY)
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
        glc_fail("Wide upper tip target incomplete");
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
    if (glc_variant == 4) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_ALWAYS, 0, 255);
        glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
    }
    glColor4f(.25f, .75f, .5f, 1);
    float ax, ay, bx, by;
    if (glc_variant == 0 || glc_variant == 4) {
        /* Original diagonal, including its two formerly missing tip samples. */
        ax = 8; ay = 6.4f; bx = 16; by = 56;
    } else if (glc_variant == 1) {
        ax = 27; ay = 21; bx = 31; by = 33;
    } else if (glc_variant == 2) {
        ax = 23; ay = 33 - (8 + 1.0f / 256); bx = 31; by = 33;
    } else {
        ax = 31; ay = 33; bx = 27; by = 21;
    }
    glBegin(GL_LINES);
    glVertex2f(ax / 32 - 1, ay / 32 - 1);
    glVertex2f(bx / 32 - 1, by / 32 - 1);
    glEnd();
    if (glc_variant == 4) {
        unsigned char values[64 * 64];
        glReadPixels(0, 0, 64, 64, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, values);
        for (int i = 0; i < 64 * 64; ++i)
            if (values[i] > 1) glc_fail("Wide upper tip duplicate stencil write");
        if (values[55 * 64 + 15] != 1 || values[55 * 64 + 16] != 1)
            glc_fail("Wide upper-tip terminal coverage missing");
        glDisable(GL_STENCIL_TEST);
    }
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)original);
    glBlitFramebuffer(0, 0, 64, 64, 0, 0, 64, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)original);
    GLenum error = glGetError();
    if (error) glc_fail("Wide upper tip GL error %x", error);
    glDeleteRenderbuffers(1, &stencil);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
}
