/* UBYTE narrow upload correctness. Build: make -j2 glcompare.
 * Run core_narrow_upload and legacy_narrow_upload against Apple, GLMetal,
 * and GLMETAL_NO_FAST_UPLOAD=1 separately. No GPU execution during build. */
#include "glc_gl_legacy.h"
#include "glcompare.h"
#include <stdio.h>
#include <string.h>

struct narrow_format { GLenum storage, external; unsigned channels; char expansion; };
static const struct narrow_format core_formats[] = {
    {GL_R8, GL_RED, 1, 'r'}, {GL_RG8, GL_RG, 2, 'g'}
};
static const struct narrow_format legacy_formats[] = {
    {GL_LUMINANCE8, GL_LUMINANCE, 1, 'l'},
    {GL_LUMINANCE8_ALPHA8, GL_LUMINANCE_ALPHA, 2, 'b'},
    {GL_ALPHA8, GL_ALPHA, 1, 'a'},
    {GL_INTENSITY8, GL_RED, 1, 'i'},
    {GL_INTENSITY8, GL_LUMINANCE, 1, 'i'}
};
static const char *const core_names[] = {"red", "rg"};
static const char *const legacy_names[] = {"luminance", "luminance_alpha", "alpha", "intensity_red", "intensity_luminance"};

static void unpack(GLint alignment, GLint row_length, GLint skip_rows, GLint skip_pixels)
{
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pixels);
}

static void expected_texel(const struct narrow_format *format, const unsigned char *input,
                           unsigned char *output, int transfer)
{
    unsigned red = input[0], alpha = format->channels == 2 ? input[1] : input[0];
    /* The transfer parameters use exactly representable .5 scales. */
    if (transfer) {
        red = (unsigned)(red * .5 + .25 * 255 + .5);
        alpha = (unsigned)(alpha * .5 + .125 * 255 + .5);
    }
    output[0] = output[1] = output[2] = 0;
    output[3] = 255;
    switch (format->expansion) {
    case 'r': output[0] = red; break;
    case 'g': output[0] = red; output[1] = input[1]; break;
    case 'l': output[0] = output[1] = output[2] = red; break;
    case 'b': output[0] = output[1] = output[2] = red; output[3] = alpha; break;
    case 'a': output[3] = alpha; break;
    case 'i': output[0] = output[1] = output[2] = output[3] = red; break;
    }
}

static void read_narrow(GLuint texture, GLuint sampling_program, unsigned char *actual)
{
    glBindTexture(GL_TEXTURE_2D, texture);
    if (!sampling_program) {
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, actual);
        return;
    }
    /* Legacy raw readback exposes stored components, while sampling expands
     * luminance/intensity. Verify the channel expansion seen by the movie. */
    glUseProgram(sampling_program);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DITHER);
    glViewport(0, 0, 5, 3);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(-1, -1);
    glTexCoord2f(1, 0); glVertex2f(1, -1);
    glTexCoord2f(1, 1); glVertex2f(1, 1);
    glTexCoord2f(0, 1); glVertex2f(-1, 1);
    glEnd();
    glReadPixels(0, 0, 5, 3, GL_RGBA, GL_UNSIGNED_BYTE, actual);
}

static void check_narrow(GLuint texture, GLuint sampling_program, const unsigned char *expected, const char *stage)
{
    unsigned char actual[5 * 3 * 4];
    read_narrow(texture, sampling_program, actual);
    GLenum error = glGetError();
    if (error) glc_fail("Narrow upload %s error 0x%x", stage, error);
    for (unsigned i = 0; i < sizeof actual; ++i) {
        int difference = (int)actual[i] - expected[i];
        if (difference < -1 || difference > 1)
            glc_fail("Narrow upload %s texel %u channel %u got %u expected %u",
                     stage, i / 4, i % 4, actual[i], expected[i]);
    }
}

