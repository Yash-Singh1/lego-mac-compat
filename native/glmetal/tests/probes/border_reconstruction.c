/* Standalone float readback diagnostic. Define GLM_BORDER_LEGACY for legacy formats. */
#ifdef GLM_BORDER_LEGACY
#include "glc_gl_legacy.h"
#define GL_RGBA32F GL_RGBA32F_ARB
#else
#include "glc_gl_core.h"
#endif
#include "glcompare.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef GLM_BORDER_LEGACY
static const char *const modes[] = {"alpha_endpoints", "luminance_endpoints", "la_endpoints", "intensity_endpoints",
                                   
#ifndef GLM_BORDER_STRICT
                                   "alpha_fractional", "luminance_fractional", "la_fractional", "intensity_fractional"
#endif
};
static const GLenum formats[] = {GL_ALPHA8, GL_LUMINANCE8, GL_LUMINANCE8_ALPHA8, GL_INTENSITY8};
static const int channels[][4] = {{-1,-1,-1,3}, {0,0,0,-2}, {0,0,0,3}, {0,0,0,0}};
#define FORMAT_COUNT 4
#define PROBE_PROFILE GLC_LEGACY
#define PROBE_NAME border_reconstruction_legacy
#else
static const char *const modes[] = {"rgba_endpoints", "rgb_endpoints", "red_endpoints", "rg_endpoints", "swizzle_endpoints",
                                   "srgba_endpoints", "srgb_endpoints",
#ifndef GLM_BORDER_STRICT
                                   "rgba_fractional", "rgb_fractional", "red_fractional", "rg_fractional",
                                   "swizzle_fractional", "srgba_fractional", "srgb_fractional"
#endif
};
static const GLenum formats[] = {GL_RGBA8, GL_RGB8, GL_R8, GL_RG8, GL_RGBA8, GL_SRGB8_ALPHA8, GL_SRGB8};
static const int channels[][4] = {{0,1,2,3}, {0,1,2,-2}, {0,-1,-1,-2}, {0,1,-1,-2}, {2,0,-2,-1}, {0,1,2,3}, {0,1,2,-2}};
#define FORMAT_COUNT 7
#define PROBE_PROFILE GLC_CORE
#define PROBE_NAME border_reconstruction_core
#endif

GLC_CASE_VARIANTS(PROBE_NAME, modes, .profile = PROBE_PROFILE)
{
    int format = glc_variant % FORMAT_COUNT;
    float color[4] = {1, 0, 1, 1};
    if (glc_variant >= FORMAT_COUNT) {
        color[0] = .125f; color[1] = .25f; color[2] = .75f; color[3] = .625f;
    }
    float effective[4], decoded[4];
    for (int k = 0; k < 4; ++k) {
        int source = channels[format][k];
        effective[k] = source >= 0 ? color[source] : source == -1 ? 0 : 1;
        decoded[k] = effective[k];
#ifndef GLM_BORDER_LEGACY
        if (format >= 5 && k < 3)
            decoded[k] = effective[k] <= .04045f ? effective[k] / 12.92f : powf((effective[k] + .055f) / 1.055f, 2.4f);
#endif
    }
    GLint original_draw, original_read;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &original_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &original_read);
    GLuint inputs[3], outputs[6], fb;
    const unsigned char pixels[] = {32,64,128,192, 224,160,96,48, 80,120,200,240, 180,220,40,160};
    glGenTextures(3, inputs);
    for (int i = 0; i < 3; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, inputs[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, formats[format], 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        float border[4];
        for (int k = 0; k < 4; ++k) border[k] = i == 0 ? 0 : i == 1 ? 1 : color[k];
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
#ifndef GLM_BORDER_LEGACY
        if (format == 4) {
            const GLint swizzle[] = {GL_BLUE, GL_RED, GL_ONE, GL_ZERO};
            glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
        }
#endif
    }
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glGenTextures(6, outputs);
    GLenum attachments[6];
    glActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < 6; ++i) {
        glBindTexture(GL_TEXTURE_2D, outputs[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 32, 4, 0, GL_RGBA, GL_FLOAT, NULL);
        attachments[i] = GL_COLOR_ATTACHMENT0 + i;
        glFramebufferTexture2D(GL_FRAMEBUFFER, attachments[i], GL_TEXTURE_2D, outputs[i], 0);
    }
    glDrawBuffers(6, attachments);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) glc_fail("Border diagnostic framebuffer incomplete");
    const char *body = "uniform sampler2D black_tex,white_tex,color_tex;uniform vec4 coefficient,decoded_coefficient;"
        "void main(){vec2 uv=gl_FragCoord.xy/vec2(32,4)*1.5-.25;vec4 a=texture2D(black_tex,uv);"
        "vec4 w=texture2D(white_tex,uv);vec4 d=w-a;gl_FragData[0]=a;gl_FragData[1]=w;"
        "gl_FragData[2]=texture2D(color_tex,uv);gl_FragData[3]=a+coefficient*max(max(d.x,d.y),max(d.z,d.w));"
        "gl_FragData[4]=mix(a,w,coefficient);gl_FragData[5]=mix(a,w,decoded_coefficient);}";
    char fragment[2048];
