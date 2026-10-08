/* GL internal formats to Metal pixel formats, and client pixel conversion. */
#include "glm_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* MTLPixelFormat values (Metal/MTLPixelFormat.h). */
enum {
    MTL_R8Unorm = 10, MTL_R16Unorm = 20, MTL_R16Float = 25, MTL_RG8Unorm = 30, MTL_B5G6R5Unorm = 40,
    MTL_A1BGR5Unorm = 41, MTL_ABGR4Unorm = 42, MTL_BGR5A1Unorm = 43, MTL_R32Float = 55, MTL_RG16Unorm = 60,
    MTL_RG16Float = 65, MTL_RGBA8Unorm = 70, MTL_RGBA8Unorm_sRGB = 71, MTL_RGBA8Uint = 73, MTL_RGBA8Sint = 74,
    MTL_BGRA8Unorm = 80, MTL_RGB10A2Unorm = 90, MTL_RG11B10Float = 92, MTL_RGB9E5Float = 93, MTL_RG32Float = 105,
    MTL_RGBA16Unorm = 110, MTL_RGBA16Uint = 113, MTL_RGBA16Sint = 114, MTL_RGBA16Float = 115, MTL_RGBA32Uint = 123,
    MTL_RGBA32Sint = 124, MTL_RGBA32Float = 125, MTL_R8Uint = 13, MTL_R8Sint = 14, MTL_R16Uint = 23, MTL_R16Sint = 24,
    MTL_R32Uint = 53, MTL_R32Sint = 54, MTL_RG8Uint = 33, MTL_RG8Sint = 34, MTL_RG16Uint = 63, MTL_RG16Sint = 64,
    MTL_RG32Uint = 103, MTL_RG32Sint = 104, MTL_R8Snorm = 12, MTL_RG8Snorm = 32, MTL_RGBA8Snorm = 72,
    MTL_BC1_RGBA = 130, MTL_BC1_RGBA_sRGB = 131, MTL_BC2_RGBA = 132, MTL_BC2_RGBA_sRGB = 133, MTL_BC3_RGBA = 134,
    MTL_BC3_RGBA_sRGB = 135, MTL_BC4_RUnorm = 140, MTL_BC5_RGUnorm = 142,
    MTL_Depth16Unorm = 250, MTL_Depth32Float = 252, MTL_Stencil8 = 253, MTL_Depth24Unorm_Stencil8 = 255,
    MTL_Depth32Float_Stencil8 = 260, MTL_R8Unorm_sRGB = 11, MTL_RG8Unorm_sRGB = 31, MTL_R16Snorm = 22,
    MTL_RG16Snorm = 62, MTL_RGBA16Snorm = 112, MTL_RGB10A2Uint = 91, MTL_BC4_RSnorm = 141, MTL_BC5_RGSnorm = 143,
};

/* How converted upload data is laid out (what glm_convert_upload writes). */
enum glm_upload {
    UP_RGBA8, UP_R8, UP_RG8, UP_RGBA16, UP_R16, UP_RGBA16F, UP_RGBA32F, UP_R32F, UP_RG32F, UP_RG16F, UP_R16F,
    UP_RGB10A2, UP_DEPTH32F, UP_DEPTH24S8, UP_DEPTH16, UP_RAW, UP_RGBA8I, UP_RGBA16I, UP_RGBA32I,
    UP_R8I, UP_R16I, UP_R32I, UP_RG8I, UP_RG16I, UP_RG32I, UP_565, UP_4444, UP_5551, UP_STENCIL8,
};

#define SW(r, g, b, a) {r, g, b, a}
#define RGBA SW(0, 1, 2, 3)

struct entry {
    GLenum internal;
    uint32_t metal, upload;
    int bytes;
    bool compressed, depth, stencil, integer, srgb;
    uint8_t swizzle[4];
};

