#include "glc_gl_legacy.h"
#include "glcompare.h"

static const char *const line_modes[] = {
    "horizontal", "vertical", "diagonal", "strip", "loop", "arrays", "indexed",
    "perspective", "flat", "viewport", "clipped", "state_restore"
};

GLC_CASE_VARIANTS(legacy_wide_lines, line_modes, .profile = GLC_LEGACY,
                  .tolerance = 1, .outlier_fraction = 0.01)
{
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_LINE_SMOOTH);
    glDisable(GL_MULTISAMPLE);
    if (glc_variant == 7) {
        glMatrixMode(GL_PROJECTION);
        glFrustum(-1, 1, -1, 1, 1, 10);
        glMatrixMode(GL_MODELVIEW);
    }
    if (glc_variant == 8) glShadeModel(GL_FLAT);
    if (glc_variant == 9) glViewport(7, 9, 48, 40);
    if (glc_variant == 10) {
        const GLdouble plane[] = {1, 0.3, 0, 0.1};
        glClipPlane(GL_CLIP_PLANE0, plane);
        glEnable(GL_CLIP_PLANE0);
    }
    for (int i = 0; i < 4; ++i) {
        const GLfloat widths[] = {2, 3, 4.4f, 5.6f};
        float y = -0.7f + 0.43f * i;
        GLfloat points[3][3] = {{-0.8f, y, 0}, {0.75f, y + 0.1f, 0}, {0.35f, y + 0.3f, 0}};
        if (glc_variant == 0) points[1][1] = y;
        if (glc_variant == 1) {
            points[0][0] = points[1][0] = y;
            points[0][1] = -0.8f;
            points[1][1] = 0.75f;
        }
        if (glc_variant == 2) {
            points[0][0] = -0.75f + 0.4f * i;
            points[0][1] = -0.8f;
            points[1][0] = points[0][0] + 0.25f;
            points[1][1] = 0.75f;
        }
        if (glc_variant == 7) {
            points[0][2] = -1.1f;
            points[1][0] *= 3;
            points[1][1] *= 3;
            points[1][2] = -3.0f;
        }
        glLineWidth(widths[i]);
        glColor3f(0.2f * i + 0.2f, 1, 0.3f);
        if (glc_variant == 5 || glc_variant == 6) {
            glEnableClientState(GL_VERTEX_ARRAY);
            glVertexPointer(3, GL_FLOAT, 0, points);
            if (glc_variant == 5) glDrawArrays(GL_LINES, 0, 2);
            else {
                const GLubyte indices[] = {1, 0};
                glDrawElements(GL_LINES, 2, GL_UNSIGNED_BYTE, indices);
            }
            glDisableClientState(GL_VERTEX_ARRAY);
        } else {
            glBegin(glc_variant == 3 ? GL_LINE_STRIP : glc_variant == 4 ? GL_LINE_LOOP : GL_LINES);
            glVertex3fv(points[0]);
            glColor3f(1, 0.1f * i + 0.2f, 0.6f);
            glVertex3fv(points[1]);
            if (glc_variant == 3 || glc_variant == 4) glVertex3fv(points[2]);
            glEnd();
        }
    }
    if (glc_variant == 11) {
        glLineWidth(1);
        glColor3f(0, 0, 1);
        glBegin(GL_TRIANGLES);
        glVertex2f(-0.2f, -0.2f);
        glVertex2f(0.2f, -0.2f);
        glVertex2f(0, 0.2f);
        glEnd();
    }
}
