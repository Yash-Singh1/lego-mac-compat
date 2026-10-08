#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdio.h>
#include <unistd.h>

GLC_CASE(driver_texture_swizzle_api, .profile = GLC_CORE)
{
    static const GLenum targets[] = {
        GL_TEXTURE_1D, GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_1D_ARRAY,
        GL_TEXTURE_2D_ARRAY, GL_TEXTURE_RECTANGLE, GL_TEXTURE_CUBE_MAP,
        GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_2D_MULTISAMPLE_ARRAY
    };
    static const GLint values[] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA, GL_ONE, GL_ZERO,
        0x1902, 0x1907, -1, 2};
    static const GLenum states[] = {GL_TEXTURE_SWIZZLE_R, GL_TEXTURE_SWIZZLE_G,
        GL_TEXTURE_SWIZZLE_B, GL_TEXTURE_SWIZZLE_A};
    for (unsigned t = 0; t < sizeof targets / sizeof targets[0]; ++t) {
        GLuint texture;
        glGenTextures(1, &texture);
        glBindTexture(targets[t], texture);
        GLenum bind_error = glGetError();
        GLint initial = -99;
        glGetTexParameterIiv(targets[t], GL_TEXTURE_SWIZZLE_R, &initial);
        GLenum query_error = glGetError();
        fprintf(stderr, "swizzle_initial target=%04x bind_error=%04x query_error=%04x value=%04x\n",
            targets[t], bind_error, query_error, initial);
        glDeleteTextures(1, &texture);
    }
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_CUBE_MAP, texture);
    for (unsigned s = 0; s < sizeof states / sizeof states[0]; ++s) {
        for (unsigned v = 0; v < sizeof values / sizeof values[0]; ++v) {
            glTexParameteri(GL_TEXTURE_CUBE_MAP, states[s], values[v]);
            GLenum error = glGetError();
            fprintf(stderr, "swizzle_scalar pname=%04x value=%d error=%04x expected=%04x\n",
                states[s], values[v], error, v < 6 ? GL_NO_ERROR : GL_INVALID_ENUM);
        }
    }
    for (unsigned component = 0; component < 4; ++component) {
        for (unsigned v = 0; v < sizeof values / sizeof values[0]; ++v) {
            GLint parameters[] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
            parameters[component] = values[v];
            glTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_SWIZZLE_RGBA, parameters);
            GLenum error = glGetError();
            fprintf(stderr, "swizzle_vector component=%u value=%d error=%04x expected=%04x\n",
                component, values[v], error, v < 6 ? GL_NO_ERROR : GL_INVALID_ENUM);
        }
    }
    glDeleteTextures(1, &texture);
    usleep(50000);
    glClearColor(.25f, .5f, .75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
