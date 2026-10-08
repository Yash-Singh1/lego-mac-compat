#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdio.h>
#include <string.h>

static const char *const rgtc_volume_modes[] = {"red", "signed_red", "rg", "signed_rg"};
GLC_CASE_VARIANTS(core_rgtc_volume_fallback, rgtc_volume_modes, .profile = GLC_CORE)
{
    const GLenum formats[] = {GL_COMPRESSED_RED_RGTC1, GL_COMPRESSED_SIGNED_RED_RGTC1,
                             GL_COMPRESSED_RG_RGTC2, GL_COMPRESSED_SIGNED_RG_RGTC2};
    int channels = glc_variant < 2 ? 1 : 2;
    GLenum external = channels == 1 ? GL_RED : GL_RG;
    GLenum type = glc_variant & 1 ? GL_BYTE : GL_UNSIGNED_BYTE;
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_3D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    unsigned char input[8 * 8 * 2 * 2], output[sizeof input];
    for (unsigned i = 0; i < sizeof input; ++i) input[i] = (unsigned char)(type == GL_BYTE ? (int)(i % 193) - 96 : 32 + i % 193);
    for (int depth = 1; depth <= 2; ++depth) {
        glTexImage3D(GL_TEXTURE_3D, 0, formats[glc_variant], 8, 8, depth, 0, external, type, input);
        GLenum error = glGetError();
        if (error) glc_fail("RGTC volume must accept uncompressed input, error %x", error);
        GLint compressed = -1, internal = -1, actual_depth = -1;
        glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_COMPRESSED, &compressed);
        glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal);
        glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_DEPTH, &actual_depth);
        fprintf(stderr, "RGTC volume %s depth%d: compressed=%d internal=%x depth=%d\n",
                rgtc_volume_modes[glc_variant], depth, compressed, internal, actual_depth);
        if (compressed || actual_depth != depth) glc_fail("RGTC volume did not use uncompressed fallback storage");
        memset(output, 0, sizeof output);
        glGetTexImage(GL_TEXTURE_3D, 0, external, type, output);
        if (glGetError() || memcmp(input, output, (size_t)8 * 8 * depth * channels))
            glc_fail("RGTC volume fallback changed normalized input values");
    }
    glDeleteTextures(1, &texture);
    glClearColor(.25f, .5f, .75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