static const struct entry table[] = {
    /* unsized and 8-bit colour */
    {GL_RGBA, MTL_RGBA8Unorm, UP_RGBA8, 4, 0, 0, 0, 0, 0, RGBA},
    {4, MTL_RGBA8Unorm, UP_RGBA8, 4, 0, 0, 0, 0, 0, RGBA},
    {GL_RGBA8, MTL_RGBA8Unorm, UP_RGBA8, 4, 0, 0, 0, 0, 0, RGBA},
    /* Apple keeps RGBA2 and RGB4 at 4 bits a channel. */
    {GL_RGBA2, MTL_ABGR4Unorm, UP_4444, 2, 0, 0, 0, 0, 0, RGBA},
    {GL_RGB, MTL_RGBA8Unorm, UP_RGBA8, 4, 0, 0, 0, 0, 0, SW(0, 1, 2, 5)},
    {3, MTL_RGBA8Unorm, UP_RGBA8, 4, 0, 0, 0, 0, 0, SW(0, 1, 2, 5)},
    {GL_RGB8, MTL_RGBA8Unorm, UP_RGBA8, 4, 0, 0, 0, 0, 0, SW(0, 1, 2, 5)},
    /* GL_RGB_RAW_422_APPLE: Apple stores (and reports) it as RGB8. */
    {0x8A51, MTL_RGBA8Unorm, UP_RGBA8, 4, 0, 0, 0, 0, 0, SW(0, 1, 2, 5)},
    {GL_RGB4, MTL_ABGR4Unorm, UP_4444, 2, 0, 0, 0, 0, 0, SW(0, 1, 2, 5)},
    {GL_RGB5, MTL_B5G6R5Unorm, UP_565, 2, 0, 0, 0, 0, 0, SW(0, 1, 2, 5)},
    /* At 3-3-2 precision (textures.m quantizes uploads), though Apple
       reports 5-6-5 sizes. */
    {GL_R3_G3_B2, MTL_RGBA8Unorm, UP_RGBA8, 4, 0, 0, 0, 0, 0, SW(0, 1, 2, 5)},
    {GL_RGBA4, MTL_ABGR4Unorm, UP_4444, 2, 0, 0, 0, 0, 0, RGBA},
    {GL_RGB5_A1, MTL_A1BGR5Unorm, UP_5551, 2, 0, 0, 0, 0, 0, RGBA},
    {GL_RGB10_A2, MTL_RGB10A2Unorm, UP_RGB10A2, 4, 0, 0, 0, 0, 0, RGBA},
    {GL_RGB10, MTL_RGB10A2Unorm, UP_RGB10A2, 4, 0, 0, 0, 0, 0, SW(0, 1, 2, 5)},
    {GL_RGBA12, MTL_RGBA16Unorm, UP_RGBA16, 8, 0, 0, 0, 0, 0, RGBA},
    {GL_RGBA16, MTL_RGBA16Unorm, UP_RGBA16, 8, 0, 0, 0, 0, 0, RGBA},
    {GL_RGB12, MTL_RGBA16Unorm, UP_RGBA16, 8, 0, 0, 0, 0, 0, SW(0, 1, 2, 5)},
    {GL_RGB16, MTL_RGBA16Unorm, UP_RGBA16, 8, 0, 0, 0, 0, 0, SW(0, 1, 2, 5)},
    {GL_SRGB8_ALPHA8, MTL_RGBA8Unorm_sRGB, UP_RGBA8, 4, 0, 0, 0, 0, 1, RGBA},
    {GL_SRGB_ALPHA, MTL_RGBA8Unorm_sRGB, UP_RGBA8, 4, 0, 0, 0, 0, 1, RGBA},
    {GL_SRGB8, MTL_RGBA8Unorm_sRGB, UP_RGBA8, 4, 0, 0, 0, 0, 1, SW(0, 1, 2, 5)},
    {GL_SRGB, MTL_RGBA8Unorm_sRGB, UP_RGBA8, 4, 0, 0, 0, 0, 1, SW(0, 1, 2, 5)},
    /* luminance / alpha / intensity (legacy): R8 or RG8 plus a swizzle */
    {GL_LUMINANCE, MTL_R8Unorm, UP_R8, 1, 0, 0, 0, 0, 0, SW(0, 0, 0, 5)},
    {1, MTL_R8Unorm, UP_R8, 1, 0, 0, 0, 0, 0, SW(0, 0, 0, 5)},
    {GL_LUMINANCE8, MTL_R8Unorm, UP_R8, 1, 0, 0, 0, 0, 0, SW(0, 0, 0, 5)},
    {GL_LUMINANCE16, MTL_R16Unorm, UP_R16, 2, 0, 0, 0, 0, 0, SW(0, 0, 0, 5)},
    {GL_LUMINANCE_ALPHA, MTL_RG8Unorm, UP_RG8, 2, 0, 0, 0, 0, 0, SW(0, 0, 0, 1)},
    {2, MTL_RG8Unorm, UP_RG8, 2, 0, 0, 0, 0, 0, SW(0, 0, 0, 1)},
    {GL_LUMINANCE8_ALPHA8, MTL_RG8Unorm, UP_RG8, 2, 0, 0, 0, 0, 0, SW(0, 0, 0, 1)},
    {GL_ALPHA, MTL_R8Unorm, UP_R8, 1, 0, 0, 0, 0, 0, SW(4, 4, 4, 0)},
    {GL_ALPHA8, MTL_R8Unorm, UP_R8, 1, 0, 0, 0, 0, 0, SW(4, 4, 4, 0)},
    {GL_INTENSITY, MTL_R8Unorm, UP_R8, 1, 0, 0, 0, 0, 0, SW(0, 0, 0, 0)},
    {GL_INTENSITY8, MTL_R8Unorm, UP_R8, 1, 0, 0, 0, 0, 0, SW(0, 0, 0, 0)},
    {GL_LUMINANCE32F_ARB, MTL_R32Float, UP_R32F, 4, 0, 0, 0, 0, 0, SW(0, 0, 0, 5)},
    {GL_LUMINANCE16F_ARB, MTL_R16Float, UP_R16F, 2, 0, 0, 0, 0, 0, SW(0, 0, 0, 5)},
    {GL_ALPHA32F_ARB, MTL_R32Float, UP_R32F, 4, 0, 0, 0, 0, 0, SW(4, 4, 4, 0)},
    /* R/RG */
    {GL_RED, MTL_R8Unorm, UP_R8, 1, 0, 0, 0, 0, 0, SW(0, 4, 4, 5)},
    {GL_R8, MTL_R8Unorm, UP_R8, 1, 0, 0, 0, 0, 0, SW(0, 4, 4, 5)},
    {GL_R16, MTL_R16Unorm, UP_R16, 2, 0, 0, 0, 0, 0, SW(0, 4, 4, 5)},
    {GL_RG, MTL_RG8Unorm, UP_RG8, 2, 0, 0, 0, 0, 0, SW(0, 1, 4, 5)},
    {GL_RG8, MTL_RG8Unorm, UP_RG8, 2, 0, 0, 0, 0, 0, SW(0, 1, 4, 5)},
    {GL_R16F, MTL_R16Float, UP_R16F, 2, 0, 0, 0, 0, 0, SW(0, 4, 4, 5)},
    {GL_RG16F, MTL_RG16Float, UP_RG16F, 4, 0, 0, 0, 0, 0, SW(0, 1, 4, 5)},
    {GL_R32F, MTL_R32Float, UP_R32F, 4, 0, 0, 0, 0, 0, SW(0, 4, 4, 5)},
    {GL_RG32F, MTL_RG32Float, UP_RG32F, 8, 0, 0, 0, 0, 0, SW(0, 1, 4, 5)},
    /* float */
    {GL_RGBA16F, MTL_RGBA16Float, UP_RGBA16F, 8, 0, 0, 0, 0, 0, RGBA},
    {GL_RGB16F, MTL_RGBA16Float, UP_RGBA16F, 8, 0, 0, 0, 0, 0, SW(0, 1, 2, 5)},
    {GL_RGBA32F, MTL_RGBA32Float, UP_RGBA32F, 16, 0, 0, 0, 0, 0, RGBA},
    {GL_RGB32F, MTL_RGBA32Float, UP_RGBA32F, 16, 0, 0, 0, 0, 0, SW(0, 1, 2, 5)},
    {GL_R11F_G11F_B10F, MTL_RG11B10Float, UP_RAW, 4, 0, 0, 0, 0, 0, SW(0, 1, 2, 5)},
    {GL_RGB9_E5, MTL_RGB9E5Float, UP_RAW, 4, 0, 0, 0, 0, 0, SW(0, 1, 2, 5)},
    /* integer */
    {GL_RGBA8UI, MTL_RGBA8Uint, UP_RGBA8I, 4, 0, 0, 0, 1, 0, RGBA},
    {GL_RGBA8I, MTL_RGBA8Sint, UP_RGBA8I, 4, 0, 0, 0, 1, 0, RGBA},
    {GL_RGBA16UI, MTL_RGBA16Uint, UP_RGBA16I, 8, 0, 0, 0, 1, 0, RGBA},
    {GL_RGBA16I, MTL_RGBA16Sint, UP_RGBA16I, 8, 0, 0, 0, 1, 0, RGBA},
    {GL_RGBA32UI, MTL_RGBA32Uint, UP_RGBA32I, 16, 0, 0, 0, 1, 0, RGBA},
    {GL_RGBA32I, MTL_RGBA32Sint, UP_RGBA32I, 16, 0, 0, 0, 1, 0, RGBA},
    {GL_R8UI, MTL_R8Uint, UP_R8I, 1, 0, 0, 0, 1, 0, SW(0, 4, 4, 5)},
    {GL_R8I, MTL_R8Sint, UP_R8I, 1, 0, 0, 0, 1, 0, SW(0, 4, 4, 5)},
    {GL_R16UI, MTL_R16Uint, UP_R16I, 2, 0, 0, 0, 1, 0, SW(0, 4, 4, 5)},
    {GL_R16I, MTL_R16Sint, UP_R16I, 2, 0, 0, 0, 1, 0, SW(0, 4, 4, 5)},
    {GL_R32UI, MTL_R32Uint, UP_R32I, 4, 0, 0, 0, 1, 0, SW(0, 4, 4, 5)},
    {GL_R32I, MTL_R32Sint, UP_R32I, 4, 0, 0, 0, 1, 0, SW(0, 4, 4, 5)},
    {GL_RG8UI, MTL_RG8Uint, UP_RG8I, 2, 0, 0, 0, 1, 0, SW(0, 1, 4, 5)},
    {GL_RG16UI, MTL_RG16Uint, UP_RG16I, 4, 0, 0, 0, 1, 0, SW(0, 1, 4, 5)},
    {GL_RG32UI, MTL_RG32Uint, UP_RG32I, 8, 0, 0, 0, 1, 0, SW(0, 1, 4, 5)},
    /* compressed */
    {GL_COMPRESSED_RGB_S3TC_DXT1_EXT, MTL_BC1_RGBA, UP_RAW, 8, 1, 0, 0, 0, 0, SW(0, 1, 2, 5)},
    {GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, MTL_BC1_RGBA, UP_RAW, 8, 1, 0, 0, 0, 0, RGBA},
    {GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, MTL_BC2_RGBA, UP_RAW, 16, 1, 0, 0, 0, 0, RGBA},
    {GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, MTL_BC3_RGBA, UP_RAW, 16, 1, 0, 0, 0, 0, RGBA},
    {GL_COMPRESSED_SRGB_S3TC_DXT1_EXT, MTL_BC1_RGBA_sRGB, UP_RAW, 8, 1, 0, 0, 0, 1, SW(0, 1, 2, 5)},
    {GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT, MTL_BC1_RGBA_sRGB, UP_RAW, 8, 1, 0, 0, 0, 1, RGBA},
    {GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT, MTL_BC2_RGBA_sRGB, UP_RAW, 16, 1, 0, 0, 0, 1, RGBA},
    {GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT, MTL_BC3_RGBA_sRGB, UP_RAW, 16, 1, 0, 0, 0, 1, RGBA},
    {GL_COMPRESSED_RED_RGTC1, MTL_BC4_RUnorm, UP_RAW, 8, 1, 0, 0, 0, 0, SW(0, 4, 4, 5)},
    {GL_COMPRESSED_RG_RGTC2, MTL_BC5_RGUnorm, UP_RAW, 16, 1, 0, 0, 0, 0, SW(0, 1, 4, 5)},
    /* sized legacy luminance / alpha / intensity (GL 2.1 table 3.16); sRGB
       luminance alpha is RGBA storage: Metal's RG8 sRGB would decode alpha */
    {GL_ALPHA4, MTL_R8Unorm, UP_R8, 1, 0, 0, 0, 0, 0, SW(4, 4, 4, 0)},
    {GL_ALPHA12, MTL_R16Unorm, UP_R16, 2, 0, 0, 0, 0, 0, SW(4, 4, 4, 0)},
    {GL_ALPHA16, MTL_R16Unorm, UP_R16, 2, 0, 0, 0, 0, 0, SW(4, 4, 4, 0)},
    {GL_LUMINANCE4, MTL_R8Unorm, UP_R8, 1, 0, 0, 0, 0, 0, SW(0, 0, 0, 5)},
    {GL_LUMINANCE12, MTL_R16Unorm, UP_R16, 2, 0, 0, 0, 0, 0, SW(0, 0, 0, 5)},
    {GL_LUMINANCE4_ALPHA4, MTL_RG8Unorm, UP_RG8, 2, 0, 0, 0, 0, 0, SW(0, 0, 0, 1)},
    {GL_LUMINANCE6_ALPHA2, MTL_RG8Unorm, UP_RG8, 2, 0, 0, 0, 0, 0, SW(0, 0, 0, 1)},
    {GL_LUMINANCE12_ALPHA4, MTL_RG16Unorm, UP_RAW, 4, 0, 0, 0, 0, 0, SW(0, 0, 0, 1)},
    {GL_LUMINANCE12_ALPHA12, MTL_RG16Unorm, UP_RAW, 4, 0, 0, 0, 0, 0, SW(0, 0, 0, 1)},
    {GL_LUMINANCE16_ALPHA16, MTL_RG16Unorm, UP_RAW, 4, 0, 0, 0, 0, 0, SW(0, 0, 0, 1)},
    {GL_INTENSITY4, MTL_R8Unorm, UP_R8, 1, 0, 0, 0, 0, 0, SW(0, 0, 0, 0)},
    {GL_INTENSITY12, MTL_R16Unorm, UP_R16, 2, 0, 0, 0, 0, 0, SW(0, 0, 0, 0)},
    {GL_INTENSITY16, MTL_R16Unorm, UP_R16, 2, 0, 0, 0, 0, 0, SW(0, 0, 0, 0)},
    {GL_ALPHA16F_ARB, MTL_R16Float, UP_R16F, 2, 0, 0, 0, 0, 0, SW(4, 4, 4, 0)},
    {GL_INTENSITY16F_ARB, MTL_R16Float, UP_R16F, 2, 0, 0, 0, 0, 0, SW(0, 0, 0, 0)},
    {GL_INTENSITY32F_ARB, MTL_R32Float, UP_R32F, 4, 0, 0, 0, 0, 0, SW(0, 0, 0, 0)},
    {GL_LUMINANCE_ALPHA16F_ARB, MTL_RG16Float, UP_RG16F, 4, 0, 0, 0, 0, 0, SW(0, 0, 0, 1)},
    {GL_LUMINANCE_ALPHA32F_ARB, MTL_RG32Float, UP_RG32F, 8, 0, 0, 0, 0, 0, SW(0, 0, 0, 1)},
    {GL_SLUMINANCE, MTL_R8Unorm_sRGB, UP_R8, 1, 0, 0, 0, 0, 1, SW(0, 0, 0, 5)},
    {GL_SLUMINANCE8, MTL_R8Unorm_sRGB, UP_R8, 1, 0, 0, 0, 0, 1, SW(0, 0, 0, 5)},
    {GL_SLUMINANCE_ALPHA, MTL_RGBA8Unorm_sRGB, UP_RGBA8, 4, 0, 0, 0, 0, 1, SW(0, 0, 0, 3)},
    {GL_SLUMINANCE8_ALPHA8, MTL_RGBA8Unorm_sRGB, UP_RGBA8, 4, 0, 0, 0, 0, 1, SW(0, 0, 0, 3)},
    /* EXT_texture_integer luminance / alpha / intensity */
    {GL_ALPHA8UI_EXT, MTL_R8Uint, UP_RAW, 1, 0, 0, 0, 1, 0, SW(4, 4, 4, 0)},
    {GL_LUMINANCE8UI_EXT, MTL_R8Uint, UP_RAW, 1, 0, 0, 0, 1, 0, SW(0, 0, 0, 5)},
    {GL_INTENSITY8UI_EXT, MTL_R8Uint, UP_RAW, 1, 0, 0, 0, 1, 0, SW(0, 0, 0, 0)},
    {GL_LUMINANCE_ALPHA8UI_EXT, MTL_RG8Uint, UP_RAW, 2, 0, 0, 0, 1, 0, SW(0, 0, 0, 1)},
    {GL_ALPHA8I_EXT, MTL_R8Sint, UP_RAW, 1, 0, 0, 0, 1, 0, SW(4, 4, 4, 0)},
    {GL_LUMINANCE8I_EXT, MTL_R8Sint, UP_RAW, 1, 0, 0, 0, 1, 0, SW(0, 0, 0, 5)},
    {GL_INTENSITY8I_EXT, MTL_R8Sint, UP_RAW, 1, 0, 0, 0, 1, 0, SW(0, 0, 0, 0)},
    {GL_LUMINANCE_ALPHA8I_EXT, MTL_RG8Sint, UP_RAW, 2, 0, 0, 0, 1, 0, SW(0, 0, 0, 1)},
    {GL_ALPHA16UI_EXT, MTL_R16Uint, UP_RAW, 2, 0, 0, 0, 1, 0, SW(4, 4, 4, 0)},
    {GL_LUMINANCE16UI_EXT, MTL_R16Uint, UP_RAW, 2, 0, 0, 0, 1, 0, SW(0, 0, 0, 5)},
    {GL_INTENSITY16UI_EXT, MTL_R16Uint, UP_RAW, 2, 0, 0, 0, 1, 0, SW(0, 0, 0, 0)},
    {GL_LUMINANCE_ALPHA16UI_EXT, MTL_RG16Uint, UP_RAW, 4, 0, 0, 0, 1, 0, SW(0, 0, 0, 1)},
    {GL_ALPHA16I_EXT, MTL_R16Sint, UP_RAW, 2, 0, 0, 0, 1, 0, SW(4, 4, 4, 0)},
    {GL_LUMINANCE16I_EXT, MTL_R16Sint, UP_RAW, 2, 0, 0, 0, 1, 0, SW(0, 0, 0, 5)},
    {GL_INTENSITY16I_EXT, MTL_R16Sint, UP_RAW, 2, 0, 0, 0, 1, 0, SW(0, 0, 0, 0)},
    {GL_LUMINANCE_ALPHA16I_EXT, MTL_RG16Sint, UP_RAW, 4, 0, 0, 0, 1, 0, SW(0, 0, 0, 1)},
    {GL_ALPHA32UI_EXT, MTL_R32Uint, UP_RAW, 4, 0, 0, 0, 1, 0, SW(4, 4, 4, 0)},
    {GL_LUMINANCE32UI_EXT, MTL_R32Uint, UP_RAW, 4, 0, 0, 0, 1, 0, SW(0, 0, 0, 5)},
    {GL_INTENSITY32UI_EXT, MTL_R32Uint, UP_RAW, 4, 0, 0, 0, 1, 0, SW(0, 0, 0, 0)},
    {GL_LUMINANCE_ALPHA32UI_EXT, MTL_RG32Uint, UP_RAW, 8, 0, 0, 0, 1, 0, SW(0, 0, 0, 1)},
    {GL_ALPHA32I_EXT, MTL_R32Sint, UP_RAW, 4, 0, 0, 0, 1, 0, SW(4, 4, 4, 0)},
    {GL_LUMINANCE32I_EXT, MTL_R32Sint, UP_RAW, 4, 0, 0, 0, 1, 0, SW(0, 0, 0, 5)},
    {GL_INTENSITY32I_EXT, MTL_R32Sint, UP_RAW, 4, 0, 0, 0, 1, 0, SW(0, 0, 0, 0)},
    {GL_LUMINANCE_ALPHA32I_EXT, MTL_RG32Sint, UP_RAW, 8, 0, 0, 0, 1, 0, SW(0, 0, 0, 1)},
    /* generic compressed formats: stored uncompressed, as Apple does */
    {GL_COMPRESSED_RED, MTL_R8Unorm, UP_R8, 1, 0, 0, 0, 0, 0, SW(0, 4, 4, 5)},
    {GL_COMPRESSED_RG, MTL_RG8Unorm, UP_RG8, 2, 0, 0, 0, 0, 0, SW(0, 1, 4, 5)},
    {GL_COMPRESSED_RGB, MTL_RGBA8Unorm, UP_RGBA8, 4, 0, 0, 0, 0, 0, SW(0, 1, 2, 5)},
    {GL_COMPRESSED_RGBA, MTL_RGBA8Unorm, UP_RGBA8, 4, 0, 0, 0, 0, 0, RGBA},
    {GL_COMPRESSED_ALPHA, MTL_R8Unorm, UP_R8, 1, 0, 0, 0, 0, 0, SW(4, 4, 4, 0)},
    {GL_COMPRESSED_LUMINANCE, MTL_R8Unorm, UP_R8, 1, 0, 0, 0, 0, 0, SW(0, 0, 0, 5)},
    {GL_COMPRESSED_LUMINANCE_ALPHA, MTL_RG8Unorm, UP_RG8, 2, 0, 0, 0, 0, 0, SW(0, 0, 0, 1)},
    {GL_COMPRESSED_INTENSITY, MTL_R8Unorm, UP_R8, 1, 0, 0, 0, 0, 0, SW(0, 0, 0, 0)},
    {GL_COMPRESSED_SRGB, MTL_RGBA8Unorm_sRGB, UP_RGBA8, 4, 0, 0, 0, 0, 1, SW(0, 1, 2, 5)},
    {GL_COMPRESSED_SRGB_ALPHA, MTL_RGBA8Unorm_sRGB, UP_RGBA8, 4, 0, 0, 0, 0, 1, RGBA},
    {GL_COMPRESSED_SLUMINANCE, MTL_R8Unorm_sRGB, UP_R8, 1, 0, 0, 0, 0, 1, SW(0, 0, 0, 5)},
    {GL_COMPRESSED_SLUMINANCE_ALPHA, MTL_RGBA8Unorm_sRGB, UP_RGBA8, 4, 0, 0, 0, 0, 1, SW(0, 0, 0, 3)},
    {GL_COMPRESSED_SIGNED_RED_RGTC1, MTL_BC4_RSnorm, UP_RAW, 8, 1, 0, 0, 0, 0, SW(0, 4, 4, 5)},
    {GL_COMPRESSED_SIGNED_RG_RGTC2, MTL_BC5_RGSnorm, UP_RAW, 16, 1, 0, 0, 0, 0, SW(0, 1, 4, 5)},
    {GL_COMPRESSED_LUMINANCE_ALPHA_3DC_ATI, MTL_BC5_RGUnorm, UP_RAW, 16, 1, 0, 0, 0, 0, SW(0, 0, 0, 1)},
    /* R/RG/RGB sized formats missing above */
    {GL_RG16, MTL_RG16Unorm, UP_RAW, 4, 0, 0, 0, 0, 0, SW(0, 1, 4, 5)},
    {GL_RG8I, MTL_RG8Sint, UP_RG8I, 2, 0, 0, 0, 1, 0, SW(0, 1, 4, 5)},
    {GL_RG16I, MTL_RG16Sint, UP_RG16I, 4, 0, 0, 0, 1, 0, SW(0, 1, 4, 5)},
    {GL_RG32I, MTL_RG32Sint, UP_RG32I, 8, 0, 0, 0, 1, 0, SW(0, 1, 4, 5)},
    {GL_RGB8UI, MTL_RGBA8Uint, UP_RGBA8I, 4, 0, 0, 0, 1, 0, SW(0, 1, 2, 5)},
    {GL_RGB8I, MTL_RGBA8Sint, UP_RGBA8I, 4, 0, 0, 0, 1, 0, SW(0, 1, 2, 5)},
    {GL_RGB16UI, MTL_RGBA16Uint, UP_RGBA16I, 8, 0, 0, 0, 1, 0, SW(0, 1, 2, 5)},
    {GL_RGB16I, MTL_RGBA16Sint, UP_RGBA16I, 8, 0, 0, 0, 1, 0, SW(0, 1, 2, 5)},
    {GL_RGB32UI, MTL_RGBA32Uint, UP_RGBA32I, 16, 0, 0, 0, 1, 0, SW(0, 1, 2, 5)},
    {GL_RGB32I, MTL_RGBA32Sint, UP_RGBA32I, 16, 0, 0, 0, 1, 0, SW(0, 1, 2, 5)},
    {GL_RGB10_A2UI, MTL_RGB10A2Uint, UP_RAW, 4, 0, 0, 0, 1, 0, RGBA},
    {GL_RGB565, MTL_B5G6R5Unorm, UP_565, 2, 0, 0, 0, 0, 0, SW(0, 1, 2, 5)},
    {GL_R8_SNORM, MTL_R8Snorm, UP_RAW, 1, 0, 0, 0, 0, 0, SW(0, 4, 4, 5)},
    {GL_RG8_SNORM, MTL_RG8Snorm, UP_RAW, 2, 0, 0, 0, 0, 0, SW(0, 1, 4, 5)},
    {GL_RGB8_SNORM, MTL_RGBA8Snorm, UP_RAW, 4, 0, 0, 0, 0, 0, SW(0, 1, 2, 5)},
    {GL_RGBA8_SNORM, MTL_RGBA8Snorm, UP_RAW, 4, 0, 0, 0, 0, 0, RGBA},
    {GL_R16_SNORM, MTL_R16Snorm, UP_RAW, 2, 0, 0, 0, 0, 0, SW(0, 4, 4, 5)},
    {GL_RG16_SNORM, MTL_RG16Snorm, UP_RAW, 4, 0, 0, 0, 0, 0, SW(0, 1, 4, 5)},
    {GL_RGB16_SNORM, MTL_RGBA16Snorm, UP_RAW, 8, 0, 0, 0, 0, 0, SW(0, 1, 2, 5)},
    {GL_RGBA16_SNORM, MTL_RGBA16Snorm, UP_RAW, 8, 0, 0, 0, 0, 0, RGBA},
    /* depth / stencil */
    {GL_DEPTH_COMPONENT, MTL_Depth32Float, UP_DEPTH32F, 4, 0, 1, 0, 0, 0, SW(0, 0, 0, 5)},
    /* Apple reports 16 bits but keeps (and reads back) more precision. */
    {GL_DEPTH_COMPONENT16, MTL_Depth32Float, UP_DEPTH32F, 4, 0, 1, 0, 0, 0, SW(0, 0, 0, 5)},
    {GL_DEPTH_COMPONENT24, MTL_Depth32Float, UP_DEPTH32F, 4, 0, 1, 0, 0, 0, SW(0, 0, 0, 5)},
    {GL_DEPTH_COMPONENT32, MTL_Depth32Float, UP_DEPTH32F, 4, 0, 1, 0, 0, 0, SW(0, 0, 0, 5)},
    {GL_DEPTH_COMPONENT32F, MTL_Depth32Float, UP_DEPTH32F, 4, 0, 1, 0, 0, 0, SW(0, 0, 0, 5)},
    {GL_DEPTH_STENCIL, MTL_Depth32Float_Stencil8, UP_DEPTH24S8, 8, 0, 1, 1, 0, 0, SW(0, 0, 0, 5)},
    {GL_DEPTH24_STENCIL8, MTL_Depth32Float_Stencil8, UP_DEPTH24S8, 8, 0, 1, 1, 0, 0, SW(0, 0, 0, 5)},
    {GL_DEPTH32F_STENCIL8, MTL_Depth32Float_Stencil8, UP_DEPTH24S8, 8, 0, 1, 1, 0, 0, SW(0, 0, 0, 5)},
    {GL_STENCIL_INDEX8, MTL_Stencil8, UP_STENCIL8, 1, 0, 0, 1, 0, 0, SW(0, 0, 0, 5)},
};

