/* Legacy profile: texturing through the fixed-function pipeline. */
#include "glc_gl_legacy.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "glcompare.h"

/* Draws a quad covering most of the target with texture coordinates that
   run from `lo` to `hi` (outside [0,1] exercises wrap modes). */
static void textured_quad(float lo, float hi)
{
    glBegin(GL_QUADS);
    glTexCoord2f(lo, lo); glVertex2f(-0.9f, -0.9f);
    glTexCoord2f(hi, lo); glVertex2f(0.9f, -0.85f);
    glTexCoord2f(hi, hi); glVertex2f(0.85f, 0.9f);
    glTexCoord2f(lo, hi); glVertex2f(-0.85f, 0.85f);
    glEnd();
}

static GLuint pattern_texture(int size, unsigned seed)
{
    unsigned char *pixels = malloc((size_t)size * size * 4);
    glc_pattern_rgba8(pixels, size, size, seed);
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    free(pixels);
    return texture;
}

/* ---- formats ------------------------------------------------------------- */

struct upload_format {
    GLenum internal, format, type;
    int bytes; /* per texel of the source data */
};
static const char *const format_names[] = {
    "rgba8", "rgb8", "bgra_8888_rev", "bgra_byte", "luminance", "luminance_alpha", "alpha", "intensity",
    "rgb_565", "rgba_4444", "rgba_5551", "bgra_1555_rev", "rgba_float", "rgba16", "rgb10_a2", "srgb8_alpha8",
    "alpha4", "luminance12", "luminance4_alpha4", "luminance16_alpha16", "intensity16", "luminance_alpha_float16",
    "intensity_float32", "sluminance8", "sluminance8_alpha8", "compressed_luminance_alpha", "rg16", "rgba_float16",
};
static const struct upload_format formats[] = {
    {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4},
    {GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE, 3},
    {GL_RGBA8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, 4},
    {GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, 4},
    {GL_LUMINANCE8, GL_LUMINANCE, GL_UNSIGNED_BYTE, 1},
    {GL_LUMINANCE8_ALPHA8, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, 2},
    {GL_ALPHA8, GL_ALPHA, GL_UNSIGNED_BYTE, 1},
    {GL_INTENSITY8, GL_LUMINANCE, GL_UNSIGNED_BYTE, 1},
    {GL_RGB5, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2},
    {GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, 2},
    {GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, 2},
    {GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, 2},
    {GL_RGBA32F_ARB, GL_RGBA, GL_FLOAT, 16},
    {GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT, 8},
    {GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_10_10_10_2, 4},
    {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4},
    {GL_ALPHA4, GL_ALPHA, GL_UNSIGNED_BYTE, 1},
    {GL_LUMINANCE12, GL_LUMINANCE, GL_UNSIGNED_SHORT, 2},
    {GL_LUMINANCE4_ALPHA4, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, 2},
    {GL_LUMINANCE16_ALPHA16, GL_LUMINANCE_ALPHA, GL_UNSIGNED_SHORT, 4},
    {GL_INTENSITY16, GL_LUMINANCE, GL_UNSIGNED_SHORT, 2},
    {GL_LUMINANCE_ALPHA16F_ARB, GL_LUMINANCE_ALPHA, GL_FLOAT, 8},
    {GL_INTENSITY32F_ARB, GL_LUMINANCE, GL_FLOAT, 4},
    {GL_SLUMINANCE8, GL_LUMINANCE, GL_UNSIGNED_BYTE, 1},
    {GL_SLUMINANCE8_ALPHA8, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, 2},
    /* (Apple compresses generic RGB/RGBA formats to S3TC: not comparable.) */
    {GL_COMPRESSED_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, 2},
    {GL_RG16, GL_RG, GL_UNSIGNED_SHORT, 4},
    {GL_RGBA16F_ARB, GL_RGBA, GL_FLOAT, 16},
};

