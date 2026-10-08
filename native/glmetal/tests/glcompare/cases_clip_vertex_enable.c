#include "glc_gl_legacy.h"
#include "glcompare.h"
#include <stdlib.h>

static const char *const masks[] = {"none", "plane0", "plane1", "both"};
GLC_CASE_VARIANTS(legacy_glsl_clip_plane_enable, masks, .profile = GLC_LEGACY)
{
    GLuint program = glc_program(
        "#version 120\nvoid main(){gl_Position=gl_Vertex;gl_ClipVertex=gl_Vertex;"
        "gl_FrontColor=vec4(gl_ClipPlane[0].w,gl_ClipPlane[1].w,0,1);}",
        "#version 120\nvoid main(){gl_FragColor=gl_Color;}", NULL);
    glUseProgram(program);
    const GLdouble planes[2][4] = {{1, 0, 0, .2}, {0, 1, 0, .3}};
    glClipPlane(GL_CLIP_PLANE0, planes[0]);
    glClipPlane(GL_CLIP_PLANE1, planes[1]);
    GLdouble baseline[2][4];
    glGetClipPlane(GL_CLIP_PLANE0, baseline[0]);
    glGetClipPlane(GL_CLIP_PLANE1, baseline[1]);
    const int passes[] = {glc_variant ^ 3, 0, glc_variant};
    for (int pass = 0; pass < 3; ++pass) {
        int mask = passes[pass];
        for (int p = 0; p < 2; ++p) {
            if (mask & (1 << p)) glEnable(GL_CLIP_PLANE0 + p);
            else glDisable(GL_CLIP_PLANE0 + p);
            GLdouble equation[4];
            glGetClipPlane(GL_CLIP_PLANE0 + p, equation);
            for (int c = 0; c < 4; ++c)
                if (equation[c] != baseline[p][c])
                    glc_fail("Clip plane %d enable changed component %d from %.17g to %.17g",
                             p, c, baseline[p][c], equation[c]);
        }
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS);
        glVertex2f(-.9f, -.9f);
        glVertex2f(.9f, -.9f);
        glVertex2f(.9f, .9f);
        glVertex2f(-.9f, .9f);
        glEnd();
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x) {
                GLubyte color[4];
                glReadPixels(x ? 56 : 8, y ? 56 : 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, color);
                int visible = !((mask & 1) && !x) && !((mask & 2) && !y);
                const int expected[] = {visible ? 51 : 0, visible ? 77 : 0, 0, visible ? 255 : 0};
                for (int c = 0; c < 4; ++c)
                    if (abs((int)color[c] - expected[c]) > 1)
                        glc_fail("Clip enable mask %d pass %d sample %d/%d component %d: %u != %d",
                                 mask, pass, x, y, c, color[c], expected[c]);
            }
    }
    GLenum error = glGetError();
    if (error) glc_fail("Clip vertex enable GL error %x", error);
}