/* Texture internal formats per profile, as Apple's implementation accepts
   them: core has no luminance/alpha/intensity or numeric formats, legacy
   (GL 2.1) no SNORM, RGB565 or RGB10_A2UI; neither textures stencil only. */
bool glm_texture_format_allowed(GLenum internal_format, bool core)
{
    struct glm_format_info info;
    if (!glm_format_lookup(internal_format, &info)) return false;
    if (internal_format == GL_STENCIL_INDEX8) return false;
    if (core)
        return internal_format > 4 && (info.depth || info.stencil || info.swizzle[0] != info.swizzle[1]);
    switch (internal_format) {
    case GL_R8_SNORM: case GL_RG8_SNORM: case GL_RGB8_SNORM: case GL_RGBA8_SNORM: case GL_R16_SNORM: case GL_RG16_SNORM:
    case GL_RGB16_SNORM: case GL_RGBA16_SNORM: case GL_RGB565: case GL_RGB10_A2UI: return false;
    default: return true;
    }
}

bool glm_format_lookup(GLenum internal_format, struct glm_format_info *out)
{
    for (size_t i = 0; i < sizeof table / sizeof table[0]; ++i) {
        const struct entry *e = &table[i];
        if (e->internal != internal_format) continue;
        *out = (struct glm_format_info){e->metal, e->upload, e->bytes, e->compressed, e->depth, e->stencil,
                                        e->integer, e->srgb, {e->swizzle[0], e->swizzle[1], e->swizzle[2], e->swizzle[3]}};
        return true;
    }
    return false;
}