GLC_CASE_VARIANTS(texture_format, format_names, .profile = GLC_LEGACY)
{
    const struct upload_format *f = &formats[glc_variant];
    enum { kSize = 16 };
    /* Bytes chosen so every format sees varied data; floats get [0,1]. */
    unsigned char *data = malloc(kSize * kSize * 16);
    if (f->type == GL_FLOAT) {
        float *values = (float *)data;
        for (int i = 0; i < kSize * kSize * 4; ++i) values[i] = (float)((i * 37) % 101) / 100.0f;
    } else {
        for (int i = 0; i < kSize * kSize * f->bytes; ++i) data[i] = (unsigned char)(i * 29 + (i >> 3) * 7);
    }
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)f->internal, kSize, kSize, 0, f->format, f->type, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    free(data);
    glClearColor(0.5f, 0.25f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(-1, -1);
    glTexCoord2f(1, 0); glVertex2f(1, -1);
    glTexCoord2f(1, 1); glVertex2f(1, 1);
    glTexCoord2f(0, 1); glVertex2f(-1, 1);
    glEnd();
}

/* S3TC blocks with fixed endpoints and indices. */
static const char *const dxt_names[] = {"dxt1_rgb", "dxt1_rgba", "dxt3", "dxt5"};
GLC_CASE_VARIANTS(texture_dxt, dxt_names, .profile = GLC_LEGACY)
{
    static const GLenum internals[] = {GL_COMPRESSED_RGB_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
                                       GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT};
    enum { kSize = 16, kBlocks = (kSize / 4) * (kSize / 4) };
    int block_bytes = glc_variant < 2 ? 8 : 16;
    unsigned char data[kBlocks * 16];
    for (int b = 0; b < kBlocks; ++b) {
        unsigned char *block = data + b * block_bytes;
        unsigned char *color = block_bytes == 16 ? block + 8 : block;
        if (block_bytes == 16) {
            if (glc_variant == 2) {
                for (int i = 0; i < 8; ++i) block[i] = (unsigned char)(b * 31 + i * 17);
            } else {
                block[0] = (unsigned char)(40 + b * 13);
                block[1] = (unsigned char)(220 - b * 9);
                for (int i = 2; i < 8; ++i) block[i] = (unsigned char)(b * 53 + i * 11);
            }
        }
        uint16_t c0 = (uint16_t)(((b * 3) & 31) << 11 | ((b * 7) & 63) << 5 | (31 - b));
        uint16_t c1 = (uint16_t)(((31 - b) & 31) << 11 | ((b * 5 + 9) & 63) << 5 | ((b * 2) & 31));
        /* Alternate 4-colour and 3-colour+transparent DXT1 modes. */
        if (glc_variant < 2 && (b & 1)) { uint16_t t = c0; c0 = c1 < c0 ? c1 : c0; c1 = t > c1 ? t : c1; }
        color[0] = (unsigned char)c0; color[1] = (unsigned char)(c0 >> 8);
        color[2] = (unsigned char)c1; color[3] = (unsigned char)(c1 >> 8);
        for (int i = 4; i < 8; ++i) color[i] = (unsigned char)(b * 97 + i * 45);
    }
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, internals[glc_variant], kSize, kSize, 0, kBlocks * block_bytes, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glClearColor(0.2f, 0.8f, 0.4f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    textured_quad(0, 1);
}

/* ---- sampling ------------------------------------------------------------ */

static const char *const filter_names[] = {
    "nearest", "linear", "nearest_mipmap_nearest", "linear_mipmap_nearest",
    "nearest_mipmap_linear", "linear_mipmap_linear",
};
GLC_CASE_VARIANTS(texture_min_filter, filter_names, .profile = GLC_LEGACY, .tolerance = 2)
{
    static const GLenum filters[] = {GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST,
                                     GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR};
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    /* Every mip level a distinct pattern so level selection is visible. */
    for (int level = 0, size = 64; size >= 1; ++level, size /= 2) {
        unsigned char *pixels = malloc((size_t)size * size * 4);
        glc_pattern_rgba8(pixels, size, size, (unsigned)level + 1);
        for (int i = 0; i < size * size; ++i) pixels[i * 4 + 2] = (unsigned char)(level * 36);
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        free(pixels);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)filters[glc_variant]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    /* A receding plane covers several levels of detail. */
    glMatrixMode(GL_PROJECTION);
    glFrustum(-0.5, 0.5, -0.5, 0.5, 1, 20);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex3f(-1.5f, -1, -1.2f);
    glTexCoord2f(4, 0); glVertex3f(1.5f, -1, -1.2f);
    glTexCoord2f(4, 16); glVertex3f(1.5f, -1, -18);
    glTexCoord2f(0, 16); glVertex3f(-1.5f, -1, -18);
    glEnd();
}

GLC_CASE(texture_generate_mipmap, .profile = GLC_LEGACY, .tolerance = 2)
{
    GLuint texture = pattern_texture(64, 9);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    unsigned char *pixels = malloc(64 * 64 * 4);
    glc_pattern_rgba8(pixels, 64, 64, 9);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    free(pixels);
    (void)texture;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    /* Quads of decreasing size pick levels 0.. */
    for (int i = 0; i < 5; ++i) {
        float s = 1.0f / (float)(1 << i), x = -1 + (1 - s) * 0.9f * (float)i / 4 * 2;
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(x, -1);
        glTexCoord2f(1, 0); glVertex2f(x + s, -1);
        glTexCoord2f(1, 1); glVertex2f(x + s, -1 + s);
        glTexCoord2f(0, 1); glVertex2f(x, -1 + s);
        glEnd();
    }
}

static const char *const wrap_names[] = {"repeat", "clamp_to_edge", "mirrored_repeat", "clamp", "clamp_to_border"};
GLC_CASE_VARIANTS(texture_wrap, wrap_names, .profile = GLC_LEGACY, .tolerance = 1)
{
    static const GLenum wraps[] = {GL_REPEAT, GL_CLAMP_TO_EDGE, GL_MIRRORED_REPEAT, GL_CLAMP, GL_CLAMP_TO_BORDER};
    pattern_texture(8, 3);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)wraps[glc_variant]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)wraps[glc_variant]);
    const float border[4] = {1, 0, 1, 1};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, glc_variant == 3 ? GL_LINEAR : GL_NEAREST);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    textured_quad(-1.25f, 2.25f);
}