#ifdef GLM_BORDER_LEGACY
    snprintf(fragment, sizeof fragment, "#version 120\n%s", body);
    const char *vertex = "#version 120\nvoid main(){gl_Position=gl_Vertex;}";
#else
    char core_body[2048];
    snprintf(core_body, sizeof core_body, "%s", body);
    /* GLSL410 core uses explicit outputs and texture(), with identical arithmetic. */
    char *at;
    while ((at = strstr(core_body, "texture2D"))) { memmove(at + 7, at + 9, strlen(at + 9) + 1); memcpy(at, "texture", 7); }
    for (int i = 0; i < 6; ++i) {
        char find[32], replace[32]; snprintf(find, sizeof find, "gl_FragData[%d]", i); snprintf(replace, sizeof replace, "out%d", i);
        while ((at = strstr(core_body, find))) { size_t length = strlen(replace); memmove(at + length, at + strlen(find), strlen(at + strlen(find)) + 1); memcpy(at, replace, length); }
    }
    snprintf(fragment, sizeof fragment, "#version 410 core\nlayout(location=0)out vec4 out0;layout(location=1)out vec4 out1;"
        "layout(location=2)out vec4 out2;layout(location=3)out vec4 out3;layout(location=4)out vec4 out4;layout(location=5)out vec4 out5;%s", core_body);
    const char *vertex = "#version 410 core\nvoid main(){vec2 p=gl_VertexID==0?vec2(-1,-1):gl_VertexID==1?vec2(3,-1):vec2(-1,3);gl_Position=vec4(p,0,1);}";
#endif
    GLuint program = glc_program(vertex, fragment, NULL);
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "black_tex"), 0);
    glUniform1i(glGetUniformLocation(program, "white_tex"), 1);
    glUniform1i(glGetUniformLocation(program, "color_tex"), 2);
    glUniform4fv(glGetUniformLocation(program, "coefficient"), 1, effective);
    glUniform4fv(glGetUniformLocation(program, "decoded_coefficient"), 1, decoded);
    for (int i = 0; i < 3; ++i) { glActiveTexture(GL_TEXTURE0 + i); glBindTexture(GL_TEXTURE_2D, inputs[i]); }
    glViewport(0, 0, 32, 4);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST);
#ifdef GLM_BORDER_LEGACY
    glBegin(GL_TRIANGLES); glVertex2f(-1,-1); glVertex2f(3,-1); glVertex2f(-1,3); glEnd();
#else
    GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao); glDrawArrays(GL_TRIANGLES, 0, 3);
#endif
    float values[6][32 * 4 * 4];
    for (int i = 0; i < 6; ++i) { glReadBuffer(attachments[i]); glReadPixels(0, 0, 32, 4, GL_RGBA, GL_FLOAT, values[i]); }
    float maxima[3] = {0};
    int first[3] = {-1,-1,-1};
    for (int method = 0; method < 3; ++method)
        for (int i = 0; i < 32 * 4 * 4; ++i) {
            float delta = fabsf(values[method + 3][i] - values[2][i]);
            if (delta > maxima[method]) maxima[method] = delta;
            if (delta > 1e-7f && first[method] < 0) first[method] = i;
        }
    fprintf(stderr, "Border %s max error scalar=%a mix=%a decoded_mix=%a\n", modes[glc_variant], maxima[0], maxima[1], maxima[2]);
    for (int method = 0; method < 3; ++method) if (first[method] >= 0) {
        int i = first[method];
        fprintf(stderr, "  method%d first pixel%d channel%d a=%a w=%a native=%a reconstructed=%a\n", method, i/4, i%4,
                values[0][i], values[1][i], values[2][i], values[method+3][i]);
    }
#ifdef GLM_BORDER_STRICT
    bool endpoints_ok = true;
    for (int i = 0; i < 32 * 4 * 4; ++i) {
        float expected = effective[i % 4] == 0 ? values[0][i] : values[1][i];
        if (!isfinite(values[2][i]) || values[2][i] != expected) endpoints_ok = false;
    }
#endif
    if (glGetError()) glc_fail("Border diagnostic raised an API error");
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)original_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)original_read);
    glDeleteFramebuffers(1, &fb); glDeleteTextures(3, inputs); glDeleteTextures(6, outputs);
    glUseProgram(0); glDeleteProgram(program); glActiveTexture(GL_TEXTURE0);
#ifndef GLM_BORDER_LEGACY
    glDeleteVertexArrays(1, &vao);
#endif
#ifdef GLM_BORDER_STRICT
    if (!endpoints_ok) glc_fail("Border endpoint sample differed from selected black/white sample");
#endif
    glViewport(0, 0, 64, 64); glClearColor(.125f, .25f, .5f, 1); glClear(GL_COLOR_BUFFER_BIT);
}
