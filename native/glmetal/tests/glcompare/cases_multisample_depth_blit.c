#include "glc_gl_core.h"
#include "glcompare.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *const depth_blit_variants[] = {
    "resolve_d24", "resolve_d32f", "equal_flip_d24", "equal_flip_d32f",
    "equal_scissor_d24", "equal_scissor_d32f", "depth_only_preserves_stencil"
};

static GLuint depth_target(GLenum format, int samples, GLuint *texture)
{
    GLenum target = samples ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
    glGenTextures(1, texture);
    glBindTexture(target, *texture);
    if (samples)
        glTexImage2DMultisample(target, samples, format, 64, 64, GL_TRUE);
    else {
        GLenum external = format == GL_DEPTH32F_STENCIL8 ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT;
        GLenum type = format == GL_DEPTH32F_STENCIL8 ? GL_FLOAT_32_UNSIGNED_INT_24_8_REV : GL_FLOAT;
        glTexImage2D(target, 0, format, 64, 64, 0, external, type, NULL);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    GLuint framebuffer;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    GLenum attachment = format == GL_DEPTH32F_STENCIL8 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
    glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, target, *texture, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) glc_fail("Depth blit framebuffer incomplete: %x", status);
    return framebuffer;
}

static uint32_t depth_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

GLC_CASE_VARIANTS(core_multisample_depth_blit, depth_blit_variants, .profile = GLC_CORE)
{
    int mode = glc_variant;
    bool equal_samples = mode >= 2 && mode <= 5;
    bool flipped = mode == 2 || mode == 3;
    bool scissored = mode == 4 || mode == 5;
    bool stencil = mode == 6;
    GLenum format = stencil ? GL_DEPTH32F_STENCIL8 : mode % 2 ? GL_DEPTH_COMPONENT32F : GL_DEPTH_COMPONENT24;
    GLint original_draw, original_read;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &original_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &original_read);
    const char *vertex =
        "#version 410 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
        "gl_Position=vec4(p*2.0-1.0,0,1);}";
    GLuint fill = glc_program(vertex,
        "#version 410 core\nuniform int sample_index;void main(){"
        "gl_FragDepth=.125+.25*float(sample_index)+(gl_FragCoord.x>=32.0?.03125:0.0);"
        "gl_SampleMask[0]=1<<sample_index;}", NULL);
    GLuint display = glc_program(vertex, equal_samples ?
        "#version 410 core\nuniform sampler2DMS depth_source;uniform int sample_index,show_samples;"
        "out vec4 frag;void main(){int s=show_samples!=0?int(gl_FragCoord.x)/16:sample_index;"
        "float d=texelFetch(depth_source,ivec2(gl_FragCoord.xy),s).r;frag=vec4(d,d,d,1);}" :
        "#version 410 core\nuniform sampler2D depth_source;out vec4 frag;void main(){"
        "float d=texelFetch(depth_source,ivec2(gl_FragCoord.xy),0).r;frag=vec4(d,d,d,1);}", NULL);
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glViewport(0, 0, 64, 64);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    glStencilMask(0xff);
    GLuint source_texture, destination_texture;
    GLuint source = depth_target(format, 4, &source_texture);
    glClearDepth(1);
    glClearStencil(0x12);
    glClear(GL_DEPTH_BUFFER_BIT | (stencil ? GL_STENCIL_BUFFER_BIT : 0));
    glUseProgram(fill);
    for (int sample = 0; sample < 4; ++sample) {
        glUniform1i(glGetUniformLocation(fill, "sample_index"), sample);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    GLuint destination = depth_target(format, equal_samples ? 4 : 0, &destination_texture);
    glClearDepth(.9375);
    glClearStencil(0xab);
    glClear(GL_DEPTH_BUFFER_BIT | (stencil ? GL_STENCIL_BUFFER_BIT : 0));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, destination);
    if (scissored) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(16, 16, 32, 32);
    }
    glBlitFramebuffer(0, 0, 64, 64, flipped ? 64 : 0, 0, flipped ? 0 : 64, 64,
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, destination);
    GLenum error = glGetError();
    if (error) glc_fail("Depth blit %s GL error before readback: %x", depth_blit_variants[mode], error);
    const int probes[] = {8, 24, 40, 56};
    if (!equal_samples) {
        float measured[4] = {0};
        for (int i = 0; i < 4; ++i)
            glReadPixels(probes[i], 32, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, measured + i);
        fprintf(stderr, "Depth blit %s resolve x8/24/40/56:", depth_blit_variants[mode]);
        for (int i = 0; i < 4; ++i) fprintf(stderr, " %a/%08x", measured[i], depth_bits(measured[i]));
        fprintf(stderr, "\n");
        /* Depth resolve is implementation dependent within the sample range.
           Log the exact value without requiring a particular sample choice. */
        for (int i = 0; i < 4; ++i) {
            float minimum = .125f + (probes[i] >= 32 ? .03125f : 0);
            float maximum = minimum + .75f;
            if (!isfinite(measured[i]) || measured[i] < minimum - .0000002f ||
                measured[i] > maximum + .0000002f)
                glc_fail("Depth resolve outside original sample range: %.9g", measured[i]);
        }
        if (stencil) {
            GLubyte measured_stencil[4] = {0};
            for (int i = 0; i < 4; ++i)
                glReadPixels(probes[i], 32, 1, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, measured_stencil + i);
            fprintf(stderr, "Depth-only destination stencil x8/24/40/56: %02x %02x %02x %02x\n",
                    measured_stencil[0], measured_stencil[1], measured_stencil[2], measured_stencil[3]);
            for (int i = 0; i < 4; ++i)
                if (measured_stencil[i] != 0xab) glc_fail("Depth-only blit modified destination stencil");
        }
    }
    /* Read each multisample depth through a float colour target, avoiding
       the forbidden direct ReadPixels from a multisample framebuffer. */
    GLuint verification_texture = 0, verification_framebuffer = 0;
    glDisable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(display);
    glUniform1i(glGetUniformLocation(display, "depth_source"), 0);
    if (equal_samples) {
        glGenTextures(1, &verification_texture);
        glBindTexture(GL_TEXTURE_2D, verification_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 64, 64, 0, GL_RGBA, GL_FLOAT, NULL);
        glGenFramebuffers(1, &verification_framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, verification_framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, verification_texture, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            glc_fail("Depth sample verification framebuffer incomplete");
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, destination_texture);
        for (int sample = 0; sample < 4; ++sample) {
            glUniform1i(glGetUniformLocation(display, "sample_index"), sample);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            float measured[4] = {0};
            for (int i = 0; i < 4; ++i) glReadPixels(probes[i], 32, 1, 1, GL_RED, GL_FLOAT, measured + i);
            fprintf(stderr, "Depth blit %s sample%d x8/24/40/56:", depth_blit_variants[mode], sample);
            for (int i = 0; i < 4; ++i) fprintf(stderr, " %a/%08x", measured[i], depth_bits(measured[i]));
            fprintf(stderr, "\n");
            for (int i = 0; i < 4; ++i) {
                bool untouched = scissored && (probes[i] < 16 || probes[i] >= 48);
                int source_x = flipped ? 63 - probes[i] : probes[i];
                float expected = untouched ? .9375f : .125f + .25f * sample + (source_x >= 32 ? .03125f : 0);
                if (fabsf(measured[i] - expected) > 0.0000002f)
                    glc_fail("Depth sample correspondence changed: sample%d x%d %.9g != %.9g",
                             sample, probes[i], measured[i], expected);
            }
        }
        glUniform1i(glGetUniformLocation(display, "show_samples"), 1);
    } else glBindTexture(GL_TEXTURE_2D, destination_texture);
    error = glGetError();
    if (error) glc_fail("Depth blit %s verification GL error: %x", depth_blit_variants[mode], error);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)original_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)original_read);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDeleteFramebuffers(1, &verification_framebuffer);
    glDeleteTextures(1, &verification_texture);
    glDeleteFramebuffers(1, &source);
    glDeleteFramebuffers(1, &destination);
    glDeleteTextures(1, &source_texture);
    glDeleteTextures(1, &destination_texture);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(fill);
    glDeleteProgram(display);
}
