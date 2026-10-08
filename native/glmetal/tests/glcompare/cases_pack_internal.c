#include "glc_gl_legacy.h"
#include "glcompare.h"
#include <math.h>
#include <stdio.h>

static const char *const internal_pack_modes[] = {"copy_color", "copy_depth", "copy_stencil", "compressed_mipmap"};

GLC_CASE_VARIANTS(legacy_internal_pack_swap, internal_pack_modes, .profile = GLC_LEGACY)
{
    int mode = glc_variant;
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDepthMask(GL_TRUE);
    glStencilMask(0xff);
    bool values_ok = true;
    if (mode == 3) {
        GLuint textures[2];
        float input[8 * 8 * 4], output[2][4 * 4 * 4];
        for (int i = 0; i < 8 * 8; ++i) {
            input[i * 4] = .125f; input[i * 4 + 1] = .25f;
            input[i * 4 + 2] = .5f; input[i * 4 + 3] = 1;
        }
        glGenTextures(2, textures);
        for (int i = 0; i < 2; ++i) {
            glBindTexture(GL_TEXTURE_2D, textures[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA, 8, 8, 0, GL_RGBA, GL_FLOAT, input);
            glPixelStorei(GL_PACK_SWAP_BYTES, i == 0 ? GL_FALSE : GL_TRUE);
            glGenerateMipmap(GL_TEXTURE_2D);
        }
        GLboolean state = GL_FALSE;
        glGetBooleanv(GL_PACK_SWAP_BYTES, &state);
        values_ok &= state == GL_TRUE;
        glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
        for (int i = 0; i < 2; ++i) {
            glBindTexture(GL_TEXTURE_2D, textures[i]);
            glGetTexImage(GL_TEXTURE_2D, 1, GL_RGBA, GL_FLOAT, output[i]);
        }
        float largest = 0;
        for (int i = 0; i < 4 * 4 * 4; ++i) {
            float difference = fabsf(output[0][i] - output[1][i]);
            if (!isfinite(output[1][i]) || difference > .000001f) values_ok = false;
            if (difference > largest) largest = difference;
        }
        fprintf(stderr, "Internal compressed mipmap: retained_swap=%d maximum_difference=%a\n", state, largest);
        glDeleteTextures(2, textures);
    } else {
        glClearColor(.125f, .25f, .5f, 1);
        glClearDepth(.375);
        glClearStencil(0x53);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        glScissor(16, 0, 8, 8);
        glClearColor(0, 0, 0, 1);
        glClearDepth(.875);
        glClearStencil(0x31);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        if (mode == 1) { glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS); }
        else glDisable(GL_DEPTH_TEST);
        glWindowPos2i(16, 0);
        glPixelStorei(GL_PACK_SWAP_BYTES, GL_TRUE);
        glCopyPixels(0, 0, 8, 8, mode == 0 ? GL_COLOR : mode == 1 ? GL_DEPTH : GL_STENCIL);
        GLboolean state = GL_FALSE;
        glGetBooleanv(GL_PACK_SWAP_BYTES, &state);
        values_ok &= state == GL_TRUE;
        glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
        if (mode == 0) {
            unsigned char value[4] = {0};
            glReadPixels(20, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, value);
            fprintf(stderr, "Internal copy color: %u/%u/%u/%u retained_swap=%d\n",
                    value[0], value[1], value[2], value[3], state);
            values_ok &= value[0] == 32 && value[1] == 64 && value[2] == 128 && value[3] == 255;
        } else if (mode == 1) {
            float value = 0;
            glReadPixels(20, 4, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &value);
            fprintf(stderr, "Internal copy depth: %a retained_swap=%d\n", value, state);
            values_ok &= isfinite(value) && fabsf(value - .375f) < .0000002f;
        } else {
            unsigned char value = 0;
            glReadPixels(20, 4, 1, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, &value);
            fprintf(stderr, "Internal copy stencil: %02x retained_swap=%d\n", value, state);
            values_ok &= value == 0x53;
        }
    }
    GLenum error = glGetError();
    glDisable(GL_DEPTH_TEST);
    glClearColor(.25f, .5f, .75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (error || !values_ok) glc_fail("Internal pixel readback used PACK_SWAP_BYTES or lost caller state: %x", error);
}
