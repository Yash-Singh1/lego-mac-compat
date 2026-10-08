/* Apple's recorded legacy profile advertises GL_ARB_depth_clamp. */
#include "glc_gl_legacy.h"
#include "glcompare.h"
#include <stdio.h>

GLC_CASE(legacy_depth_clamp_fixed_function, .profile = GLC_LEGACY, .tolerance = 1)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_LIGHTING);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_CLAMP);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDepthRange(0, 1);
    glClearDepth(1);
    glClearColor(.2f, .3f, .4f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor4f(0, 1, 0, 1);
    glBegin(GL_TRIANGLE_FAN);
    glVertex3f(-.9f, -.9f, -1.5f);
    glVertex3f(.9f, -.85f, 1.5f);
    glVertex3f(.85f, .9f, 1.5f);
    glVertex3f(-.85f, .85f, -1.5f);
    glEnd();
    const int x[] = {8, 32, 56};
    float depth[3];
    GLubyte color[3][4];
    for (int i = 0; i < 3; ++i) {
        glReadPixels(x[i], 32, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, depth + i);
        glReadPixels(x[i], 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, color[i]);
    }
    fprintf(stderr, "Fixed-function depth clamp depths=%a/%a/%a green=%u/%u/%u\n",
            depth[0], depth[1], depth[2], color[0][1], color[1][1], color[2][1]);
    GLenum error = glGetError();
    if (error) glc_fail("Fixed-function depth clamp GL error %x", error);
    if (depth[0] != 0 || depth[2] != 1 || color[0][1] != 255 || color[2][1] != 77)
        glc_fail("Fixed-function depth clamp bounds or far-plane LESS coverage differ");
}
