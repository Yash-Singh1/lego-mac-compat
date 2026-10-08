#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *const pack_image_modes[] = {
#ifdef GLM_PACK_READPIXELS_DIAGNOSTIC
    "read_skip0_height0", "read_skip1_height0", "read_skip0_height3", "read_skip1_height3"
#else
    "get3d_skip0_height0", "get3d_skip1_height0", "get3d_skip0_height3", "get3d_skip1_height3"
#endif
};

#ifdef GLM_PACK_READPIXELS_DIAGNOSTIC
GLC_CASE_VARIANTS(core_pack_readpixels_diagnostic, pack_image_modes, .profile = GLC_CORE)
#else
GLC_CASE_VARIANTS(core_pack_image_state, pack_image_modes, .profile = GLC_CORE)
#endif
{
#ifdef GLM_PACK_READPIXELS_DIAGNOSTIC
    const bool get_texture = false;
#else
    const bool get_texture = true;
#endif
    const GLint skip = glc_variant & 1;
    const GLint height = (glc_variant & 2) ? 3 : 0;
    GLint original_draw, original_read;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &original_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &original_read);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
    glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
    float input[32];
    for (int i = 0; i < 32; ++i) input[i] = (float)(i + 1) / 64.0f;
    GLuint texture, framebuffer;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_3D, texture);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA32F, 2, 2, 2, 0, GL_RGBA, GL_FLOAT, input);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        glc_fail("Pack image probe framebuffer incomplete");
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
    glPixelStorei(GL_PACK_IMAGE_HEIGHT, height);
    glPixelStorei(GL_PACK_SKIP_IMAGES, skip);
    enum { GUARD = 32, PAYLOAD = 512, TOTAL = GUARD + PAYLOAD + GUARD };
    _Alignas(float) unsigned char output[TOTAL], expected[TOTAL];
    memset(output, 0xaa, sizeof output);
    memset(expected, 0xaa, sizeof expected);
    if (get_texture)
        glGetTexImage(GL_TEXTURE_3D, 0, GL_RGBA, GL_FLOAT, output + GUARD);
    else
        glReadPixels(0, 0, 2, 2, GL_RGBA, GL_FLOAT, output + GUARD);
    GLenum error = glGetError();
    const size_t image_stride = 2 * 4 * sizeof(float) * (height ? height : 2);
    const size_t offset = get_texture ? skip * image_stride : 0;
    const int layers = get_texture ? 2 : 1;
    for (int layer = 0; layer < layers; ++layer)
        memcpy(expected + GUARD + offset + layer * image_stride, input + layer * 16, 16 * sizeof(float));
    int first = -1, last = -1;
    for (int i = 0; i < PAYLOAD; ++i) {
        if (output[GUARD + i] == 0xaa) continue;
        if (first < 0) first = i;
        last = i;
    }
    fprintf(stderr, "Pack image %s: error=%x expected_offset=%zu first_changed=%d last_changed=%d\n",
            pack_image_modes[glc_variant], error, offset, first, last);
    const int probes[] = {0, 64, 96, 128, 192};
    for (int i = 0; i < 5; ++i) {
        float value[4];
        uint32_t bits[4];
        memcpy(value, output + GUARD + probes[i], sizeof value);
        memcpy(bits, output + GUARD + probes[i], sizeof bits);
        fprintf(stderr, "  byte%d: %a/%08x %a/%08x %a/%08x %a/%08x\n", probes[i],
                value[0], bits[0], value[1], bits[1], value[2], bits[2], value[3], bits[3]);
    }
    const bool guards_ok = memcmp(output, expected, GUARD) == 0 &&
                           memcmp(output + GUARD + PAYLOAD, expected + GUARD + PAYLOAD, GUARD) == 0;
    const bool pixels_ok = memcmp(output + GUARD, expected + GUARD, PAYLOAD) == 0;
    fprintf(stderr, "  guards=%s expected_layout=%s\n", guards_ok ? "intact" : "modified",
            pixels_ok ? "matches" : "differs");
    glPixelStorei(GL_PACK_SKIP_IMAGES, 0);
    glPixelStorei(GL_PACK_IMAGE_HEIGHT, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)original_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)original_read);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    glClearColor(.25f, .5f, .75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (error || !guards_ok || !pixels_ok)
        glc_fail("Pack image state did not match expected layout; see values and guard log");
}