GLenum glm_base_internal_format(GLenum internal_format, GLenum format)
{
    (void)format;
    return internal_format;
}

/* ---- pixel transfer validation ---------------------------------------------- */

enum { FMT_INVALID, FMT_COLOR, FMT_INTEGER, FMT_DEPTH, FMT_DEPTH_STENCIL, FMT_STENCIL, FMT_INDEX, FMT_YCBCR };

static int format_kind(GLenum format, bool core)
{
    switch (format) {
    case GL_RED: case GL_GREEN: case GL_BLUE: case GL_RG: case GL_RGB: case GL_BGR: case GL_RGBA: case GL_BGRA:
        return FMT_COLOR;
    case GL_ALPHA: case GL_LUMINANCE: case GL_LUMINANCE_ALPHA: case GL_ABGR_EXT: return core ? FMT_INVALID : FMT_COLOR;
    case GL_RED_INTEGER: case GL_GREEN_INTEGER: case GL_BLUE_INTEGER: case GL_RG_INTEGER: case GL_RGB_INTEGER:
    case GL_BGR_INTEGER: case GL_RGBA_INTEGER: case GL_BGRA_INTEGER:
        return FMT_INTEGER;
    case GL_DEPTH_COMPONENT: return FMT_DEPTH;
    case GL_DEPTH_STENCIL: return FMT_DEPTH_STENCIL;
    case GL_STENCIL_INDEX: return FMT_STENCIL;
    case GL_COLOR_INDEX: return core ? FMT_INVALID : FMT_INDEX;
    case GL_YCBCR_422_APPLE: return core ? FMT_INVALID : FMT_YCBCR;
    default: return FMT_INVALID;
    }
}

