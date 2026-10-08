#include "glc_gl_core.h"
#include "glcompare.h"
#include <string.h>

static const char *const setters[] = {"scalar_i", "scalar_f", "vector_i", "vector_f"};

GLC_CASE_VARIANTS(core_swizzle_invalid_preserves_state, setters, .profile = GLC_CORE)
{
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_CUBE_MAP, texture);
    const GLint initial[] = {GL_BLUE, GL_ALPHA, GL_ONE, GL_GREEN};
    const GLenum components[] = {GL_TEXTURE_SWIZZLE_R, GL_TEXTURE_SWIZZLE_G,
        GL_TEXTURE_SWIZZLE_B, GL_TEXTURE_SWIZZLE_A};
    glTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_SWIZZLE_RGBA, initial);
    if (glGetError()) glc_fail("Initial swizzle failed");
    for (int component = 0; component < 4; ++component) {
        GLint integers[] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
        GLfloat floats[] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
        integers[component] = -1;
        floats[component] = -1;
        switch (glc_variant) {
        case 0: glTexParameteri(GL_TEXTURE_CUBE_MAP, components[component], -1); break;
        case 1: glTexParameterf(GL_TEXTURE_CUBE_MAP, components[component], -1); break;
        case 2: glTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_SWIZZLE_RGBA, integers); break;
        case 3: glTexParameterfv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_SWIZZLE_RGBA, floats); break;
        }
        GLenum error = glGetError();
        /* Native Apple uses INVALID_OPERATION here. Exact core error codes
         * remain audited by CTS and the standalone texture_swizzle_api probe. */
        if (error != GL_INVALID_ENUM && error != GL_INVALID_OPERATION)
            glc_fail("Invalid swizzle component %d was accepted, error %x", component, error);
        GLint actual[4];
        glGetTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_SWIZZLE_RGBA, actual);
        if (glGetError() || memcmp(actual, initial, sizeof actual))
            glc_fail("Rejected swizzle changed state at component %d", component);
    }
    glDeleteTextures(1, &texture);
    glClearColor(.25f, .5f, .75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
