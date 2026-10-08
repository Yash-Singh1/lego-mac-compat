#include "glc_gl_legacy.h"
#include "glcompare.h"

static const char *const interval_names[] = {"perspective_scissor", "clipped_scissor", "perspective_ordering", "clipped_ordering", "perspective_blend", "clipped_stencil"};

GLC_CASE_VARIANTS(legacy_wide_projected_interval, interval_names, .profile = GLC_LEGACY)
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
    bool clipped = glc_variant == 1 || glc_variant == 3 || glc_variant == 5;
    if (!clipped) glFrustum(-1, 1, -1, 1, 1, 10);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    const GLdouble plane[] = {1, .3, 0, .1};
    if (clipped) { glClipPlane(GL_CLIP_PLANE0, plane); glEnable(GL_CLIP_PLANE0); }
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
    if (glc_variant <= 1) { glEnable(GL_SCISSOR_TEST); glScissor(6, 4, 50, 54); }
    const float widths[] = {2, 3, 4.4f, 5.6f};
    for (int row = 0; row < 4; ++row) {
        float y = -.7f + .43f * row;
        float p[2][3] = {{-.8f, y, clipped ? 0 : -1.1f},
            {.75f * (clipped ? 1 : 3), (y + .1f) * (clipped ? 1 : 3), clipped ? 0 : -3}};
        glLineWidth(widths[row]);
        int repeats = glc_variant == 4 ? 2 : 1;
        for (int repeat = 0; repeat < repeats; ++repeat) {
            glColor4f(.25f, .5f, .75f, glc_variant == 4 ? .5f : 1);
            glBegin(GL_LINES);
            glVertex3fv(p[0]); glVertex3fv(p[1]);
            glEnd();
        }
    }
    if (glc_variant <= 1) {
        GLint box[4]; glGetIntegerv(GL_SCISSOR_BOX, box);
        if (!glIsEnabled(GL_SCISSOR_TEST) || box[0] != 6 || box[1] != 4 || box[2] != 50 || box[3] != 54)
            glc_fail("Major interval did not restore application scissor");
        glDisable(GL_SCISSOR_TEST);
    } else if (glIsEnabled(GL_SCISSOR_TEST)) glc_fail("Major interval leaked internal scissor");
    if (glc_variant == 2 || glc_variant == 3) {
        glDisable(GL_CLIP_PLANE0);
        glMatrixMode(GL_PROJECTION); glLoadIdentity();
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
        int touched = 0;
        for (int i = 0; i < 64 * 64; ++i) {
            if (values[i] > 1) glc_fail("Wide interval duplicate stencil write");
            touched += values[i] != 0;
        }
        if (!touched) glc_fail("Projected interval wrote no stencil samples");
        glDisable(GL_STENCIL_TEST);
    }
    if (glc_variant == 4) {
        unsigned char pixels[64 * 64][4];
        glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        int touched = 0;
        for (int i = 0; i < 64 * 64; ++i) {
            if (!pixels[i][0] && !pixels[i][1] && !pixels[i][2]) continue;
            ++touched;
            if (pixels[i][0] != 48 || pixels[i][1] != 96 || pixels[i][2] != 144)
                glc_fail("Projected interval lost draw order or repeated blend at pixel %d: %u/%u/%u", i,
                         pixels[i][0], pixels[i][1], pixels[i][2]);
        }
        if (!touched) glc_fail("Projected interval wrote no blended samples");
    }
    glDisable(GL_BLEND);
    glDisable(GL_CLIP_PLANE0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)original);
    glBlitFramebuffer(0, 0, 64, 64, 0, 0, 64, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)original);
    GLenum error = glGetError();
    if (error) glc_fail("Wide interval GL error %x", error);
    glDeleteRenderbuffers(1, &stencil);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
}
