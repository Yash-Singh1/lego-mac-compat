#include "glc_gl_core.h"
#include "glcompare.h"

GLC_CASE(core_depth_stencil_read_errors, .profile = GLC_CORE)
{
    const GLenum invalid[] = {GL_BYTE, GL_UNSIGNED_BYTE, GL_SHORT, GL_UNSIGNED_SHORT,
        GL_INT, GL_UNSIGNED_INT, GL_FLOAT, GL_HALF_FLOAT, GL_UNSIGNED_SHORT_5_6_5,
        GL_UNSIGNED_INT_8_8_8_8, GL_UNSIGNED_INT_2_10_10_10_REV};
    GLuint texture, fbo;
    GLint original;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &original);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, 2, 2, 0,
                 GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, texture, 0);
    glReadBuffer(GL_NONE);
    glDrawBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE || glGetError())
        glc_fail("Depth/stencil fixture setup failed");
    unsigned char data[32];
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        glReadPixels(0, 0, 1, 1, GL_DEPTH_STENCIL, invalid[i], data);
        GLenum error = glGetError();
        if (error != GL_INVALID_ENUM) glc_fail("Read type %x error %x, expected INVALID_ENUM", invalid[i], error);
    }
    glReadPixels(0, 0, 1, 1, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, data);
    if (glGetError()) glc_fail("Legal packed type rejected");
    glBindFramebuffer(GL_FRAMEBUFFER, original);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &texture);
    glClearColor(.25, .5, .75, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
