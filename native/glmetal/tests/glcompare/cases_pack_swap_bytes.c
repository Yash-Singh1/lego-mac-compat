#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *const pack_swap_modes[] = {
    "float_get", "float_read", "ushort_get", "ushort_read", "integer_get", "integer_read",
    "rgb9e5_get", "depth32fs8_get", "depth32fs8_read"
};

GLC_CASE_VARIANTS(core_pack_swap_bytes, pack_swap_modes, .profile = GLC_CORE)
{
    int mode = glc_variant;
    bool read_pixels = mode == 1 || mode == 3 || mode == 5 || mode == 8;
    bool depth = mode >= 7;
    GLint original_draw, original_read;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &original_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &original_read);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
    GLenum internal = GL_RGBA32F, format = GL_RGBA, type = GL_FLOAT;
    int bytes = 16, element = 4;
    float floats[16];
    uint16_t shorts[16];
    uint32_t integers[16];
    struct { float depth; uint32_t stencil; } depth_stencil[4];
    for (int i = 0; i < 16; ++i) {
        floats[i] = (float)(i + 1) / 32.0f;
        shorts[i] = (uint16_t)(0x1234 + i * 0x101);
        integers[i] = 0x12345678u + (uint32_t)i * 0x10203u;
    }
    for (int i = 0; i < 4; ++i) {
        depth_stencil[i].depth = (float)(i + 1) / 8.0f;
        depth_stencil[i].stencil = (uint32_t)(0x35 + i * 0x11);
    }
    const void *input = floats;
    if (mode == 2 || mode == 3) {
        internal = GL_RGBA16; type = GL_UNSIGNED_SHORT; bytes = 8; element = 2; input = shorts;
    } else if (mode == 4 || mode == 5) {
        internal = GL_RGBA32UI; format = GL_RGBA_INTEGER; type = GL_UNSIGNED_INT; input = integers;
    } else if (mode == 6) {
        internal = GL_RGB9_E5; format = GL_RGB; type = GL_UNSIGNED_INT_5_9_9_9_REV; bytes = 4;
        input = NULL;
    } else if (depth) {
        internal = GL_DEPTH32F_STENCIL8; format = GL_DEPTH_STENCIL;
        type = GL_FLOAT_32_UNSIGNED_INT_24_8_REV; bytes = 8; input = depth_stencil;
    }
    GLuint texture, framebuffer;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    if (mode == 6) {
        const float rgb[] = {.125f, .25f, .5f, .75f, .5f, .25f,
                             1.0f, 2.0f, 4.0f, .375f, .625f, .875f};
        glTexImage2D(GL_TEXTURE_2D, 0, internal, 2, 2, 0, format, GL_FLOAT, rgb);
    } else glTexImage2D(GL_TEXTURE_2D, 0, internal, 2, 2, 0, format, type, input);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, depth ? GL_DEPTH_STENCIL_ATTACHMENT : GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, texture, 0);
    glDrawBuffer(depth ? GL_NONE : GL_COLOR_ATTACHMENT0);
    glReadBuffer(depth ? GL_NONE : GL_COLOR_ATTACHMENT0);
    if (read_pixels && glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        glc_fail("Pack swap framebuffer incomplete");
    glPixelStorei(GL_PACK_ALIGNMENT, 8);
    glPixelStorei(GL_PACK_ROW_LENGTH, 5);
    glPixelStorei(GL_PACK_SKIP_ROWS, 1);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 1);
    glPixelStorei(GL_PACK_IMAGE_HEIGHT, 0);
    glPixelStorei(GL_PACK_SKIP_IMAGES, 0);
    enum { GUARD = 32, PAYLOAD = 512, TOTAL = GUARD + PAYLOAD + GUARD };
    _Alignas(float) unsigned char native[TOTAL], swapped[TOTAL], expected[TOTAL];
    memset(native, 0xaa, sizeof native);
    memset(swapped, 0xaa, sizeof swapped);
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
    GLboolean native_swap_state = GL_TRUE;
    glGetBooleanv(GL_PACK_SWAP_BYTES, &native_swap_state);
    if (read_pixels) glReadPixels(0, 0, 2, 2, format, type, native + GUARD);
    else glGetTexImage(GL_TEXTURE_2D, 0, format, type, native + GUARD);
    GLenum native_error = glGetError();
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_TRUE);
    GLboolean swapped_swap_state = GL_FALSE;
    glGetBooleanv(GL_PACK_SWAP_BYTES, &swapped_swap_state);
    if (read_pixels) glReadPixels(0, 0, 2, 2, format, type, swapped + GUARD);
    else glGetTexImage(GL_TEXTURE_2D, 0, format, type, swapped + GUARD);
    GLenum swapped_error = glGetError();
    memcpy(expected, native, sizeof expected);
    int row = (5 * bytes + 7) / 8 * 8;
    int start = GUARD + row + bytes;
    bool padding_ok = true, changed = false;
    for (int i = 0; i < TOTAL; ++i) {
        bool pixel_byte = false;
        for (int y = 0; y < 2; ++y)
            pixel_byte |= i >= start + y * row && i < start + y * row + 2 * bytes;
        if (!pixel_byte && (native[i] != 0xaa || swapped[i] != 0xaa)) padding_ok = false;
    }
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            for (int component = 0; component < bytes; component += element)
                for (int b = 0; b < element; ++b) {
                    int offset = start + y * row + x * bytes + component;
                    expected[offset + b] = native[offset + element - 1 - b];
                    changed |= expected[offset + b] != native[offset + b];
                }
    bool values_ok = memcmp(expected, swapped, sizeof expected) == 0;
    fprintf(stderr, "Pack swap %s: errors=%x/%x states=%u/%u row=%d start=%d changed=%d padding=%d matches=%d\n",
            pack_swap_modes[mode], native_error, swapped_error, native_swap_state, swapped_swap_state,
            row, start - GUARD, changed, padding_ok, values_ok);
    fprintf(stderr, "  first pixel native/swapped:");
    for (int i = 0; i < bytes; ++i) fprintf(stderr, " %02x/%02x", native[start + i], swapped[start + i]);
    fprintf(stderr, "\n");
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)original_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)original_read);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    glClearColor(.25f, .5f, .75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (native_error || swapped_error || native_swap_state || !swapped_swap_state ||
        !padding_ok || !changed || !values_ok)
        glc_fail("PACK_SWAP_BYTES did not reverse elements or changed padding");
}