GLC_CASE(texture_lod_bias_and_levels, .profile = GLC_LEGACY, .tolerance = 2)
{
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    for (int level = 0, size = 32; size >= 1; ++level, size /= 2) {
        unsigned char *pixels = malloc((size_t)size * size * 4);
        for (int i = 0; i < size * size; ++i) {
            pixels[i * 4] = (unsigned char)(level * 50);
            pixels[i * 4 + 1] = (unsigned char)(255 - level * 40);
            pixels[i * 4 + 2] = (unsigned char)((i & 1) * 200);
            pixels[i * 4 + 3] = 255;
        }
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        free(pixels);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 4);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    textured_quad(0, 1);
}

/* ---- texture environment ------------------------------------------------- */

static const char *const env_modes[] = {"modulate", "replace", "decal", "blend", "add"};
GLC_CASE_VARIANTS(texture_env, env_modes, .profile = GLC_LEGACY)
{
    static const GLenum modes[] = {GL_MODULATE, GL_REPLACE, GL_DECAL, GL_BLEND, GL_ADD};
    pattern_texture(16, 5);
    const float env_color[4] = {0.9f, 0.2f, 0.4f, 0.6f};
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, env_color);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, (GLint)modes[glc_variant]);
    glClearColor(0.1f, 0.2f, 0.3f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glColor4f(1, 0.5f, 0.25f, 0.75f); glTexCoord2f(0, 0); glVertex2f(-0.9f, -0.9f);
    glColor4f(0.25f, 1, 0.5f, 0.5f);  glTexCoord2f(1, 0); glVertex2f(0.9f, -0.9f);
    glColor4f(0.5f, 0.25f, 1, 1);     glTexCoord2f(1, 1); glVertex2f(0.9f, 0.9f);
    glColor4f(1, 1, 1, 0.25f);        glTexCoord2f(0, 1); glVertex2f(-0.9f, 0.9f);
    glEnd();
}