/* Errors of a format/type pair for pixel transfers, before the internal
   format is considered, as Apple's implementation reports them (measured:
   build/probes/teximage.c). */
GLenum glm_format_type_error(GLenum format, GLenum type, bool core)
{
    int kind = format_kind(format, core);
    if (kind == FMT_INVALID) return GL_INVALID_ENUM;
    bool rgb = format == GL_RGB || format == GL_RGB_INTEGER, rgba = format == GL_RGBA || format == GL_BGRA ||
                                                                   format == GL_RGBA_INTEGER || format == GL_BGRA_INTEGER;
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_UNSIGNED_INT: case GL_INT:
        if (kind == FMT_STENCIL) return GL_INVALID_ENUM;
        return kind == FMT_DEPTH_STENCIL || kind == FMT_YCBCR ? GL_INVALID_OPERATION : GL_NO_ERROR;
    case GL_HALF_FLOAT: case GL_FLOAT:
        if (kind == FMT_STENCIL) return GL_INVALID_ENUM;
        return kind == FMT_DEPTH_STENCIL || kind == FMT_YCBCR || kind == FMT_INTEGER ? GL_INVALID_OPERATION : GL_NO_ERROR;
    case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV: case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV:
        return rgb ? GL_NO_ERROR : GL_INVALID_OPERATION;
    case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_4_4_4_4_REV: case GL_UNSIGNED_SHORT_5_5_5_1:
    case GL_UNSIGNED_SHORT_1_5_5_5_REV: case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV:
    case GL_UNSIGNED_INT_10_10_10_2: case GL_UNSIGNED_INT_2_10_10_10_REV:
        return rgba ? GL_NO_ERROR : GL_INVALID_OPERATION;
    case GL_UNSIGNED_INT_24_8: case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
        return kind == FMT_DEPTH_STENCIL ? GL_NO_ERROR : GL_INVALID_OPERATION;
    case GL_UNSIGNED_INT_10F_11F_11F_REV: case GL_UNSIGNED_INT_5_9_9_9_REV:
        return format == GL_RGB ? GL_NO_ERROR : GL_INVALID_OPERATION;
    case GL_UNSIGNED_SHORT_8_8_APPLE: case GL_UNSIGNED_SHORT_8_8_REV_APPLE:
        return kind == FMT_YCBCR ? GL_NO_ERROR : GL_INVALID_OPERATION;
    case GL_BITMAP: return kind == FMT_INDEX ? GL_NO_ERROR : GL_INVALID_ENUM;
    default: return GL_INVALID_ENUM;
    }
}

