#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdio.h>

static const char *const depth_volume_modes[] = {
    "depth_unsized", "depth16", "depth24", "depth32", "depth32f", "depth24_stencil8", "depth32f_stencil8"
};

GLC_CASE_VARIANTS(core_depth_volume_errors, depth_volume_modes, .profile = GLC_CORE)
{
    static const GLenum formats[] = {
        GL_DEPTH_COMPONENT, GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT32,
        GL_DEPTH_COMPONENT32F, GL_DEPTH24_STENCIL8, GL_DEPTH32F_STENCIL8
    };
    GLenum internal = formats[glc_variant];
    bool combined = glc_variant >= 5;
    GLenum format = combined ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT;
    GLenum type = internal == GL_DEPTH32F_STENCIL8 ? GL_FLOAT_32_UNSIGNED_INT_24_8_REV :
                  combined ? GL_UNSIGNED_INT_24_8 : GL_FLOAT;
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_3D, texture);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 2, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    if (glGetError()) glc_fail("Color volume setup failed");
    glTexImage3D(GL_TEXTURE_3D, 0, internal, 2, 2, 2, 0, format, type, NULL);
    GLenum image_error = glGetError();
    GLint width = -1, stored_format = -1;
    glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_INTERNAL_FORMAT, &stored_format);
    glTexImage3D(GL_PROXY_TEXTURE_3D, 0, internal, 2, 2, 2, 0, format, type, NULL);
    GLenum proxy_error = glGetError();
    /* CopyTexImage2D has no volume target, independently of its format. */
    glCopyTexImage2D(GL_TEXTURE_3D, 0, internal, 0, 0, 2, 2, 0);
    GLenum copy_error = glGetError();
    fprintf(stderr, "Depth volume %s: image=%x proxy=%x copy=%x retained_width=%d retained_format=%x\n",
            depth_volume_modes[glc_variant], image_error, proxy_error, copy_error, width, stored_format);
    glDeleteTextures(1, &texture);
    glClearColor(.25f, .5f, .75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (image_error != GL_INVALID_OPERATION || proxy_error != GL_INVALID_OPERATION ||
        copy_error != GL_INVALID_ENUM || width != 2 || stored_format != GL_RGBA8)
        glc_fail("Invalid depth volume call did not reject safely or modified prior image");
}