static const char *const combine_names[] = {"replace", "modulate", "add", "add_signed", "interpolate", "subtract", "dot3_rgb"};
GLC_CASE_VARIANTS(texture_combine, combine_names, .profile = GLC_LEGACY, .tolerance = 1)
{
    static const GLenum functions[] = {GL_REPLACE, GL_MODULATE, GL_ADD, GL_ADD_SIGNED, GL_INTERPOLATE, GL_SUBTRACT, GL_DOT3_RGB};
    pattern_texture(16, 7);
    const float env_color[4] = {0.3f, 0.6f, 0.9f, 0.4f};
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, env_color);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, (GLint)functions[glc_variant]);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB, GL_CONSTANT);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_ALPHA);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_PRIMARY_COLOR);
    glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, glc_variant == 1 ? 2.0f : 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glColor4f(1, 0.5f, 0.25f, 0.75f); glTexCoord2f(0, 0); glVertex2f(-0.9f, -0.9f);
    glColor4f(0.25f, 1, 0.5f, 0.5f);  glTexCoord2f(1, 0); glVertex2f(0.9f, -0.9f);
    glColor4f(0.5f, 0.25f, 1, 1);     glTexCoord2f(1, 1); glVertex2f(0.9f, 0.9f);
    glColor4f(1, 1, 1, 0.25f);        glTexCoord2f(0, 1); glVertex2f(-0.9f, 0.9f);
    glEnd();
}

GLC_CASE(multitexture_modulate_add, .profile = GLC_LEGACY)
{
    glActiveTexture(GL_TEXTURE0);
    pattern_texture(16, 11);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glActiveTexture(GL_TEXTURE1);
    pattern_texture(4, 12);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glColor3f(0.7f, 0.7f, 0.7f);
    glMultiTexCoord2f(GL_TEXTURE0, 0, 0); glMultiTexCoord2f(GL_TEXTURE1, 0.5f, 0.5f); glVertex2f(-0.9f, -0.9f);
    glMultiTexCoord2f(GL_TEXTURE0, 2, 0); glMultiTexCoord2f(GL_TEXTURE1, 1, 0.5f); glVertex2f(0.9f, -0.9f);
    glMultiTexCoord2f(GL_TEXTURE0, 2, 2); glMultiTexCoord2f(GL_TEXTURE1, 1, 1); glVertex2f(0.9f, 0.9f);
    glMultiTexCoord2f(GL_TEXTURE0, 0, 2); glMultiTexCoord2f(GL_TEXTURE1, 0.5f, 1); glVertex2f(-0.9f, 0.9f);
    glEnd();
}

GLC_CASE(texture_matrix_and_texgen, .profile = GLC_LEGACY)
{
    pattern_texture(8, 13);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glMatrixMode(GL_TEXTURE);
    glRotatef(30, 0, 0, 1);
    glScalef(2, 1.5f, 1);
    glMatrixMode(GL_MODELVIEW);
    const float plane_s[4] = {0.5f, 0.1f, 0, 0.5f}, plane_t[4] = {-0.1f, 0.5f, 0, 0.5f};
    glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
    glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
    glTexGenfv(GL_S, GL_OBJECT_PLANE, plane_s);
    glTexGenfv(GL_T, GL_OBJECT_PLANE, plane_t);
    glEnable(GL_TEXTURE_GEN_S);
    glEnable(GL_TEXTURE_GEN_T);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glRectf(-0.9f, -0.9f, 0.9f, 0.9f);
}

GLC_CASE(texture_rectangle, .profile = GLC_LEGACY)
{
    enum { kW = 24, kH = 10 };
    unsigned char pixels[kW * kH * 4];
    glc_pattern_rgba8(pixels, kW, kH, 17);
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, texture);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA8, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_RECTANGLE_ARB);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(-0.9f, -0.9f);
    glTexCoord2f(kW, 0); glVertex2f(0.9f, -0.9f);
    glTexCoord2f(kW, kH); glVertex2f(0.9f, 0.9f);
    glTexCoord2f(0, kH); glVertex2f(-0.9f, 0.9f);
    glEnd();
}

