/* Standalone raw endpoint and native coverage diagnostic. No analytic image
 * assertions: root compares providers and the optional interval trim. */
#include "glc_gl_legacy.h"
#include "glcompare.h"
#include <math.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *const endpoint_modes[] = {"perspective", "clipped"};
static uint32_t endpoint_bits(float value)
{
    uint32_t bits; memcpy(&bits, &value, sizeof bits); return bits;
}

GLC_CASE_VARIANTS(legacy_wide_projected_endpoints, endpoint_modes, .profile = GLC_LEGACY)
{
    const struct timespec pause = {0, 50000000};
    GLint original;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &original);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    if (!glc_variant) glFrustum(-1, 1, -1, 1, 1, 10);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    const GLdouble plane[] = {1, .3, 0, .1};
    if (glc_variant) glClipPlane(GL_CLIP_PLANE0, plane);
    glDisable(GL_CLIP_PLANE0); glDisable(GL_DEPTH_TEST);
    glDisable(GL_MULTISAMPLE); glDisable(GL_LINE_SMOOTH);
    glDisable(GL_LIGHTING); glDisable(GL_FOG); glDisable(GL_TEXTURE_2D);
    GLuint program = glc_program(
        "#version 120\nvarying vec4 rawClip;varying vec4 rawEye;varying float rawDistance;"
        "uniform vec4 plane;void main(){rawClip=gl_ModelViewProjectionMatrix*gl_Vertex;"
        "rawEye=gl_ModelViewMatrix*gl_Vertex;rawDistance=dot(plane,rawEye);"
        "gl_Position=vec4(0,0,0,1);gl_PointSize=1.;}",
        "#version 120\nvarying vec4 rawClip;varying vec4 rawEye;varying float rawDistance;"
        "uniform int field;void main(){gl_FragColor=field==0?rawClip:field==1?rawEye:vec4(rawDistance);}", NULL);
    GLuint texture, framebuffer;
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F_ARB, 1, 1, 0, GL_RGBA, GL_FLOAT, NULL);
    glGenFramebuffers(1, &framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        glc_fail("Raw endpoint framebuffer incomplete");
    glViewport(0, 0, 1, 1); glUseProgram(program);
    glUniform4f(glGetUniformLocation(program, "plane"), 1, .3f, 0, .1f);
    GLint field = glGetUniformLocation(program, "field");
    float points[4][2][3], raw[4][2][3][4];
    for (int row = 0; row < 4; ++row) {
        float y = -.7f + .43f * row;
        points[row][0][0] = -.8f; points[row][0][1] = y;
        points[row][0][2] = glc_variant ? 0 : -1.1f;
        points[row][1][0] = .75f * (glc_variant ? 1 : 3);
        points[row][1][1] = (y + .1f) * (glc_variant ? 1 : 3);
        points[row][1][2] = glc_variant ? 0 : -3;
        for (int end = 0; end < 2; ++end) for (int f = 0; f < 3; ++f) {
            glUniform1i(field, f);
            glClear(GL_COLOR_BUFFER_BIT); glBegin(GL_POINTS);
            glVertex3fv(points[row][end]); glEnd();
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, raw[row][end][f]);
            fprintf(stderr, "RawEndpoint mode%s row%d end%d field%d bits%08x,%08x,%08x,%08x values%a,%a,%a,%a\n",
                endpoint_modes[glc_variant], row, end, f,
                endpoint_bits(raw[row][end][f][0]), endpoint_bits(raw[row][end][f][1]),
                endpoint_bits(raw[row][end][f][2]), endpoint_bits(raw[row][end][f][3]),
                raw[row][end][f][0], raw[row][end][f][1], raw[row][end][f][2], raw[row][end][f][3]);
            nanosleep(&pause, NULL);
        }
    }
    glUseProgram(0); glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)original);
    glViewport(0, 0, 64, 64); if (glc_variant) glEnable(GL_CLIP_PLANE0);
    glClearColor(0, 0, 0, 1);
    const float widths[] = {2, 3, 4.4f, 5.6f};
    for (int row = 0; row < 4; ++row) {
        double clip[2][4];
        for (int end = 0; end < 2; ++end) for (int c = 0; c < 4; ++c) clip[end][c] = raw[row][end][0][c];
        if (glc_variant) {
            double da = raw[row][0][2][0], db = raw[row][1][2][0];
            if ((da < 0) != (db < 0)) {
                int outside = da < 0 ? 0 : 1;
                double t = da / (da - db);
                for (int c = 0; c < 4; ++c) clip[outside][c] = clip[0][c] + t * (clip[1][c] - clip[0][c]);
            }
        }
        double window[2][2];
        for (int end = 0; end < 2; ++end) {
            if (!(clip[end][3] > 0) || !isfinite(clip[end][3])) glc_fail("Raw endpoint W invalid");
            for (int c = 0; c < 2; ++c) {
                window[end][c] = 32 * (clip[end][c] / clip[end][3] + 1);
                if (!isfinite(window[end][c]) || window[end][c] <= INT_MIN + 1.0 || window[end][c] >= INT_MAX - 1.0)
                    glc_fail("Projected endpoint outside diagnostic bounds");
            }
        }
        int major = fabs(window[1][0] - window[0][0]) > fabs(window[1][1] - window[0][1]) ? 0 : 1;
        double lo = floor(fmin(window[0][major], window[1][major]) * 256 + .5) / 256;
        double hi = floor(fmax(window[0][major], window[1][major]) * 256 + .5) / 256;
        int first = (int)ceil(lo - .5), end = (int)ceil(hi - .5);
        fprintf(stderr, "ProjectedEndpoint mode%s row%d window%.17g,%.17g/%.17g,%.17g major%d interval%d,%d\n",
            endpoint_modes[glc_variant], row, window[0][0], window[0][1], window[1][0], window[1][1], major, first, end);
        glClear(GL_COLOR_BUFFER_BIT); glLineWidth(widths[row]);
        if (getenv("GLM_WIDE_ENDPOINT_TRIM")) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(major ? 0 : first, major ? first : 0, major ? 64 : end - first, major ? end - first : 64);
        }
        glBegin(GL_LINES); glColor3f(.2f * row + .2f, 1, .3f); glVertex3fv(points[row][0]);
        glColor3f(1, .1f * row + .2f, .6f); glVertex3fv(points[row][1]); glEnd();
        glDisable(GL_SCISSOR_TEST);
        unsigned char pixels[64 * 64 * 4]; glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        for (int minor = 0; minor < 64; ++minor) {
            int begin = 64, finish = -1;
            for (int m = 0; m < 64; ++m) {
                int x = major ? minor : m, y = major ? m : minor;
                const unsigned char *p = pixels + (y * 64 + x) * 4;
                if (p[0] || p[1] || p[2]) { if (begin == 64) begin = m; finish = m; }
            }
            if (finish >= 0) fprintf(stderr, "EndpointCoverage mode%s row%d minor%d covered%d,%d outside%d\n",
                endpoint_modes[glc_variant], row, minor, begin, finish, begin < first || finish >= end);
        }
        nanosleep(&pause, NULL);
    }
    glDisable(GL_CLIP_PLANE0); glDeleteProgram(program);
    glDeleteFramebuffers(1, &framebuffer); glDeleteTextures(1, &texture);
    GLenum error = glGetError(); if (error) glc_fail("Raw endpoint diagnostic GL error %x", error);
}
