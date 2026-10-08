#include "glc_gl_legacy.h"
#include "glcompare.h"
#include "wide_symbolic_diamond.h"
#include <math.h>
#include <stdio.h>
#include <time.h>

static const char *const tip_names[] = {
    "above_45", "steep", "near_vertical", "diagonal_ratio", "x_major_control"
};
static const float slopes[5][2] = {
    {8, 8 + 1.0f / 256}, {4, 12}, {1.0f / 256, 12}, {4, 24}, {12, 4}
};
static const int translations[3][2] = {{30, 28}, {32, 32}, {36, 35}};

GLC_CASE_VARIANTS(legacy_wide_line_upper_tip, tip_names, .profile = GLC_LEGACY)
{
    glViewport(0, 0, 64, 64);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glUseProgram(0);
    glDisable(GL_MULTISAMPLE); glDisable(GL_LINE_SMOOTH);
    glDisable(GL_DEPTH_TEST); glDisable(GL_FOG);
    glDisable(GL_LIGHTING); glDisable(GL_TEXTURE_2D);
    glLineWidth(1); glClearColor(0, 0, 0, 1); glColor4f(1, 1, 1, 1);
    const struct timespec pause = {0, 50000000};
    unsigned errors = 0;
    for (int translation = 0; translation < 3; ++translation) {
        for (int x_sign = -1; x_sign <= 1; x_sign += 2) {
            for (int y_sign = -1; y_sign <= 1; y_sign += 2) {
                for (int shift = -1; shift <= 1; ++shift) {
                    int px = translations[translation][0], py = translations[translation][1];
                    float endpoint[2] = {px + .5f, py + 1.0f + shift / 256.0f};
                    float start[2] = {
                        endpoint[0] + x_sign * slopes[glc_variant][0],
                        endpoint[1] + y_sign * slopes[glc_variant][1]
                    };
                    int observed[2], expected[2];
                    for (int reverse = 0; reverse < 2; ++reverse) {
                        const float *a = reverse ? endpoint : start;
                        const float *b = reverse ? start : endpoint;
                        expected[reverse] = glm_probe_symbolic_diamond(
                            lroundf(a[0] * 256), lroundf(a[1] * 256),
                            lroundf(b[0] * 256), lroundf(b[1] * 256),
                            px * 256 + 128, py * 256 + 128);
                        glClear(GL_COLOR_BUFFER_BIT);
                        glBegin(GL_LINES);
                        glVertex2f(a[0] / 32 - 1, a[1] / 32 - 1);
                        glVertex2f(b[0] / 32 - 1, b[1] / 32 - 1);
                        glEnd();
                        unsigned char pixel[4];
                        glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
                        observed[reverse] = pixel[0] != 0;
                        if (observed[reverse] != expected[reverse]) ++errors;
                    }
                    fprintf(stderr,
                        "UpperTip %s translation%d signs%d,%d shift%d target%d,%d "
                        "start%.9g,%.9g endpoint%.9g,%.9g original%d reversed%d "
                        "expected%d,%d eligible%d duplicate%d\n",
                        tip_names[glc_variant], translation, x_sign, y_sign, shift, px, py,
                        start[0], start[1], endpoint[0], endpoint[1], observed[0], observed[1],
                        expected[0], expected[1], !observed[0] && observed[1], observed[0] && observed[1]);
                    nanosleep(&pause, NULL);
                }
            }
        }
    }
    GLenum error = glGetError();
    if (error) glc_fail("Upper-tip native GL error %x", error);
    if (errors) glc_fail("Upper-tip symbolic rule disagreed at %u native samples", errors);
}