GLC_CASE(texture_cube_map_reflection, .profile = GLC_LEGACY, .tolerance = 2, .outlier_fraction = 0.002) /* face-edge texel ties */
{
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_CUBE_MAP, texture);
    for (int face = 0; face < 6; ++face) {
        unsigned char pixels[8 * 8 * 4];
        for (int i = 0; i < 64; ++i) {
            pixels[i * 4] = (unsigned char)(face * 40 + (i & 7) * 8);
            pixels[i * 4 + 1] = (unsigned char)(255 - face * 40);
            pixels[i * 4 + 2] = (unsigned char)((i >> 3) * 30);
            pixels[i * 4 + 3] = 255;
        }
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)face, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
    glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
    glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
    glEnable(GL_TEXTURE_GEN_S);
    glEnable(GL_TEXTURE_GEN_T);
    glEnable(GL_TEXTURE_GEN_R);
    glEnable(GL_TEXTURE_CUBE_MAP);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glClear(GL_COLOR_BUFFER_BIT);
    /* Away from the eye: a vertex at the eye point has no view direction
       (Apple's implementation then samples NaN everywhere in the fan). */
    glBegin(GL_TRIANGLE_FAN);
    glNormal3f(0, 0, 1); glVertex3f(0, 0, -0.5f);
    for (int i = 0; i <= 24; ++i) {
        float a = (float)i / 24 * 6.2831853f;
        glNormal3f(0.9f * __builtin_cosf(a), 0.9f * __builtin_sinf(a), 0.3f);
        glVertex3f(0.9f * __builtin_cosf(a), 0.9f * __builtin_sinf(a), -0.5f);
    }
    glEnd();
}

GLC_CASE(texture_3d, .profile = GLC_LEGACY, .tolerance = 1)
{
    enum { kSize = 8 };
    unsigned char pixels[kSize * kSize * kSize * 4];
    for (int i = 0; i < kSize * kSize * kSize; ++i) {
        pixels[i * 4] = (unsigned char)((i % kSize) * 32);
        pixels[i * 4 + 1] = (unsigned char)(((i / kSize) % kSize) * 32);
        pixels[i * 4 + 2] = (unsigned char)((i / (kSize * kSize)) * 32);
        pixels[i * 4 + 3] = 255;
    }
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_3D, texture);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, kSize, kSize, kSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glEnable(GL_TEXTURE_3D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glTexCoord3f(0, 0, 0.1f); glVertex2f(-0.9f, -0.9f);
    glTexCoord3f(1, 0, 0.4f); glVertex2f(0.9f, -0.9f);
    glTexCoord3f(1, 1, 0.9f); glVertex2f(0.9f, 0.9f);
    glTexCoord3f(0, 1, 0.6f); glVertex2f(-0.9f, 0.9f);
    glEnd();
}

GLC_CASE(texture_sub_image_and_unpack, .profile = GLC_LEGACY)
{
    GLuint texture = pattern_texture(32, 19);
    (void)texture;
    unsigned char source[40 * 20 * 4];
    glc_pattern_rgba8(source, 40, 20, 23);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 40);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 5);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 3);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 7, 9, 20, 12, GL_RGBA, GL_UNSIGNED_BYTE, source);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(-1, -1);
    glTexCoord2f(1, 0); glVertex2f(1, -1);
    glTexCoord2f(1, 1); glVertex2f(1, 1);
    glTexCoord2f(0, 1); glVertex2f(-1, 1);
    glEnd();
}

/* APPLE_row_bytes: a byte row stride overriding ROW_LENGTH/ALIGNMENT, on
   upload and on readback (read back into a second texture). */