/* glTexImage's error for these arguments (0 if none): the format/type pair,
   then the internal format (unknown: GL_INVALID_ENUM; colour, integer and
   depth internal formats each take only their own kind of format). */
/* Errors of reading `format`/`type` back (glReadPixels, glGetTexImage)
   from an image of `internal_format` (0: none), with depth and stencil
   images present as given. */
GLenum glm_read_error(GLenum internal_format, bool depth, bool stencil, GLenum format, GLenum type, bool core,
                      bool read_pixels)
{
    /* Pixel rectangle commands require INVALID_ENUM for this combination. */
    if (read_pixels && format == GL_DEPTH_STENCIL && type != GL_UNSIGNED_INT_24_8 &&
        type != GL_FLOAT_32_UNSIGNED_INT_24_8_REV) return GL_INVALID_ENUM;
    bool plain_type = type == GL_UNSIGNED_BYTE || type == GL_BYTE || type == GL_UNSIGNED_SHORT || type == GL_SHORT ||
                      type == GL_UNSIGNED_INT || type == GL_INT;
    (void)plain_type;
    GLenum error;
    if (format == GL_STENCIL_INDEX && read_pixels) /* glReadPixels only; the types red takes */
        error = glm_format_type_error(GL_RED, type, core);
    else if (format == GL_GREEN_INTEGER || format == GL_BLUE_INTEGER)
        error = glm_format_type_error(GL_RED_INTEGER, type, core); /* the same types as red */
    else
        error = glm_format_type_error(format, type, core);
    if (error || internal_format == (GLenum)-1) return error;
    int kind = format == GL_GREEN_INTEGER || format == GL_BLUE_INTEGER ? FMT_INTEGER
               : format == GL_STENCIL_INDEX ? FMT_STENCIL : format_kind(format, core);
    if (kind == FMT_DEPTH) return depth ? GL_NO_ERROR : GL_INVALID_OPERATION;
    if (kind == FMT_STENCIL) return stencil ? GL_NO_ERROR : GL_INVALID_OPERATION;
    if (kind == FMT_DEPTH_STENCIL) return depth && stencil ? GL_NO_ERROR : GL_INVALID_OPERATION;
    struct glm_format_info info;
    if (!internal_format || !glm_format_lookup(internal_format, &info) || info.depth || info.stencil)
        return GL_INVALID_OPERATION;
    if (info.integer) return kind == FMT_INTEGER ? GL_NO_ERROR : GL_INVALID_OPERATION;
    return kind == FMT_COLOR ? GL_NO_ERROR : GL_INVALID_OPERATION;
}

GLenum glm_tex_image_error(GLenum internal_format, GLenum format, GLenum type, bool core)
{
    GLenum error = glm_format_type_error(format, type, core);
    if (error) return error;
    struct glm_format_info info;
    if (!glm_texture_format_allowed(internal_format, core) || !glm_format_lookup(internal_format, &info))
        return GL_INVALID_ENUM;
    int kind = format_kind(format, core);
    if (info.depth || info.stencil) return kind == FMT_DEPTH || kind == FMT_DEPTH_STENCIL ? GL_NO_ERROR : GL_INVALID_OPERATION;
    if (info.integer) return kind == FMT_INTEGER ? GL_NO_ERROR : GL_INVALID_OPERATION;
    return kind == FMT_COLOR || kind == FMT_INDEX || kind == FMT_YCBCR ? GL_NO_ERROR : GL_INVALID_OPERATION;
}
