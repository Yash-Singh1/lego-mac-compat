#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdio.h>

static bool sample_queries_failed;

static void check_framebuffer_samples(const char *label, GLint expected)
{
    GLint draw = -1, read = -1;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    if (draw && glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        glc_fail("%s framebuffer incomplete", label);
    GLint buffers = -1, samples = -1;
    glGetIntegerv(GL_SAMPLE_BUFFERS, &buffers);
    glGetIntegerv(GL_SAMPLES, &samples);
    fprintf(stderr, "%s: draw=%d read=%d sample_buffers=%d samples=%d expected=%d\n",
            label, draw, read, buffers, samples, expected);
    if (buffers != (expected != 0) || samples != expected) sample_queries_failed = true;
}

GLC_CASE(core_framebuffer_sample_queries, .profile = GLC_CORE)
{
    sample_queries_failed = false;
    GLint original_draw, original_read;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &original_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &original_read);
    GLuint framebuffers[2], textures[2], renderbuffer;
    glGenFramebuffers(2, framebuffers);
    glGenTextures(2, textures);
    glGenRenderbuffers(1, &renderbuffer);
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[0]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[0], 0);
    check_framebuffer_samples("single texture", 0);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, textures[1]);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA8, 64, 64, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[1]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, textures[1], 0);
    check_framebuffer_samples("multisample texture", 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffers[0]);
    check_framebuffer_samples("multisample draw, single read", 4);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers[0]);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffers[1]);
    check_framebuffer_samples("single draw, multisample read", 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    check_framebuffer_samples("default draw, multisample read", 0);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[1]);
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, 64, 64);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
    check_framebuffer_samples("multisample renderbuffer", 4);
    glDeleteRenderbuffers(1, &renderbuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[0], 0);
    check_framebuffer_samples("deleted renderbuffer, single replacement", 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, textures[1], 0);
    glDeleteTextures(1, &textures[1]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[0], 0);
    check_framebuffer_samples("deleted texture, single replacement", 0);
    GLenum error = glGetError();
    if (error) glc_fail("Framebuffer sample query GL error: %x", error);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)original_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)original_read);
    glDeleteFramebuffers(2, framebuffers);
    glDeleteTextures(1, &textures[0]);
    glClearColor(.25f, .5f, .75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (sample_queries_failed) glc_fail("Framebuffer sample queries differed; see logged states");
}