GLC_CASE(texture_apple_row_bytes, .profile = GLC_LEGACY)
{
    GLuint texture = pattern_texture(32, 19);
    unsigned char source[40 * 20 * 4 + 64];
    glc_pattern_rgba8(source, 40, 20, 23);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 7);  /* ignored while ROW_BYTES is set */
    glPixelStorei(GL_UNPACK_ROW_BYTES_APPLE, 40 * 4 + 12);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 4, 6, 20, 12, GL_RGBA, GL_UNSIGNED_BYTE, source);
    glPixelStorei(GL_UNPACK_ROW_BYTES_APPLE, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    unsigned char readback[32 * 36 * 4];
    memset(readback, 0x80, sizeof readback);
    glPixelStorei(GL_PACK_ROW_BYTES_APPLE, 36 * 4);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, readback);
    glPixelStorei(GL_PACK_ROW_BYTES_APPLE, 0);
    GLuint copy;
    glGenTextures(1, &copy);
    glBindTexture(GL_TEXTURE_2D, copy);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 36, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, readback);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    (void)texture;
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(-1, -1);
    glTexCoord2f(1, 0); glVertex2f(1, -1);
    glTexCoord2f(1, 1); glVertex2f(1, 1);
    glTexCoord2f(0, 1); glVertex2f(-1, 1);
    glEnd();
}

GLC_CASE(copy_tex_sub_image, .profile = GLC_LEGACY)
{
    glClearColor(0, 0, 0.3f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0); glVertex2f(-1, -1);
    glColor3f(0, 1, 0); glVertex2f(1, -1);
    glColor3f(0, 0, 1); glVertex2f(0, 1);
    glEnd();
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 4, 2, 16, 16, 24, 28);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glClearColor(0.5f, 0.5f, 0.5f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    textured_quad(0, 1);
}

/* ---- lighting and fog ---------------------------------------------------- */

static void sphere(int slices, int stacks)
{
    for (int i = 0; i < stacks; ++i) {
        float a0 = 3.1415927f * ((float)i / stacks - 0.5f), a1 = 3.1415927f * ((float)(i + 1) / stacks - 0.5f);
        glBegin(GL_QUAD_STRIP);
        for (int j = 0; j <= slices; ++j) {
            float b = 6.2831853f * (float)j / slices;
            float x0 = __builtin_cosf(a0) * __builtin_cosf(b), y0 = __builtin_sinf(a0), z0 = __builtin_cosf(a0) * __builtin_sinf(b);
            float x1 = __builtin_cosf(a1) * __builtin_cosf(b), y1 = __builtin_sinf(a1), z1 = __builtin_cosf(a1) * __builtin_sinf(b);
            glNormal3f(x0, y0, z0); glVertex3f(x0 * 0.8f, y0 * 0.8f, z0 * 0.8f);
            glNormal3f(x1, y1, z1); glVertex3f(x1 * 0.8f, y1 * 0.8f, z1 * 0.8f);
        }
        glEnd();
    }
}

static const char *const light_names[] = {"directional", "point_attenuated", "spot", "two_lights_specular", "color_material",
                                          "two_side", "color_material_ambient", "color_material_specular",
                                          "color_material_emission", "color_material_both_strong_ambient"};
