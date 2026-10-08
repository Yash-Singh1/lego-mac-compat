#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *const missing_modes[] = {
    "stencil_d24", "stencil_d32f", "depth_d24", "depth_d32f"
};

static int aspect_values_changed(const uint8_t *a, const uint8_t *b, int wide)
{
    if (!wide) return memcmp(a, b, 16) != 0;
    for (int i = 0; i < 4; ++i) {
        uint32_t sa, sb;
        memcpy(&sa, a + i * 8 + 4, 4);
        memcpy(&sb, b + i * 8 + 4, 4);
        if (memcmp(a + i * 8, b + i * 8, 4) || (sa & 0xff) != (sb & 0xff)) return 1;
    }
    return 0;
}

GLC_CASE_VARIANTS(core_copy_missing_aspect, missing_modes, .profile = GLC_CORE)
{
    GLint original_draw, original_read;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &original_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &original_read);
    GLenum format = glc_variant & 1 ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8;
    GLenum type = glc_variant & 1 ? GL_FLOAT_32_UNSIGNED_INT_24_8_REV : GL_UNSIGNED_INT_24_8;
    GLuint texture, source, destination, renderbuffer;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, format, 2, 2, 0, GL_DEPTH_STENCIL, type, NULL);
    glGenFramebuffers(1, &destination);
    glBindFramebuffer(GL_FRAMEBUFFER, destination);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, texture, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        glc_fail("Combined destination is incomplete");
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glStencilMask(0xff);
    glClearDepth(.375);
    glClearStencil(0xab);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    uint8_t before[32] = {0}, after[32] = {0};
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_STENCIL, type, before);

    glGenFramebuffers(1, &source);
    glBindFramebuffer(GL_FRAMEBUFFER, source);
    glGenRenderbuffers(1, &renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, glc_variant < 2 ? GL_DEPTH_COMPONENT24 : GL_STENCIL_INDEX8, 2, 2);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, glc_variant < 2 ? GL_DEPTH_ATTACHMENT : GL_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, renderbuffer);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        glc_fail("Single-aspect source is incomplete");
    GLenum setup_error = glGetError();
    if (setup_error != GL_NO_ERROR) glc_fail("Copy missing-aspect setup failed");
    glCopyTexImage2D(GL_TEXTURE_2D, 0, format, 0, 0, 3, 3, 0);
    GLenum image_error = glGetError();
    GLint retained_width = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &retained_width);
    if (retained_width != 2) glc_fail("Rejected copy changed the reported destination width");
    glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_STENCIL, type, after);
    int image_changed = aspect_values_changed(before, after, glc_variant & 1);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, 2, 2);
    GLenum sub_error = glGetError();
    glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_STENCIL, type, after);
    int sub_changed = aspect_values_changed(before, after, glc_variant & 1);
    fprintf(stderr, "Missing aspect %s: image error=0x%x changed=%d, sub error=0x%x changed=%d\n",
            missing_modes[glc_variant], image_error, image_changed, sub_error, sub_changed);
    if (image_error != GL_INVALID_OPERATION || sub_error != GL_INVALID_OPERATION)
        glc_fail("Copy must reject a missing required framebuffer aspect");
    if (image_changed || sub_changed) glc_fail("Rejected copy changed destination values");
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)original_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)original_read);
    glDeleteRenderbuffers(1, &renderbuffer);
    glDeleteFramebuffers(1, &source);
    glDeleteFramebuffers(1, &destination);
    glDeleteTextures(1, &texture);
    glClearColor(.125f, .25f, .5f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