static void run_narrow(const struct narrow_format *format, int legacy)
{
    unsigned char source[512], expected[5 * 3 * 4];
    memset(source, 0xcd, sizeof source);
    unsigned stride = (11 * format->channels + 7) & ~7u;
    for (unsigned y = 0; y < 3; ++y) for (unsigned x = 0; x < 5; ++x) {
        unsigned char *pixel = source + (y + 2) * stride + (x + 3) * format->channels;
        pixel[0] = 17 + y * 53 + x * 11;
        if (format->channels == 2) pixel[1] = 231 - y * 37 - x * 13;
        expected_texel(format, pixel, expected + (y * 5 + x) * 4, 0);
    }
    GLuint sampling_program = 0;
    if (legacy) {
        sampling_program = glc_program(
            "#version 120\nvarying vec2 uv;void main(){gl_Position=gl_Vertex;uv=gl_MultiTexCoord0.xy;}",
            "#version 120\nuniform sampler2D image;varying vec2 uv;void main(){gl_FragColor=texture2D(image,uv);}", NULL);
        glUseProgram(sampling_program);
        glUniform1i(glGetUniformLocation(sampling_program, "image"), 0);
    }
    glActiveTexture(GL_TEXTURE0);
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    unpack(8, 11, 2, 3);
    glTexImage2D(GL_TEXTURE_2D, 0, format->storage, 5, 3, 0, format->external, GL_UNSIGNED_BYTE, source);
    check_narrow(texture, sampling_program, expected, "aligned rows and skips");

    memset(source, 0xe7, sizeof source);
    stride = (7 * format->channels + 3) & ~3u;
    for (unsigned y = 0; y < 2; ++y) for (unsigned x = 0; x < 2; ++x) {
        unsigned char *pixel = source + (y + 1) * stride + (x + 2) * format->channels;
        pixel[0] = 193 - y * 29 - x * 41;
        if (format->channels == 2) pixel[1] = 31 + y * 47 + x * 19;
        expected_texel(format, pixel, expected + ((y + 1) * 5 + x + 1) * 4, 0);
    }
    unpack(4, 7, 1, 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 1, 2, 2, format->external, GL_UNSIGNED_BYTE, source);
    check_narrow(texture, sampling_program, expected, "subimage keeps surrounding texels");

    if (legacy) {
        glPixelTransferf(GL_RED_SCALE, .5f);
        glPixelTransferf(GL_RED_BIAS, .25f);
        glPixelTransferf(GL_ALPHA_SCALE, .5f);
        glPixelTransferf(GL_ALPHA_BIAS, .125f);
        unsigned char pixel[] = {96, 160};
        unpack(1, 0, 0, 0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 4, 0, 1, 1, format->external, GL_UNSIGNED_BYTE, pixel);
        expected_texel(format, pixel, expected + 4 * 4, 1);
        /* Readback has its own transfer stage. Reset first to isolate upload. */
        glPixelTransferf(GL_RED_SCALE, 1);
        glPixelTransferf(GL_RED_BIAS, 0);
        glPixelTransferf(GL_ALPHA_SCALE, 1);
        glPixelTransferf(GL_ALPHA_BIAS, 0);
        check_narrow(texture, sampling_program, expected, "nonidentity pixel transfer fallback");
        /* Identity restored must allow the fast path again. */
        pixel[0] = 73; pixel[1] = 149;
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, format->external, GL_UNSIGNED_BYTE, pixel);
        expected_texel(format, pixel, expected, 0);
        check_narrow(texture, sampling_program, expected, "identity restored after fallback");
    }
    unpack(4, 0, 0, 0);
    unsigned char actual[5 * 3 * 4];
    read_narrow(texture, sampling_program, actual);
    /* Display the checked bytes through an RGBA texture. This avoids relying
     * on legacy luminance/alpha framebuffer renderability or shader syntax. */
    GLuint image, framebuffer;
    GLint original;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &original);
    glGenTextures(1, &image);
    glBindTexture(GL_TEXTURE_2D, image);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 5, 3, 0, GL_RGBA, GL_UNSIGNED_BYTE, actual);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image, 0);
    if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        glc_fail("Narrow upload visualization framebuffer incomplete");
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)original);
    glBlitFramebuffer(0, 0, 5, 3, 0, 0, glc_width, glc_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)original);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &image);
    glDeleteTextures(1, &texture);
    if (sampling_program) { glUseProgram(0); glDeleteProgram(sampling_program); }
    GLenum error = glGetError();
    if (error) glc_fail("Narrow upload visualization error 0x%x", error);
}

GLC_CASE_VARIANTS(core_narrow_upload, core_names, .profile = GLC_CORE, .tolerance = 1)
{
    run_narrow(core_formats + glc_variant, 0);
}
GLC_CASE_VARIANTS(legacy_narrow_upload, legacy_names, .profile = GLC_LEGACY, .tolerance = 1)
{
    run_narrow(legacy_formats + glc_variant, 1);
}