GLC_CASE_VARIANTS(lighting, light_names, .profile = GLC_LEGACY, .tolerance = 2)
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    const float diffuse[4] = {0.9f, 0.7f, 0.4f, 1}, ambient[4] = {0.1f, 0.1f, 0.2f, 1};
    glLightfv(GL_LIGHT0, GL_DIFFUSE, diffuse);
    glLightfv(GL_LIGHT0, GL_AMBIENT, ambient);
    const float spec[4] = {1, 1, 1, 1};
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spec);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 24);
    switch (glc_variant) {
    case 0: { const float p[4] = {-0.5f, 0.7f, 1, 0}; glLightfv(GL_LIGHT0, GL_POSITION, p); break; }
    case 1: {
        const float p[4] = {0.6f, 0.4f, 1.2f, 1};
        glLightfv(GL_LIGHT0, GL_POSITION, p);
        glLightf(GL_LIGHT0, GL_CONSTANT_ATTENUATION, 0.5f);
        glLightf(GL_LIGHT0, GL_LINEAR_ATTENUATION, 0.4f);
        glLightf(GL_LIGHT0, GL_QUADRATIC_ATTENUATION, 0.2f);
        break;
    }
    case 2: {
        const float p[4] = {0, 0, 2, 1}, d[3] = {0.2f, 0.1f, -1};
        glLightfv(GL_LIGHT0, GL_POSITION, p);
        glLightfv(GL_LIGHT0, GL_SPOT_DIRECTION, d);
        glLightf(GL_LIGHT0, GL_SPOT_CUTOFF, 25);
        glLightf(GL_LIGHT0, GL_SPOT_EXPONENT, 8);
        break;
    }
    case 3: {
        const float p0[4] = {-1, 1, 1, 0}, p1[4] = {1, -0.5f, 1, 0}, c1[4] = {0.2f, 0.5f, 1, 1};
        glLightfv(GL_LIGHT0, GL_POSITION, p0);
        glEnable(GL_LIGHT1);
        glLightfv(GL_LIGHT1, GL_POSITION, p1);
        glLightfv(GL_LIGHT1, GL_DIFFUSE, c1);
        glLightfv(GL_LIGHT1, GL_SPECULAR, c1);
        glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, GL_TRUE);
        break;
    }
    case 4: {
        const float p[4] = {0.3f, 0.5f, 1, 0};
        glLightfv(GL_LIGHT0, GL_POSITION, p);
        glEnable(GL_COLOR_MATERIAL);
        glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
        glColor3f(0.2f, 0.9f, 0.3f);
        break;
    }
    case 5: {
        const float p[4] = {0, 0, 1, 0};
        glLightfv(GL_LIGHT0, GL_POSITION, p);
        glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
        const float back[4] = {0.1f, 0.2f, 0.9f, 1};
        glMaterialfv(GL_BACK, GL_DIFFUSE, back);
        glEnable(GL_CLIP_PLANE0);
        const double plane[4] = {0, 0, -1, 0.3};
        glClipPlane(GL_CLIP_PLANE0, plane);
        break;
    }
    case 6: case 7: case 8: case 9: {
        /* Each colour material mode with a strong light term for it. */
        static const GLenum modes[] = {GL_AMBIENT, GL_SPECULAR, GL_EMISSION, GL_AMBIENT_AND_DIFFUSE};
        const float p[4] = {0.3f, 0.5f, 1, 0}, strong[4] = {0.8f, 0.8f, 0.8f, 1};
        glLightfv(GL_LIGHT0, GL_POSITION, p);
        glLightfv(GL_LIGHT0, GL_AMBIENT, strong);
        glEnable(GL_COLOR_MATERIAL);
        glColorMaterial(GL_FRONT_AND_BACK, modes[glc_variant - 6]);
        glColor3f(0.2f, 0.9f, 0.3f);
        break;
    }
    }
    sphere(16, 12);
}

static const char *const fog_names[] = {"linear", "exp", "exp2", "coord"};
GLC_CASE_VARIANTS(fog, fog_names, .profile = GLC_LEGACY, .tolerance = 2)
{
    static const GLenum modes[] = {GL_LINEAR, GL_EXP, GL_EXP2, GL_LINEAR};
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_FOG);
    const float color[4] = {0.6f, 0.6f, 0.7f, 1};
    glFogfv(GL_FOG_COLOR, color);
    glFogi(GL_FOG_MODE, (GLint)modes[glc_variant]);
    glFogf(GL_FOG_START, 1.5f);
    glFogf(GL_FOG_END, 12);
    glFogf(GL_FOG_DENSITY, 0.25f);
    if (glc_variant == 3) glFogi(GL_FOG_COORDINATE_SOURCE, GL_FOG_COORDINATE);
    glMatrixMode(GL_PROJECTION);
    glFrustum(-0.5, 0.5, -0.5, 0.5, 1, 20);
    glBegin(GL_QUADS);
    glColor3f(0.9f, 0.3f, 0.1f);
    glFogCoordf(1); glVertex3f(-1.5f, -1, -1.2f);
    glFogCoordf(1); glVertex3f(1.5f, -1, -1.2f);
    glColor3f(0.1f, 0.8f, 0.2f);
    glFogCoordf(12); glVertex3f(1.5f, 1, -15);
    glFogCoordf(12); glVertex3f(-1.5f, 1, -15);
    glEnd();
}
