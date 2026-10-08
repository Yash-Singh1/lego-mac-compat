#include "glc_gl_core.h"
#include "glcompare.h"
#include <math.h>
#include <stdio.h>

static const char *const transfer_modes[] = {
    "resolve_d24", "resolve_d32f", "replicate_d24", "replicate_d32f",
    "flip_ms_d24", "flip_ms_d32f", "scissor_ms_d24", "scissor_ms_d32f",
    "depth_only_d24", "depth_only_d32f", "stencil_only_d24", "stencil_only_d32f",
    "stencil_flip_d24", "stencil_flip_d32f", "copy_image_d24", "copy_image_d32f",
    "copy_sub_image_d24", "copy_sub_image_d32f"
};

static GLuint transfer_target(GLenum format, int samples, GLuint *storage)
{
    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    if (samples) {
        glGenRenderbuffers(1, storage);
        glBindRenderbuffer(GL_RENDERBUFFER, *storage);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, format, 8, 8);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, *storage);
    } else {
        glGenTextures(1, storage);
        glBindTexture(GL_TEXTURE_2D, *storage);
        glTexImage2D(GL_TEXTURE_2D, 0, format, 8, 8, 0, GL_DEPTH_STENCIL,
                     format == GL_DEPTH24_STENCIL8 ? GL_UNSIGNED_INT_24_8 : GL_FLOAT_32_UNSIGNED_INT_24_8_REV, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, *storage, 0);
    }
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        glc_fail("Depth/stencil transfer framebuffer incomplete");
    return fb;
}

GLC_CASE_VARIANTS(core_depth_stencil_transfer, transfer_modes, .profile = GLC_CORE)
{
    GLint original_draw, original_read;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &original_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &original_read);
    int mode = glc_variant / 2;
    GLenum format = glc_variant & 1 ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8;
    int source_samples = mode == 0 || mode == 2 || mode == 3 ? 4 : 0;
    int destination_samples = mode >= 1 && mode <= 3 ? 4 : 0;
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glStencilMask(0xff);
    glEnable(GL_MULTISAMPLE);
    GLuint source_storage, destination_storage, resolved_storage = 0;
    GLuint source = transfer_target(format, source_samples, &source_storage);
    glClearDepth(.25);
    glClearStencil(0x12);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(4, 0, 4, 8);
    glClearDepth(.5);
    glClearStencil(0x34);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    GLuint destination = transfer_target(format, destination_samples, &destination_storage);
    glClearDepth(.875);
    glClearStencil(0xab);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, destination);
    if (mode == 3) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(2, 2, 4, 4);
    }
    if (mode >= 7) {
        /* Texture copies ignore the framebuffer scissor. */
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 0, 0);
        glBindTexture(GL_TEXTURE_2D, destination_storage);
        if (mode == 7) glCopyTexImage2D(GL_TEXTURE_2D, 0, format, 0, 0, 8, 8, 0);
        else glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, 8, 8);
    } else {
        GLbitfield mask = mode == 4 ? GL_DEPTH_BUFFER_BIT : mode >= 5 ? GL_STENCIL_BUFFER_BIT :
                          GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
        int flipped = mode == 2 || mode == 6;
        glBlitFramebuffer(0, 0, 8, 8, flipped ? 8 : 0, 0, flipped ? 0 : 8, 8, mask, GL_NEAREST);
    }
    glDisable(GL_SCISSOR_TEST);
    GLuint resolved = 0;
    if (destination_samples) {
        resolved = transfer_target(format, 0, &resolved_storage);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, destination);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolved);
        glBlitFramebuffer(0, 0, 8, 8, 0, 0, 8, 8, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, resolved ? resolved : destination);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    float depth[64] = {0};
    unsigned char stencil[64] = {0};
    glReadPixels(0, 0, 8, 8, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
    glReadPixels(0, 0, 8, 8, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, stencil);
    GLenum error = glGetError();
    if (error) glc_fail("Depth/stencil transfer %s GL error %x", transfer_modes[glc_variant], error);
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
        int sx = mode == 2 || mode == 6 ? 7 - x : x;
        int untouched = mode == 3 && (x < 2 || x >= 6 || y < 2 || y >= 6);
        float expected_depth = untouched || mode == 5 || mode == 6 ? .875f : sx < 4 ? .25f : .5f;
        unsigned expected_stencil = untouched || mode == 4 ? 0xab : sx < 4 ? 0x12 : 0x34;
        if (fabsf(depth[y * 8 + x] - expected_depth) > 0.0000002f || stencil[y * 8 + x] != expected_stencil)
            glc_fail("Depth/stencil transfer %s (%d,%d): depth %.9g expected %.9g, stencil %02x expected %02x",
                     transfer_modes[glc_variant], x, y, depth[y * 8 + x], expected_depth,
                     stencil[y * 8 + x], expected_stencil);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)original_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)original_read);
    glDeleteFramebuffers(1, &source);
    glDeleteFramebuffers(1, &destination);
    glDeleteFramebuffers(1, &resolved);
    if (source_samples) glDeleteRenderbuffers(1, &source_storage);
    else glDeleteTextures(1, &source_storage);
    if (destination_samples) glDeleteRenderbuffers(1, &destination_storage);
    else glDeleteTextures(1, &destination_storage);
    glDeleteTextures(1, &resolved_storage);
    glClearColor(.25, .5, .75, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

static const char *const stencil_transfer_modes[] = {"single", "flip", "resolve", "replicate", "multisample_flip"};
static GLuint stencil_target(int samples, GLuint *storage)
{
    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glGenRenderbuffers(1, storage);
    glBindRenderbuffer(GL_RENDERBUFFER, *storage);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_STENCIL_INDEX8, 8, 8);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, *storage);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        glc_fail("Standalone stencil framebuffer incomplete");
    return fb;
}

