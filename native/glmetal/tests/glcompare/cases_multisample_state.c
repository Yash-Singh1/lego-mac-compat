#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdio.h>

static const char *const sample_state_modes[] = {
    "one_variable", "one_fixed", "four_variable", "four_fixed"
};
GLC_CASE_VARIANTS(core_multisample_fixed_locations, sample_state_modes, .profile = GLC_CORE)
{
    GLsizei samples = glc_variant < 2 ? 1 : 4;
    GLboolean fixed = (glc_variant & 1) != 0;
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, texture);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_RGBA8, 8, 8, fixed);
    GLint actual_samples = -1, actual_fixed = -1;
    GLfloat float_fixed = -1;
    glGetTexLevelParameteriv(GL_TEXTURE_2D_MULTISAMPLE, 0, GL_TEXTURE_SAMPLES, &actual_samples);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_MULTISAMPLE, 0, GL_TEXTURE_FIXED_SAMPLE_LOCATIONS, &actual_fixed);
    glGetTexLevelParameterfv(GL_TEXTURE_2D_MULTISAMPLE, 0, GL_TEXTURE_FIXED_SAMPLE_LOCATIONS, &float_fixed);
    if (float_fixed != actual_fixed) glc_fail("Fixed-location getters disagree");
    GLenum error = glGetError();
    if (error) glc_fail("Multisample state GL error %x", error);
    fprintf(stderr, "Multisample state requested %d/%d, got %d/%d\n", samples, fixed, actual_samples, actual_fixed);
    glClearColor((float)actual_fixed, (float)actual_samples / 4, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDeleteTextures(1, &texture);
}
