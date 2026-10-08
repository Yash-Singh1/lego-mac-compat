#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* Use the same triangle as the legacy ClipVertex diagnostic to distinguish
   legacy software clipping from core ClipDistance interpolation. */
static const char *const clip_modes[] = {"unclipped", "clipped", "precise_clipped"};
GLC_CASE_VARIANTS(core_clip_interpolation_precision, clip_modes, .profile = GLC_CORE)
{
    char vertex[512];
    snprintf(vertex, sizeof vertex,
             "#version 410 core\nlayout(location=0) in vec4 position;"
             "layout(location=1) in vec4 color;uniform vec4 plane;out vec4 shade;"
             "void main(){gl_Position=position;shade=color;"
             "%s float distance=dot(position,plane);gl_ClipDistance[0]=distance;}",
             glc_variant == 2 ? "precise" : "");
    GLuint program = glc_program(vertex,
        "#version 410 core\nin vec4 shade;out vec4 color;void main(){color=shade;}", NULL);
    glUseProgram(program);
    glUniform4f(glGetUniformLocation(program, "plane"), 1, 1, 0, .2f);
    const float vertices[3][8] = {
        {-.9f, -.8f, -.5f, 1, 1, 0, 0, .75f},
        {.8f, -.9f, .2f, 1, 0, 1, 0, .5f},
        {.1f, .9f, .6f, 1, 0, 0, 1, 1}
    };
    GLuint vao, buffer, fb, texture;
    GLint original;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &original);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof vertices[0], NULL);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof vertices[0], (void *)(4 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 64, 64, 0, GL_RGBA, GL_FLOAT, NULL);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        glc_fail("Core clipping float framebuffer incomplete");
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    if (glc_variant) glEnable(GL_CLIP_DISTANCE0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    const int x[] = {45, 47}, y[] = {18, 15};
    for (int i = 0; i < 2; ++i) {
        float rgba[4];
        glReadPixels(x[i], y[i], 1, 1, GL_RGBA, GL_FLOAT, rgba);
        fprintf(stderr, "Core clip precision %s (%d,%d)=%a/%a/%a/%a\n",
                clip_modes[glc_variant], x[i], y[i], rgba[0], rgba[1], rgba[2], rgba[3]);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)original);
    glBlitFramebuffer(0, 0, 64, 64, 0, 0, 64, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)original);
    glDisable(GL_CLIP_DISTANCE0);
    GLenum error = glGetError();
    if (error) glc_fail("Core clipping GL error %x", error);
    glDeleteTextures(1, &texture);
    glDeleteFramebuffers(1, &fb);
    glDeleteBuffers(1, &buffer);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
}

/* Diagnose intersection arithmetic independently of the primitive topology. */
static const char *const quad_clip_modes[] = {"fan", "triangles", "cpu_float", "cpu_double", "cpu_fma"};
GLC_CASE_VARIANTS(core_clip_quad_precision, quad_clip_modes, .profile = GLC_CORE)
{
    const float input[4][8] = {
        {-.9f,-.9f,0,1, 1,0,0,1}, {.9f,-.85f,0,1, 0,1,0,.8f},
        {.85f,.9f,0,1, 0,0,1,.6f}, {-.85f,.85f,0,1, 1,1,0,.4f}
    };
    float output[18][8]; int count = 0;
    if (glc_variant < 2) {
        for (int i = 0; i < (glc_variant ? 6 : 4); ++i) {
            const int indices[] = {0,1,2,0,2,3};
            memcpy(output[count++], input[glc_variant ? indices[i] : i], sizeof input[0]);
        }
    } else for (int tri = 0; tri < 2; ++tri) {
        const int indices[3] = {0,tri+1,tri+2};
        float polygon[6][8]; int n = 0;
        for (int edge = 0; edge < 3; ++edge) {
            const float *a = input[indices[edge]], *b = input[indices[(edge+1)%3]];
            double da = (double)a[0]+(double)a[1]*.5+(double).2f;
            double db = (double)b[0]+(double)b[1]*.5+(double).2f;
            if (glc_variant != 3) {da = (float)(a[0]+a[1]*.5f+.2f); db = (float)(b[0]+b[1]*.5f+.2f);}
            if (da >= 0) memcpy(polygon[n++], a, sizeof polygon[0]);
            if ((da >= 0) != (db >= 0)) {
                double t = da/(da-db);
                if (glc_variant != 3) t = (float)((float)da/(float)((float)da-(float)db));
                for (int c = 0; c < 8; ++c) {
                    double value = a[c]+t*((double)b[c]-a[c]);
                    if (glc_variant != 3) {
                        volatile float delta = b[c]-a[c];
                        volatile float product = (float)t*delta;
                        value = glc_variant == 4 ? fmaf((float)t,delta,a[c]) : (float)(a[c]+product);
                    }
                    polygon[n][c] = (float)value;
                }
                ++n;
            }
        }
        for (int i = 1; i+1 < n; ++i) {
            memcpy(output[count++],polygon[0],sizeof polygon[0]);
            memcpy(output[count++],polygon[i],sizeof polygon[0]);
            memcpy(output[count++],polygon[i+1],sizeof polygon[0]);
        }
    }
    GLuint program=glc_program(
        "#version 410 core\nlayout(location=0) in vec4 position;layout(location=1) in vec4 color;out vec4 v;"
        "void main(){gl_Position=position;gl_ClipDistance[0]=position.x+position.y*.5+.2;v=color;}",
        "#version 410 core\nin vec4 v;out vec4 frag;void main(){frag=v;}",NULL);
    GLuint vao,buffer;glGenVertexArrays(1,&vao);glBindVertexArray(vao);
    glGenBuffers(1,&buffer);glBindBuffer(GL_ARRAY_BUFFER,buffer);
    glBufferData(GL_ARRAY_BUFFER,count*sizeof output[0],output,GL_STATIC_DRAW);
    glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,sizeof output[0],NULL);
    glVertexAttribPointer(1,4,GL_FLOAT,GL_FALSE,sizeof output[0],(void *)(4*sizeof(float)));
    glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);
    if(glc_variant<2)glEnable(GL_CLIP_DISTANCE0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(glc_variant==0?GL_TRIANGLE_FAN:GL_TRIANGLES,0,count);
    glDisable(GL_CLIP_DISTANCE0);
    GLenum error=glGetError();if(error)glc_fail("Quad clip precision GL error %x",error);
    glDeleteBuffers(1,&buffer);glDeleteVertexArrays(1,&vao);glDeleteProgram(program);
}