GLC_CASE_VARIANTS(core_stencil_transfer, stencil_transfer_modes, .profile = GLC_CORE)
{
    GLint original_draw, original_read;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &original_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &original_read);
    int mode = glc_variant;
    glDisable(GL_SCISSOR_TEST);
    glStencilMask(0xff);
    GLuint storage[3], fb[3];
    fb[0] = stencil_target(mode == 2 || mode == 4 ? 4 : 0, &storage[0]);
    glClearStencil(0x35);
    glClear(GL_STENCIL_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(4, 0, 4, 8);
    glClearStencil(0xc7);
    glClear(GL_STENCIL_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    fb[1] = stencil_target(mode >= 3 ? 4 : 0, &storage[1]);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[0]);
    int flipped = mode == 1 || mode == 4;
    glBlitFramebuffer(0, 0, 8, 8, flipped ? 8 : 0, 0, flipped ? 0 : 8, 8, GL_STENCIL_BUFFER_BIT, GL_NEAREST);
    fb[2] = stencil_target(0, &storage[2]);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[1]);
    glBlitFramebuffer(0, 0, 8, 8, 0, 0, 8, 8, GL_STENCIL_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[2]);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    unsigned char pixels[64] = {0};
    glReadPixels(0, 0, 8, 8, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, pixels);
    GLenum error = glGetError();
    if (error) glc_fail("Standalone stencil transfer %s GL error %x", stencil_transfer_modes[mode], error);
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
        unsigned expected = (flipped ? 7 - x : x) < 4 ? 0x35 : 0xc7;
        if (pixels[y * 8 + x] != expected)
            glc_fail("Standalone stencil transfer %s (%d,%d): %02x expected %02x",
                     stencil_transfer_modes[mode], x, y, pixels[y * 8 + x], expected);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)original_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)original_read);
    glDeleteFramebuffers(3, fb);
    glDeleteRenderbuffers(3, storage);
    glClearColor(.25, .5, .75, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

static const char *const depth_copy_layouts[] = {"offset_d24", "offset_d32f", "mip_d24", "mip_d32f", "array_d24", "array_d32f"};
GLC_CASE_VARIANTS(core_depth_stencil_copy_layout, depth_copy_layouts, .profile = GLC_CORE)
{
    GLint original_draw, original_read;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &original_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &original_read);
    int mode = glc_variant / 2, level = mode == 1 ? 1 : 0, side = mode == 1 ? 4 : 8;
    GLenum format = glc_variant & 1 ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8;
    GLenum type = glc_variant & 1 ? GL_FLOAT_32_UNSIGNED_INT_24_8_REV : GL_UNSIGNED_INT_24_8;
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glStencilMask(0xff);
    GLuint source_storage, destination_storage;
    GLuint source = transfer_target(format, 0, &source_storage);
    glClearDepth(.25);
    glClearStencil(0x12);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(4, 0, 4, 8);
    glClearDepth(.5);
    glClearStencil(0x34);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    GLuint destination = transfer_target(format, 0, &destination_storage);
    if (mode == 1) {
        glTexImage2D(GL_TEXTURE_2D, 1, format, 4, 4, 0, GL_DEPTH_STENCIL, type, NULL);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, destination_storage, 1);
    } else if (mode == 2) {
        glDeleteTextures(1, &destination_storage);
        glGenTextures(1, &destination_storage);
        glBindTexture(GL_TEXTURE_2D_ARRAY, destination_storage);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, format, 8, 8, 2, 0, GL_DEPTH_STENCIL, type, NULL);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, destination_storage, 0, 1);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        glc_fail("Copy layout framebuffer incomplete");
    glClearDepth(.875);
    glClearStencil(0xab);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
    if (mode == 2) glCopyTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 2, 2, 1, 2, 2, 4, 4);
    else glCopyTexSubImage2D(GL_TEXTURE_2D, level, mode ? 0 : 2, mode ? 0 : 2, 2, 2, 4, 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, destination);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    float depth[64] = {0};
    unsigned char stencil[64] = {0};
    glReadPixels(0, 0, side, side, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
    glReadPixels(0, 0, side, side, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, stencil);
    GLenum error = glGetError();
    if (error) glc_fail("Copy layout %s GL error %x", depth_copy_layouts[glc_variant], error);
    for (int y = 0; y < side; ++y) for (int x = 0; x < side; ++x) {
        int touched = mode == 1 || (x >= 2 && x < 6 && y >= 2 && y < 6);
        int left = mode == 1 ? x < 2 : x < 4;
        float expected_depth = !touched ? .875f : left ? .25f : .5f;
        unsigned expected_stencil = !touched ? 0xab : left ? 0x12 : 0x34;
        if (fabsf(depth[y * side + x] - expected_depth) > .0000002f || stencil[y * side + x] != expected_stencil)
            glc_fail("Copy layout %s (%d,%d): depth %.9g expected %.9g, stencil %02x expected %02x",
                     depth_copy_layouts[glc_variant], x, y, depth[y * side + x], expected_depth,
                     stencil[y * side + x], expected_stencil);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)original_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)original_read);
    glDeleteFramebuffers(1, &source);
    glDeleteFramebuffers(1, &destination);
    glDeleteTextures(1, &source_storage);
    glDeleteTextures(1, &destination_storage);
    glClearColor(.25, .5, .75, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
