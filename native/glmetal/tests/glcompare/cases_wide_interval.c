#include "glc_gl_legacy.h"
#include "glcompare.h"

static const char *const interval_names[] = {"loop", "viewport", "ordering", "scissor", "blend", "stencil"};

GLC_CASE_VARIANTS(legacy_wide_interval, interval_names, .profile = GLC_LEGACY)
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
        glc_fail("Wide interval target incomplete");
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
    if (glc_variant == 5) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_ALWAYS, 0, 255);
        glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
    }
    if (glc_variant == 4) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    if (glc_variant == 3) { glEnable(GL_SCISSOR_TEST); glScissor(6, 4, 50, 54); }
    if (glc_variant == 1 || glc_variant == 5) glViewport(7, 9, 48, 40);
    const float widths[] = {2, 3, 4.4f, 5.6f};
    for (int row = 0; row < 4; ++row) {
        float y = -.7f + .43f * row;
        float p[3][2] = {{-.8f, y}, {.75f, y + .1f}, {.35f, y + .3f}};
        float colors[3][3] = {{.2f * row + .2f, 1, .3f}, {1, .1f * row + .2f, .6f}, {1, .1f * row + .2f, .6f}};
        glLineWidth(widths[row]);
        glBegin(glc_variant == 0 || glc_variant == 2 ? GL_LINE_LOOP : GL_LINES);
        int count = glc_variant == 0 || glc_variant == 2 ? 3 : 2;
        for (int v = 0; v < count; ++v) {
            glColor4f(colors[v][0], colors[v][1], colors[v][2], glc_variant == 4 ? .5f : 1);
            glVertex2fv(p[v]);
        }
        glEnd();
    }
    if (glc_variant == 3) {
        GLint box[4]; glGetIntegerv(GL_SCISSOR_BOX, box);
        if (!glIsEnabled(GL_SCISSOR_TEST) || box[0] != 6 || box[1] != 4 || box[2] != 50 || box[3] != 54)
            glc_fail("Major interval did not restore application scissor");
        glDisable(GL_SCISSOR_TEST);
    } else if (glIsEnabled(GL_SCISSOR_TEST)) glc_fail("Major interval leaked internal scissor");
    if (glc_variant == 2) {
        glColor3f(0, 1, 0);
        glBegin(GL_QUADS);
        glVertex2f(-.98f, -.98f); glVertex2f(-.8f, -.98f);
        glVertex2f(-.8f, -.8f); glVertex2f(-.98f, -.8f);
        glEnd();
        unsigned char pixel[4]; glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        if (pixel[0] || pixel[1] != 255 || pixel[2]) glc_fail("Major interval ordering or scissor restoration failed");
    }
    if (glc_variant == 5) {
        unsigned char values[64 * 64];
        glReadPixels(0, 0, 64, 64, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, values);
        for (int i = 0; i < 64 * 64; ++i)
            if (values[i] > 1) glc_fail("Wide interval duplicate stencil write");
        glDisable(GL_STENCIL_TEST);
    }
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)original);
    glBlitFramebuffer(0, 0, 64, 64, 0, 0, 64, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)original);
    GLenum error = glGetError();
    if (error) glc_fail("Wide interval GL error %x", error);
    glDeleteRenderbuffers(1, &stencil);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
}
