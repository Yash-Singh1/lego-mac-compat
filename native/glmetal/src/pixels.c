/* Pixel transfer: client pixels to texture storage, framebuffer texels to
 * client pixels, and glReadPixels. Conversions go through RGBA float. */
#include "glm_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* MTLPixelFormat values handled here (see formats.c). */
enum {
    R8Unorm = 10, R8Snorm = 12, R8Uint = 13, R8Sint = 14, R16Unorm = 20, R16Uint = 23, R16Sint = 24, R16Float = 25,
    RG8Unorm = 30, RG8Snorm = 32, RG8Uint = 33, RG8Sint = 34, B5G6R5Unorm = 40, A1BGR5Unorm = 41, ABGR4Unorm = 42,
    BGR5A1Unorm = 43, R32Uint = 53, R32Sint = 54, R32Float = 55, RG16Unorm = 60, RG16Uint = 63, RG16Sint = 64,
    RG16Float = 65, RGBA8Unorm = 70, RGBA8Unorm_sRGB = 71, RGBA8Snorm = 72, RGBA8Uint = 73, RGBA8Sint = 74,
    BGRA8Unorm = 80, BGRA8Unorm_sRGB = 81, RGB10A2Unorm = 90, RG11B10Float = 92, RGB9E5Float = 93, RG32Uint = 103,
    RG32Sint = 104, RG32Float = 105, RGBA16Unorm = 110, RGBA16Uint = 113, RGBA16Sint = 114, RGBA16Float = 115,
    RGBA32Uint = 123, RGBA32Sint = 124, RGBA32Float = 125, Depth16Unorm = 250, Depth32Float = 252, Stencil8 = 253,
    Depth32Float_Stencil8 = 260, R8Unorm_sRGB = 11, RG8Unorm_sRGB = 31, R16Snorm = 22, RG16Snorm = 62,
    RGBA16Snorm = 112, RGB10A2Uint = 91,
};

size_t glm_metal_format_bytes(uint32_t f);
size_t glm_metal_format_bytes(uint32_t f)
{
    switch (f) {
    case R8Unorm: case R8Unorm_sRGB: case R8Snorm: case R8Uint: case R8Sint: case Stencil8: return 1;
    case R16Unorm: case R16Snorm: case R16Uint: case R16Sint: case R16Float: case RG8Unorm: case RG8Unorm_sRGB:
    case RG8Snorm: case RG8Uint: case RG8Sint:
    case B5G6R5Unorm: case A1BGR5Unorm: case ABGR4Unorm: case BGR5A1Unorm: case Depth16Unorm: return 2;
    case RG16Float: case RG16Unorm: case RG16Snorm: case RG16Uint: case RG16Sint: case R32Uint: case R32Sint:
    case R32Float: case RGB10A2Uint: case RGBA8Unorm: case RGBA8Unorm_sRGB: case RGBA8Snorm: case RGBA8Uint: case RGBA8Sint: case BGRA8Unorm:
    case BGRA8Unorm_sRGB: case RGB10A2Unorm: case RG11B10Float: case RGB9E5Float: case Depth32Float: return 4;
    case RG32Uint: case RG32Sint: case RG32Float: case RGBA16Unorm: case RGBA16Snorm: case RGBA16Uint: case RGBA16Sint: case RGBA16Float:
    case Depth32Float_Stencil8: return 8;
    case RGBA32Uint: case RGBA32Sint: case RGBA32Float: return 16;
    default: return 4;
    }
}

static float half_to_float(uint16_t h)
{
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, exponent = (h >> 10) & 31, mantissa = h & 1023, bits;
    if (exponent == 0) {
        if (!mantissa) bits = sign;
        else {
            exponent = 113;
            while (!(mantissa & 1024)) { mantissa <<= 1; --exponent; }
            bits = sign | exponent << 23 | (mantissa & 1023) << 13;
        }
    } else if (exponent == 31) bits = sign | 0x7f800000 | mantissa << 13;
    else bits = sign | (exponent + 112) << 23 | mantissa << 13;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static uint16_t float_to_half(float f)
{
    uint32_t bits;
    memcpy(&bits, &f, 4);
    uint32_t sign = (bits >> 16) & 0x8000;
    int32_t exponent = (int32_t)((bits >> 23) & 0xff) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffff;
    if (((bits >> 23) & 0xff) == 0xff) return (uint16_t)(sign | 0x7c00 | (mantissa ? 0x200 : 0));
    if (exponent >= 31) return (uint16_t)(sign | 0x7c00);
    if (exponent <= 0) {
        if (exponent < -10) return (uint16_t)sign;
        mantissa |= 0x800000;
        uint32_t shift = (uint32_t)(14 - exponent);
        return (uint16_t)(sign | mantissa >> shift);
    }
    /* Truncated, as Apple's implementation converts client floats. */
    return (uint16_t)(sign | (uint32_t)exponent << 10 | mantissa >> 13);
}

static float unorm(uint32_t v, int bits) { return (float)v / (float)((1u << bits) - 1); }
static float snorm(int32_t v, int bits)
{
    float f = (float)v / (float)((1 << (bits - 1)) - 1);
    return f < -1 ? -1 : f;
}
/* As Apple's implementation converts: NaN to 1, halves rounded up. */
static uint32_t to_unorm(float f, int bits)
{
    if (f != f || f >= 1) return (1u << bits) - 1;
    if (!(f > 0)) return 0;
    return (uint32_t)(f * (float)((1u << bits) - 1) + 0.5f);
}
/* Channels narrower than 8 bits: Apple's implementation keeps the top
   bits of the 8-bit value rather than rounding. */
static uint32_t to_unorm_narrow(float f, int bits) { return to_unorm(f, 8) >> (8 - bits); }

/* Unsigned 11- and 10-bit floats (5-bit exponent) from a float, via the
   truncated half Apple converts through. */
static uint32_t to_small_float(float f, int mantissa_bits)
{
    if (!(f > 0)) return 0;
    uint16_t h = float_to_half(f);
    return (uint32_t)(h >> (10 - mantissa_bits));
}

/* EXT_texture_shared_exponent's RGB9_E5 encoding. */
static uint32_t to_rgb9e5(const float *in)
{
    const float max = 65408.0f; /* (2^9 - 1) / 2^9 * 2^16 */
    float c[3];
    for (int i = 0; i < 3; ++i) c[i] = !(in[i] > 0) ? 0 : in[i] > max ? max : in[i];
    float m = fmaxf(c[0], fmaxf(c[1], c[2]));
    int exp_shared = (m > 0 ? (int)floorf(log2f(m)) : -16);
    if (exp_shared < -16) exp_shared = -16;
    exp_shared += 16;
    double scale = exp2(exp_shared - 15 - 9);
    if ((int)floor(m / scale + 0.5) == 512) {
        exp_shared += 1;
        scale *= 2;
    }
    uint32_t r = (uint32_t)floor(c[0] / scale + 0.5), g = (uint32_t)floor(c[1] / scale + 0.5), b = (uint32_t)floor(c[2] / scale + 0.5);
    return (r & 511) | (g & 511) << 9 | (b & 511) << 18 | (uint32_t)exp_shared << 27;
}

static int32_t to_snorm(float f, int bits)
{
    if (f <= -1) f = -1;
    if (f >= 1) f = 1;
    return (int32_t)lrintf(f * (float)((1 << (bits - 1)) - 1));
}

/* Texels of a Metal format to RGBA float (missing components 0,0,0,1). */
void glm_unpack_texels(uint32_t f, const uint8_t *src, size_t count, float *out);
void glm_unpack_texels(uint32_t f, const uint8_t *src, size_t count, float *out)
{
    size_t size = glm_metal_format_bytes(f);
    for (size_t i = 0; i < count; ++i, src += size, out += 4) {
        out[0] = out[1] = out[2] = 0;
        out[3] = 1;
        uint16_t s16;
        uint32_t u32;
        switch (f) {
        case R8Unorm: case R8Unorm_sRGB: out[0] = unorm(src[0], 8); break;
        case R8Snorm: out[0] = snorm((int8_t)src[0], 8); break;
        case RG8Unorm_sRGB: out[0] = unorm(src[0], 8); out[1] = unorm(src[1], 8); break;
        case R16Snorm: memcpy(&s16, src, 2); out[0] = snorm((int16_t)s16, 16); break;
        case RG16Snorm: for (int c = 0; c < 2; ++c) { memcpy(&s16, src + c * 2, 2); out[c] = snorm((int16_t)s16, 16); } break;
        case RGBA16Snorm: for (int c = 0; c < 4; ++c) { memcpy(&s16, src + c * 2, 2); out[c] = snorm((int16_t)s16, 16); } break;
        case RGB10A2Uint:
            memcpy(&u32, src, 4);
            out[0] = (float)(u32 & 1023); out[1] = (float)((u32 >> 10) & 1023); out[2] = (float)((u32 >> 20) & 1023);
            out[3] = (float)(u32 >> 30);
            break;
        case R8Uint: out[0] = src[0]; break;
        case R8Sint: out[0] = (int8_t)src[0]; break;
        case RG8Unorm: out[0] = unorm(src[0], 8); out[1] = unorm(src[1], 8); break;
        case R16Unorm: memcpy(&s16, src, 2); out[0] = unorm(s16, 16); break;
        case R16Float: memcpy(&s16, src, 2); out[0] = half_to_float(s16); break;
        case RG16Float: for (int c = 0; c < 2; ++c) { memcpy(&s16, src + c * 2, 2); out[c] = half_to_float(s16); } break;
        case R32Float: memcpy(&out[0], src, 4); break;
        case RG32Float: memcpy(out, src, 8); break;
        case RGBA32Float: memcpy(out, src, 16); break;
        case RGBA16Float: for (int c = 0; c < 4; ++c) { memcpy(&s16, src + c * 2, 2); out[c] = half_to_float(s16); } break;
        case RGBA16Unorm: for (int c = 0; c < 4; ++c) { memcpy(&s16, src + c * 2, 2); out[c] = unorm(s16, 16); } break;
        case RGBA8Unorm: case RGBA8Unorm_sRGB: for (int c = 0; c < 4; ++c) out[c] = unorm(src[c], 8); break;
        case RGBA8Snorm: for (int c = 0; c < 4; ++c) out[c] = snorm((int8_t)src[c], 8); break;
        case RGBA8Uint: for (int c = 0; c < 4; ++c) out[c] = src[c]; break;
        case RGBA8Sint: for (int c = 0; c < 4; ++c) out[c] = (int8_t)src[c]; break;
        case BGRA8Unorm: case BGRA8Unorm_sRGB:
            out[0] = unorm(src[2], 8); out[1] = unorm(src[1], 8); out[2] = unorm(src[0], 8); out[3] = unorm(src[3], 8);
            break;
        case RGB10A2Unorm:
            memcpy(&u32, src, 4);
            out[0] = unorm(u32 & 1023, 10); out[1] = unorm((u32 >> 10) & 1023, 10);
            out[2] = unorm((u32 >> 20) & 1023, 10); out[3] = unorm(u32 >> 30, 2);
            break;
        /* Metal names packed formats least-significant component first. */
        case B5G6R5Unorm:
            memcpy(&s16, src, 2);
            out[2] = unorm(s16 & 31, 5); out[1] = unorm((s16 >> 5) & 63, 6); out[0] = unorm(s16 >> 11, 5);
            break;
        case ABGR4Unorm:
            memcpy(&s16, src, 2);
            out[3] = unorm(s16 & 15, 4); out[2] = unorm((s16 >> 4) & 15, 4);
            out[1] = unorm((s16 >> 8) & 15, 4); out[0] = unorm(s16 >> 12, 4);
            break;
        case A1BGR5Unorm:
            memcpy(&s16, src, 2);
            out[3] = (float)(s16 & 1); out[2] = unorm((s16 >> 1) & 31, 5);
            out[1] = unorm((s16 >> 6) & 31, 5); out[0] = unorm(s16 >> 11, 5);
            break;
        case BGR5A1Unorm:
            memcpy(&s16, src, 2);
            out[2] = unorm(s16 & 31, 5); out[1] = unorm((s16 >> 5) & 31, 5);
            out[0] = unorm((s16 >> 10) & 31, 5); out[3] = (float)(s16 >> 15);
            break;
        case R32Uint: memcpy(&u32, src, 4); out[0] = (float)u32; break;
        case RGBA32Uint: for (int c = 0; c < 4; ++c) { memcpy(&u32, src + c * 4, 4); out[c] = (float)u32; } break;
        /* Integer formats read back as their values (the *_INTEGER pack
           formats keep them unnormalized). */
        case R16Uint: memcpy(&s16, src, 2); out[0] = s16; break;
        case R16Sint: memcpy(&s16, src, 2); out[0] = (int16_t)s16; break;
        case RG8Snorm: out[0] = snorm((int8_t)src[0], 8); out[1] = snorm((int8_t)src[1], 8); break;
        case RG8Uint: out[0] = src[0]; out[1] = src[1]; break;
        case RG8Sint: out[0] = (int8_t)src[0]; out[1] = (int8_t)src[1]; break;
        case R32Sint: { int32_t i32; memcpy(&i32, src, 4); out[0] = (float)i32; break; }
        case RG16Unorm: for (int c = 0; c < 2; ++c) { memcpy(&s16, src + c * 2, 2); out[c] = unorm(s16, 16); } break;
        case RG16Uint: for (int c = 0; c < 2; ++c) { memcpy(&s16, src + c * 2, 2); out[c] = s16; } break;
        case RG16Sint: for (int c = 0; c < 2; ++c) { memcpy(&s16, src + c * 2, 2); out[c] = (int16_t)s16; } break;
        case RG32Uint: for (int c = 0; c < 2; ++c) { memcpy(&u32, src + c * 4, 4); out[c] = (float)u32; } break;
        case RG32Sint: for (int c = 0; c < 2; ++c) { int32_t i32; memcpy(&i32, src + c * 4, 4); out[c] = (float)i32; } break;
        case RGBA16Uint: for (int c = 0; c < 4; ++c) { memcpy(&s16, src + c * 2, 2); out[c] = s16; } break;
        case RGBA16Sint: for (int c = 0; c < 4; ++c) { memcpy(&s16, src + c * 2, 2); out[c] = (int16_t)s16; } break;
        case RGBA32Sint: for (int c = 0; c < 4; ++c) { int32_t i32; memcpy(&i32, src + c * 4, 4); out[c] = (float)i32; } break;
        case RG11B10Float: {
            /* 11/11/10-bit floats: 5-bit exponent, no sign. */
            memcpy(&u32, src, 4);
            const uint32_t parts[3] = {u32 & 0x7ff, (u32 >> 11) & 0x7ff, u32 >> 22};
            const int mantissa_bits[3] = {6, 6, 5};
            for (int c = 0; c < 3; ++c) {
                uint32_t e = parts[c] >> mantissa_bits[c], m = parts[c] & ((1u << mantissa_bits[c]) - 1);
                out[c] = half_to_float((uint16_t)(e << 10 | m << (10 - mantissa_bits[c])));
            }
            break;
        }
        case RGB9E5Float: {
            memcpy(&u32, src, 4);
            float scale = exp2f((float)(int)(u32 >> 27) - 15 - 9);
            out[0] = (float)(u32 & 511) * scale;
            out[1] = (float)((u32 >> 9) & 511) * scale;
            out[2] = (float)((u32 >> 18) & 511) * scale;
            break;
        }
        case Depth32Float: memcpy(&out[0], src, 4); break;
        case Depth32Float_Stencil8: memcpy(&out[0], src, 4); out[1] = src[4]; break;
        case Depth16Unorm: memcpy(&s16, src, 2); out[0] = unorm(s16, 16); break;
        default: break;
        }
    }
}

/* RGBA float to texels of a Metal format. */
static void pack_texels(uint32_t f, const float *in, size_t count, uint8_t *dst)
{
    size_t size = glm_metal_format_bytes(f);
    for (size_t i = 0; i < count; ++i, dst += size, in += 4) {
        uint16_t s16;
        uint32_t u32;
        switch (f) {
        case R8Unorm: case R8Unorm_sRGB: dst[0] = (uint8_t)to_unorm(in[0], 8); break;
        case R8Snorm: dst[0] = (uint8_t)to_snorm(in[0], 8); break;
        case RG8Unorm_sRGB: dst[0] = (uint8_t)to_unorm(in[0], 8); dst[1] = (uint8_t)to_unorm(in[1], 8); break;
        case RG8Snorm: dst[0] = (uint8_t)to_snorm(in[0], 8); dst[1] = (uint8_t)to_snorm(in[1], 8); break;
        case R16Snorm: s16 = (uint16_t)to_snorm(in[0], 16); memcpy(dst, &s16, 2); break;
        case RG16Unorm: for (int c = 0; c < 2; ++c) { s16 = (uint16_t)to_unorm(in[c], 16); memcpy(dst + c * 2, &s16, 2); } break;
        case RG16Snorm: for (int c = 0; c < 2; ++c) { s16 = (uint16_t)to_snorm(in[c], 16); memcpy(dst + c * 2, &s16, 2); } break;
        case RGBA16Snorm: for (int c = 0; c < 4; ++c) { s16 = (uint16_t)to_snorm(in[c], 16); memcpy(dst + c * 2, &s16, 2); } break;
        case RGB10A2Uint:
            u32 = ((uint32_t)in[0] & 1023) | ((uint32_t)in[1] & 1023) << 10 | ((uint32_t)in[2] & 1023) << 20 | ((uint32_t)in[3] & 3) << 30;
            memcpy(dst, &u32, 4);
            break;
        case R8Uint: case R8Sint: dst[0] = (uint8_t)(int32_t)in[0]; break;
        case RG8Unorm: dst[0] = (uint8_t)to_unorm(in[0], 8); dst[1] = (uint8_t)to_unorm(in[1], 8); break;
        case RG8Uint: case RG8Sint: dst[0] = (uint8_t)(int32_t)in[0]; dst[1] = (uint8_t)(int32_t)in[1]; break;
        case R16Unorm: s16 = (uint16_t)to_unorm(in[0], 16); memcpy(dst, &s16, 2); break;
        case R16Uint: case R16Sint: s16 = (uint16_t)(int32_t)in[0]; memcpy(dst, &s16, 2); break;
        case R16Float: s16 = float_to_half(in[0]); memcpy(dst, &s16, 2); break;
        case RG16Float: for (int c = 0; c < 2; ++c) { s16 = float_to_half(in[c]); memcpy(dst + c * 2, &s16, 2); } break;
        case RG16Uint: case RG16Sint: for (int c = 0; c < 2; ++c) { s16 = (uint16_t)(int32_t)in[c]; memcpy(dst + c * 2, &s16, 2); } break;
        case R32Float: memcpy(dst, &in[0], 4); break;
        case R32Uint: case R32Sint: u32 = (uint32_t)(int64_t)in[0]; memcpy(dst, &u32, 4); break;
        case RG32Float: memcpy(dst, in, 8); break;
        case RG32Uint: case RG32Sint: for (int c = 0; c < 2; ++c) { u32 = (uint32_t)(int64_t)in[c]; memcpy(dst + c * 4, &u32, 4); } break;
        case RGBA32Float: memcpy(dst, in, 16); break;
        case RGBA32Uint: case RGBA32Sint: for (int c = 0; c < 4; ++c) { u32 = (uint32_t)(int64_t)in[c]; memcpy(dst + c * 4, &u32, 4); } break;
        case RGBA16Float: for (int c = 0; c < 4; ++c) { s16 = float_to_half(in[c]); memcpy(dst + c * 2, &s16, 2); } break;
        case RGBA16Unorm: for (int c = 0; c < 4; ++c) { s16 = (uint16_t)to_unorm(in[c], 16); memcpy(dst + c * 2, &s16, 2); } break;
        case RGBA16Uint: case RGBA16Sint: for (int c = 0; c < 4; ++c) { s16 = (uint16_t)(int32_t)in[c]; memcpy(dst + c * 2, &s16, 2); } break;
        case RGBA8Unorm: case RGBA8Unorm_sRGB: for (int c = 0; c < 4; ++c) dst[c] = (uint8_t)to_unorm(in[c], 8); break;
        case RGBA8Snorm: for (int c = 0; c < 4; ++c) dst[c] = (uint8_t)to_snorm(in[c], 8); break;
        case RGBA8Uint: case RGBA8Sint: for (int c = 0; c < 4; ++c) dst[c] = (uint8_t)(int32_t)in[c]; break;
        case BGRA8Unorm: case BGRA8Unorm_sRGB:
            dst[0] = (uint8_t)to_unorm(in[2], 8); dst[1] = (uint8_t)to_unorm(in[1], 8);
            dst[2] = (uint8_t)to_unorm(in[0], 8); dst[3] = (uint8_t)to_unorm(in[3], 8);
            break;
        case RGB10A2Unorm:
            u32 = to_unorm(in[0], 10) | to_unorm(in[1], 10) << 10 | to_unorm(in[2], 10) << 20 | to_unorm(in[3], 2) << 30;
            memcpy(dst, &u32, 4);
            break;
        case B5G6R5Unorm:
            s16 = (uint16_t)(to_unorm_narrow(in[2], 5) | to_unorm_narrow(in[1], 6) << 5 | to_unorm_narrow(in[0], 5) << 11);
            memcpy(dst, &s16, 2);
            break;
        case ABGR4Unorm:
            s16 = (uint16_t)(to_unorm_narrow(in[3], 4) | to_unorm_narrow(in[2], 4) << 4 | to_unorm_narrow(in[1], 4) << 8 |
                             to_unorm_narrow(in[0], 4) << 12);
            memcpy(dst, &s16, 2);
            break;
        case A1BGR5Unorm:
            s16 = (uint16_t)(to_unorm_narrow(in[3], 1) | to_unorm_narrow(in[2], 5) << 1 | to_unorm_narrow(in[1], 5) << 6 |
                             to_unorm_narrow(in[0], 5) << 11);
            memcpy(dst, &s16, 2);
            break;
        case BGR5A1Unorm:
            s16 = (uint16_t)(to_unorm_narrow(in[2], 5) | to_unorm_narrow(in[1], 5) << 5 | to_unorm_narrow(in[0], 5) << 10 |
                             to_unorm_narrow(in[3], 1) << 15);
            memcpy(dst, &s16, 2);
            break;
        case RG11B10Float:
            u32 = to_small_float(in[0], 6) | to_small_float(in[1], 6) << 11 | to_small_float(in[2], 5) << 22;
            memcpy(dst, &u32, 4);
            break;
        case RGB9E5Float: u32 = to_rgb9e5(in); memcpy(dst, &u32, 4); break;
        case Depth32Float: memcpy(dst, &in[0], 4); break;
        case Depth32Float_Stencil8: /* depth, then the stencil index (a byte, padded) */
            memcpy(dst, &in[0], 4);
            u32 = in[1] < 0 ? 0 : in[1] > 255 ? 255 : (uint32_t)in[1];
            memcpy(dst + 4, &u32, 4);
            break;
        case Depth16Unorm: s16 = (uint16_t)to_unorm(in[0], 16); memcpy(dst, &s16, 2); break;
        default: memset(dst, 0, size); break;
        }
    }
}

/* ---- client layouts ------------------------------------------------------ */

static int format_components(GLenum format)
{
    switch (format) {
    case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_LUMINANCE: case GL_DEPTH_COMPONENT:
    case GL_STENCIL_INDEX: case GL_RED_INTEGER: case GL_GREEN_INTEGER: case GL_BLUE_INTEGER: case GL_INTENSITY:
    case GL_COLOR_INDEX: return 1;
    case GL_LUMINANCE_ALPHA: case GL_RG: case GL_RG_INTEGER: case GL_DEPTH_STENCIL: return 2;
    case GL_RGB: case GL_BGR: case GL_RGB_INTEGER: case GL_BGR_INTEGER: return 3;
    case GL_RGBA: case GL_BGRA: case GL_RGBA_INTEGER: case GL_BGRA_INTEGER: case GL_ABGR_EXT: return 4;
    default: return 0;
    }
}

static bool packed_type(GLenum type, int *bytes)
{
    switch (type) {
    case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV: *bytes = 1; return true;
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV: case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_4_4_4_4_REV: case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_SHORT_1_5_5_5_REV:
        *bytes = 2; return true;
    case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV: case GL_UNSIGNED_INT_10_10_10_2:
    case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_24_8: case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV:
        *bytes = 4; return true;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: *bytes = 8; return true;
    default: return false;
    }
}

static int type_bytes(GLenum type)
{
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: return 1;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: return 2;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: return 4;
    default: return 0;
    }
}

static size_t pixel_bytes(GLenum format, GLenum type);
size_t glm_pixel_bytes(GLenum format, GLenum type) { return pixel_bytes(format, type); }

/* GL_(UN)PACK_SWAP_BYTES: the `bytes` of one client pixel with each element
   (component, or packed value) byte-reversed, in `tmp`; `p` otherwise. */
static const uint8_t *swapped_pixel(const uint8_t *p, size_t bytes, GLenum type, bool swap, uint8_t *tmp)
{
    if (!swap) return p;
    size_t element = (size_t)type_bytes(type);
    if (!element) element = type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV ? 4 : bytes;
    if (element < 2) return p;
    for (size_t i = 0; i + element <= bytes && i + element <= 32; i += element)
        for (size_t k = 0; k < element; ++k) tmp[i + k] = p[i + element - 1 - k];
    return tmp;
}

static void swap_output_pixel(uint8_t *p, size_t bytes, GLenum type)
{
    uint8_t tmp[32];
    const uint8_t *swapped = swapped_pixel(p, bytes, type, true, tmp);
    if (swapped != p) memcpy(p, swapped, bytes);
}

static size_t pixel_bytes(GLenum format, GLenum type)
{
    int bytes;
    if (packed_type(type, &bytes)) return (size_t)bytes;
    return (size_t)(format_components(format) * type_bytes(type));
}

static bool is_integer_format(GLenum format)
{
    return format == GL_RED_INTEGER || format == GL_GREEN_INTEGER || format == GL_BLUE_INTEGER || format == GL_RG_INTEGER ||
           format == GL_RGB_INTEGER || format == GL_RGBA_INTEGER || format == GL_BGR_INTEGER || format == GL_BGRA_INTEGER;
}

/* Integer client components (for *_INTEGER formats), in format order. */
static void decode_pixel_int(GLenum type, const uint8_t *p, int64_t *c, int n)
{
    uint32_t v;
    switch (type) {
    case GL_UNSIGNED_BYTE: for (int i = 0; i < n; ++i) c[i] = p[i]; return;
    case GL_BYTE: for (int i = 0; i < n; ++i) c[i] = (int8_t)p[i]; return;
    case GL_UNSIGNED_SHORT: for (int i = 0; i < n; ++i) { uint16_t s; memcpy(&s, p + i * 2, 2); c[i] = s; } return;
    case GL_SHORT: for (int i = 0; i < n; ++i) { int16_t s; memcpy(&s, p + i * 2, 2); c[i] = s; } return;
    case GL_UNSIGNED_INT: for (int i = 0; i < n; ++i) { uint32_t s; memcpy(&s, p + i * 4, 4); c[i] = s; } return;
    case GL_INT: for (int i = 0; i < n; ++i) { int32_t s; memcpy(&s, p + i * 4, 4); c[i] = s; } return;
    case GL_UNSIGNED_INT_2_10_10_10_REV:
        memcpy(&v, p, 4); c[0] = v & 1023; c[1] = (v >> 10) & 1023; c[2] = (v >> 20) & 1023; c[3] = v >> 30; return;
    case GL_UNSIGNED_INT_10_10_10_2:
        memcpy(&v, p, 4); c[0] = v >> 22; c[1] = (v >> 12) & 1023; c[2] = (v >> 2) & 1023; c[3] = v & 3; return;
    case GL_UNSIGNED_INT_8_8_8_8_REV: memcpy(&v, p, 4); for (int i = 0; i < 4; ++i) c[i] = (v >> (8 * i)) & 255; return;
    case GL_UNSIGNED_INT_8_8_8_8: memcpy(&v, p, 4); for (int i = 0; i < 4; ++i) c[i] = (v >> (24 - 8 * i)) & 255; return;
    case GL_UNSIGNED_BYTE_3_3_2: v = p[0]; c[0] = v >> 5; c[1] = (v >> 2) & 7; c[2] = v & 3; return;
    case GL_UNSIGNED_BYTE_2_3_3_REV: v = p[0]; c[0] = v & 7; c[1] = (v >> 3) & 7; c[2] = v >> 6; return;
    }
    uint16_t h;
    memcpy(&h, p, 2);
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5: c[0] = h >> 11; c[1] = (h >> 5) & 63; c[2] = h & 31; return;
    case GL_UNSIGNED_SHORT_5_6_5_REV: c[0] = h & 31; c[1] = (h >> 5) & 63; c[2] = h >> 11; return;
    case GL_UNSIGNED_SHORT_4_4_4_4: c[0] = h >> 12; c[1] = (h >> 8) & 15; c[2] = (h >> 4) & 15; c[3] = h & 15; return;
    case GL_UNSIGNED_SHORT_4_4_4_4_REV: c[0] = h & 15; c[1] = (h >> 4) & 15; c[2] = (h >> 8) & 15; c[3] = h >> 12; return;
    case GL_UNSIGNED_SHORT_5_5_5_1: c[0] = h >> 11; c[1] = (h >> 6) & 31; c[2] = (h >> 1) & 31; c[3] = h & 1; return;
    case GL_UNSIGNED_SHORT_1_5_5_5_REV: c[0] = h & 31; c[1] = (h >> 5) & 31; c[2] = (h >> 10) & 31; c[3] = h >> 15; return;
    default: return;
    }
}

static void int_to_rgba(GLenum format, const int64_t *c, int64_t *rgba)
{
    rgba[0] = rgba[1] = rgba[2] = 0;
    rgba[3] = 1;
    switch (format) {
    case GL_RED_INTEGER: rgba[0] = c[0]; break;
    case GL_GREEN_INTEGER: rgba[1] = c[0]; break;
    case GL_BLUE_INTEGER: rgba[2] = c[0]; break;
    case GL_RG_INTEGER: rgba[0] = c[0]; rgba[1] = c[1]; break;
    case GL_RGB_INTEGER: rgba[0] = c[0]; rgba[1] = c[1]; rgba[2] = c[2]; break;
    case GL_BGR_INTEGER: rgba[0] = c[2]; rgba[1] = c[1]; rgba[2] = c[0]; break;
    case GL_RGBA_INTEGER: memcpy(rgba, c, 4 * sizeof *c); break;
    case GL_BGRA_INTEGER: rgba[0] = c[2]; rgba[1] = c[1]; rgba[2] = c[0]; rgba[3] = c[3]; break;
    }
}

static void pack_texel_int(uint32_t f, const int64_t *in, uint8_t *dst);

/* Texels of a Metal format from RGBA floats / integers (mipmap generation). */
GLM_HIDDEN void glm_pack_texels(uint32_t f, const float *in, size_t count, uint8_t *dst) { pack_texels(f, in, count, dst); }
GLM_HIDDEN void glm_pack_texels_int(uint32_t f, const int64_t *in, size_t count, uint8_t *dst)
{
    size_t size = glm_metal_format_bytes(f);
    for (size_t i = 0; i < count; ++i) pack_texel_int(f, in + i * 4, dst + i * size);
}

/* One texel of an integer Metal format; values wrap to the channel size. */
static void pack_texel_int(uint32_t f, const int64_t *in, uint8_t *dst)
{
    int channels = 4, bytes = 4;
    switch (f) {
    case R8Uint: case R8Sint: channels = 1; bytes = 1; break;
    case RG8Uint: case RG8Sint: channels = 2; bytes = 1; break;
    case RGBA8Uint: case RGBA8Sint: bytes = 1; break;
    case R16Uint: case R16Sint: channels = 1; bytes = 2; break;
    case RG16Uint: case RG16Sint: channels = 2; bytes = 2; break;
    case RGBA16Uint: case RGBA16Sint: bytes = 2; break;
    case R32Uint: case R32Sint: channels = 1; break;
    case RG32Uint: case RG32Sint: channels = 2; break;
    case RGBA32Uint: case RGBA32Sint: break;
    case RGB10A2Uint: {
        int64_t c[4];
        for (int i = 0; i < 4; ++i) c[i] = in[i] < 0 ? 0 : in[i] > (i == 3 ? 3 : 1023) ? (i == 3 ? 3 : 1023) : in[i];
        uint32_t u = (uint32_t)c[0] | (uint32_t)c[1] << 10 | (uint32_t)c[2] << 20 | (uint32_t)c[3] << 30;
        memcpy(dst, &u, 4);
        return;
    }
    default: return;
    }
    /* Clamped to the channel's range, as Apple's implementation converts. */
    bool is_signed = f == R8Sint || f == RG8Sint || f == RGBA8Sint || f == R16Sint || f == RG16Sint || f == RGBA16Sint ||
                     f == R32Sint || f == RG32Sint || f == RGBA32Sint;
    int bits = bytes * 8;
    int64_t lo = is_signed ? -((int64_t)1 << (bits - 1)) : 0;
    int64_t hi = is_signed ? ((int64_t)1 << (bits - 1)) - 1 : ((int64_t)1 << bits) - 1;
    for (int c = 0; c < channels; ++c) {
        int64_t clamped = in[c] < lo ? lo : in[c] > hi ? hi : in[c];
        uint32_t v = (uint32_t)clamped;
        memcpy(dst + c * bytes, &v, (size_t)bytes); /* little-endian: the low bytes */
    }
}

/* Texels of an integer Metal format as RGBA integers (missing: 0, alpha 1);
   false for other formats. */
GLM_HIDDEN bool glm_unpack_texels_int(uint32_t f, const uint8_t *src, size_t count, int64_t *out)
{
    int channels = 4, bytes = 4;
    bool is_signed = false;
    switch (f) {
    case R8Sint: is_signed = true; /* fall through */ case R8Uint: channels = 1; bytes = 1; break;
    case RG8Sint: is_signed = true; /* fall through */ case RG8Uint: channels = 2; bytes = 1; break;
    case RGBA8Sint: is_signed = true; /* fall through */ case RGBA8Uint: bytes = 1; break;
    case R16Sint: is_signed = true; /* fall through */ case R16Uint: channels = 1; bytes = 2; break;
    case RG16Sint: is_signed = true; /* fall through */ case RG16Uint: channels = 2; bytes = 2; break;
    case RGBA16Sint: is_signed = true; /* fall through */ case RGBA16Uint: bytes = 2; break;
    case R32Sint: is_signed = true; /* fall through */ case R32Uint: channels = 1; break;
    case RG32Sint: is_signed = true; /* fall through */ case RG32Uint: channels = 2; break;
    case RGBA32Sint: is_signed = true; /* fall through */ case RGBA32Uint: break;
    case RGB10A2Uint:
        for (size_t i = 0; i < count; ++i) {
            uint32_t v;
            memcpy(&v, src + i * 4, 4);
            int64_t *o = out + i * 4;
            o[0] = v & 1023; o[1] = (v >> 10) & 1023; o[2] = (v >> 20) & 1023; o[3] = v >> 30;
        }
        return true;
    default: return false;
    }
    for (size_t i = 0; i < count; ++i) {
        int64_t *o = out + i * 4;
        o[0] = o[1] = o[2] = 0;
        o[3] = 1;
        for (int c = 0; c < channels; ++c) {
            const uint8_t *p = src + (i * (size_t)channels + (size_t)c) * (size_t)bytes;
            uint32_t u = 0;
            memcpy(&u, p, (size_t)bytes);
            if (is_signed) o[c] = bytes == 1 ? (int8_t)u : bytes == 2 ? (int16_t)u : (int32_t)u;
            else o[c] = u;
        }
    }
    return true;
}

static int64_t clamp_to(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* RGBA integers packed as `format`/`type` (an integer format), clamped to
   the type, with the pack state's layout. */
GLM_HIDDEN bool glm_convert_pack_int(struct glm_context *ctx, const int64_t *rgba, GLsizei width, GLsizei height,
                                     GLenum format, GLenum type, void *out)
{
    const struct glm_state *s = &ctx->state;
    size_t bytes = pixel_bytes(format, type);
    if (!bytes) return false;
    size_t row_pixels = s->pack_row_length > 0 ? (size_t)s->pack_row_length : (size_t)width;
    size_t alignment = (size_t)(s->pack_alignment > 0 ? s->pack_alignment : 4);
    size_t row = s->pack_row_bytes > 0 ? (size_t)s->pack_row_bytes : (row_pixels * bytes + alignment - 1) / alignment * alignment;
    uint8_t *base = (uint8_t *)out + (size_t)s->pack_skip_rows * row + (size_t)s->pack_skip_pixels * bytes;
    int n = format_components(format);
    for (GLsizei y = 0; y < height; ++y)
        for (GLsizei x = 0; x < width; ++x) {
            const int64_t *v = rgba + ((size_t)y * width + x) * 4;
            uint8_t *p = base + (size_t)y * row + (size_t)x * bytes;
            int64_t c[4];
            switch (format) {
            case GL_GREEN_INTEGER: c[0] = v[1]; break;
            case GL_BLUE_INTEGER: c[0] = v[2]; break;
            case GL_BGR_INTEGER: c[0] = v[2]; c[1] = v[1]; c[2] = v[0]; break;
            case GL_BGRA_INTEGER: c[0] = v[2]; c[1] = v[1]; c[2] = v[0]; c[3] = v[3]; break;
            default: memcpy(c, v, sizeof c); break;
            }
            uint32_t u;
            switch (type) {
            case GL_UNSIGNED_BYTE: for (int i = 0; i < n; ++i) p[i] = (uint8_t)clamp_to(c[i], 0, 255); break;
            case GL_BYTE: for (int i = 0; i < n; ++i) p[i] = (uint8_t)(int8_t)clamp_to(c[i], -128, 127); break;
            case GL_UNSIGNED_SHORT: for (int i = 0; i < n; ++i) { uint16_t t = (uint16_t)clamp_to(c[i], 0, 65535); memcpy(p + i * 2, &t, 2); } break;
            case GL_SHORT: for (int i = 0; i < n; ++i) { int16_t t = (int16_t)clamp_to(c[i], -32768, 32767); memcpy(p + i * 2, &t, 2); } break;
            case GL_UNSIGNED_INT: for (int i = 0; i < n; ++i) { uint32_t t = (uint32_t)clamp_to(c[i], 0, 4294967295LL); memcpy(p + i * 4, &t, 4); } break;
            case GL_INT: for (int i = 0; i < n; ++i) { int32_t t = (int32_t)clamp_to(c[i], INT32_MIN, INT32_MAX); memcpy(p + i * 4, &t, 4); } break;
            case GL_UNSIGNED_INT_2_10_10_10_REV:
                u = (uint32_t)clamp_to(c[0], 0, 1023) | (uint32_t)clamp_to(c[1], 0, 1023) << 10 |
                    (uint32_t)clamp_to(c[2], 0, 1023) << 20 | (uint32_t)clamp_to(c[3], 0, 3) << 30;
                memcpy(p, &u, 4);
                break;
            case GL_UNSIGNED_INT_10_10_10_2:
                u = (uint32_t)clamp_to(c[0], 0, 1023) << 22 | (uint32_t)clamp_to(c[1], 0, 1023) << 12 |
                    (uint32_t)clamp_to(c[2], 0, 1023) << 2 | (uint32_t)clamp_to(c[3], 0, 3);
                memcpy(p, &u, 4);
                break;
            case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV:
                u = 0;
                for (int i = 0; i < 4; ++i)
                    u |= (uint32_t)clamp_to(c[i], 0, 255) << (type == GL_UNSIGNED_INT_8_8_8_8 ? 24 - 8 * i : 8 * i);
                memcpy(p, &u, 4);
                break;
            case GL_UNSIGNED_BYTE_3_3_2:
                p[0] = (uint8_t)(clamp_to(c[0], 0, 7) << 5 | clamp_to(c[1], 0, 7) << 2 | clamp_to(c[2], 0, 3));
                break;
            case GL_UNSIGNED_BYTE_2_3_3_REV:
                p[0] = (uint8_t)(clamp_to(c[0], 0, 7) | clamp_to(c[1], 0, 7) << 3 | clamp_to(c[2], 0, 3) << 6);
                break;
            default: {
                uint16_t h;
                switch (type) {
                case GL_UNSIGNED_SHORT_5_6_5: h = (uint16_t)(clamp_to(c[0], 0, 31) << 11 | clamp_to(c[1], 0, 63) << 5 | clamp_to(c[2], 0, 31)); break;
                case GL_UNSIGNED_SHORT_5_6_5_REV: h = (uint16_t)(clamp_to(c[0], 0, 31) | clamp_to(c[1], 0, 63) << 5 | clamp_to(c[2], 0, 31) << 11); break;
                case GL_UNSIGNED_SHORT_4_4_4_4:
                    h = (uint16_t)(clamp_to(c[0], 0, 15) << 12 | clamp_to(c[1], 0, 15) << 8 | clamp_to(c[2], 0, 15) << 4 | clamp_to(c[3], 0, 15));
                    break;
                case GL_UNSIGNED_SHORT_4_4_4_4_REV:
                    h = (uint16_t)(clamp_to(c[0], 0, 15) | clamp_to(c[1], 0, 15) << 4 | clamp_to(c[2], 0, 15) << 8 | clamp_to(c[3], 0, 15) << 12);
                    break;
                case GL_UNSIGNED_SHORT_5_5_5_1:
                    h = (uint16_t)(clamp_to(c[0], 0, 31) << 11 | clamp_to(c[1], 0, 31) << 6 | clamp_to(c[2], 0, 31) << 1 | clamp_to(c[3], 0, 1));
                    break;
                case GL_UNSIGNED_SHORT_1_5_5_5_REV:
                    h = (uint16_t)(clamp_to(c[0], 0, 31) | clamp_to(c[1], 0, 31) << 5 | clamp_to(c[2], 0, 31) << 10 | clamp_to(c[3], 0, 1) << 15);
                    break;
                default: return false;
                }
                memcpy(p, &h, 2);
                break;
            }
            }
            if (s->pack_swap_bytes) swap_output_pixel(p, bytes, type);
        }
    return true;
}

/* Decodes one client pixel into its components in format order, normalized
   for unsigned/signed types unless the format is an integer one. */
/* `narrow`: the storage keeps at most 8 bits a channel. Apple's
   implementation then takes the top bits of wider unsigned integers
   (rather than rounding); the values here land exactly on those 8-bit
   results. (Signed ones round, measured: build/probes/texround.c.) */
static void decode_pixel(GLenum format, GLenum type, const uint8_t *p, float *c, int n, bool narrow)
{
    bool integer = is_integer_format(format);
    uint32_t v;
    if (narrow && !integer) {
        switch (type) {
        case GL_UNSIGNED_SHORT: for (int i = 0; i < n; ++i) { uint16_t s; memcpy(&s, p + i * 2, 2); c[i] = (float)(s >> 8) / 255.0f; } return;
        case GL_UNSIGNED_INT: for (int i = 0; i < n; ++i) { uint32_t s; memcpy(&s, p + i * 4, 4); c[i] = (float)(s >> 24) / 255.0f; } return;
        }
    }
    switch (type) {
    case GL_UNSIGNED_BYTE: for (int i = 0; i < n; ++i) c[i] = integer ? p[i] : p[i] / 255.0f; return;
    case GL_BYTE: for (int i = 0; i < n; ++i) c[i] = integer ? (int8_t)p[i] : snorm((int8_t)p[i], 8); return;
    case GL_UNSIGNED_SHORT: for (int i = 0; i < n; ++i) { uint16_t s; memcpy(&s, p + i * 2, 2); c[i] = integer ? s : s / 65535.0f; } return;
    case GL_SHORT: for (int i = 0; i < n; ++i) { int16_t s; memcpy(&s, p + i * 2, 2); c[i] = integer ? s : snorm(s, 16); } return;
    case GL_UNSIGNED_INT: for (int i = 0; i < n; ++i) { uint32_t s; memcpy(&s, p + i * 4, 4); c[i] = integer ? (float)s : (float)(s / 4294967295.0); } return;
    case GL_INT: for (int i = 0; i < n; ++i) { int32_t s; memcpy(&s, p + i * 4, 4); c[i] = integer ? (float)s : (float)fmax(s / 2147483647.0, -1.0); } return;
    case GL_FLOAT: memcpy(c, p, (size_t)n * 4); return;
    case GL_HALF_FLOAT: for (int i = 0; i < n; ++i) { uint16_t s; memcpy(&s, p + i * 2, 2); c[i] = half_to_float(s); } return;
    case GL_UNSIGNED_BYTE_3_3_2: v = p[0]; c[0] = unorm(v >> 5, 3); c[1] = unorm((v >> 2) & 7, 3); c[2] = unorm(v & 3, 2); return;
    case GL_UNSIGNED_BYTE_2_3_3_REV: v = p[0]; c[0] = unorm(v & 7, 3); c[1] = unorm((v >> 3) & 7, 3); c[2] = unorm(v >> 6, 2); return;
    }
    uint16_t s;
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5: memcpy(&s, p, 2); c[0] = unorm(s >> 11, 5); c[1] = unorm((s >> 5) & 63, 6); c[2] = unorm(s & 31, 5); return;
    case GL_UNSIGNED_SHORT_5_6_5_REV: memcpy(&s, p, 2); c[0] = unorm(s & 31, 5); c[1] = unorm((s >> 5) & 63, 6); c[2] = unorm(s >> 11, 5); return;
    case GL_UNSIGNED_SHORT_4_4_4_4: memcpy(&s, p, 2); c[0] = unorm(s >> 12, 4); c[1] = unorm((s >> 8) & 15, 4); c[2] = unorm((s >> 4) & 15, 4); c[3] = unorm(s & 15, 4); return;
    case GL_UNSIGNED_SHORT_4_4_4_4_REV: memcpy(&s, p, 2); c[0] = unorm(s & 15, 4); c[1] = unorm((s >> 4) & 15, 4); c[2] = unorm((s >> 8) & 15, 4); c[3] = unorm(s >> 12, 4); return;
    case GL_UNSIGNED_SHORT_5_5_5_1: memcpy(&s, p, 2); c[0] = unorm(s >> 11, 5); c[1] = unorm((s >> 6) & 31, 5); c[2] = unorm((s >> 1) & 31, 5); c[3] = (float)(s & 1); return;
    case GL_UNSIGNED_SHORT_1_5_5_5_REV: memcpy(&s, p, 2); c[0] = unorm(s & 31, 5); c[1] = unorm((s >> 5) & 31, 5); c[2] = unorm((s >> 10) & 31, 5); c[3] = (float)(s >> 15); return;
    }
    memcpy(&v, p, 4);
    switch (type) {
    case GL_UNSIGNED_INT_8_8_8_8: c[0] = unorm(v >> 24, 8); c[1] = unorm((v >> 16) & 255, 8); c[2] = unorm((v >> 8) & 255, 8); c[3] = unorm(v & 255, 8); return;
    case GL_UNSIGNED_INT_8_8_8_8_REV: c[0] = unorm(v & 255, 8); c[1] = unorm((v >> 8) & 255, 8); c[2] = unorm((v >> 16) & 255, 8); c[3] = unorm(v >> 24, 8); return;
    case GL_UNSIGNED_INT_10_10_10_2: c[0] = unorm(v >> 22, 10); c[1] = unorm((v >> 12) & 1023, 10); c[2] = unorm((v >> 2) & 1023, 10); c[3] = unorm(v & 3, 2); return;
    case GL_UNSIGNED_INT_2_10_10_10_REV: c[0] = unorm(v & 1023, 10); c[1] = unorm((v >> 10) & 1023, 10); c[2] = unorm((v >> 20) & 1023, 10); c[3] = unorm(v >> 30, 2); return;
    case GL_UNSIGNED_INT_10F_11F_11F_REV: {
        const uint32_t parts[3] = {v & 0x7ff, (v >> 11) & 0x7ff, v >> 22};
        const int mantissa_bits[3] = {6, 6, 5};
        for (int i = 0; i < 3; ++i) {
            uint32_t e = parts[i] >> mantissa_bits[i], m = parts[i] & ((1u << mantissa_bits[i]) - 1);
            c[i] = half_to_float((uint16_t)(e << 10 | m << (10 - mantissa_bits[i])));
        }
        return;
    }
    case GL_UNSIGNED_INT_5_9_9_9_REV: {
        float scale = exp2f((float)(int)(v >> 27) - 15 - 9);
        c[0] = (float)(v & 511) * scale;
        c[1] = (float)((v >> 9) & 511) * scale;
        c[2] = (float)((v >> 18) & 511) * scale;
        return;
    }
    case GL_UNSIGNED_INT_24_8: c[0] = unorm(v >> 8, 24); c[1] = (float)(v & 255); return;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: memcpy(&c[0], p, 4); memcpy(&v, p + 4, 4); c[1] = (float)(v & 255); return;
    }
}

/* Places decoded components into RGBA per GL's conversion to RGBA
   (GL 2.1 3.6.4); luminance to R, G and B (3.6.5, "Conversion to RGBA"). */
static void to_rgba(GLenum format, const float *c, float *rgba)
{
    rgba[0] = rgba[1] = rgba[2] = 0;
    rgba[3] = 1;
    switch (format) {
    case GL_RED: case GL_RED_INTEGER: case GL_INTENSITY: case GL_DEPTH_COMPONENT: rgba[0] = c[0]; break;
    case GL_LUMINANCE: rgba[0] = rgba[1] = rgba[2] = c[0]; break;
    case GL_GREEN: rgba[1] = c[0]; break;
    case GL_BLUE: rgba[2] = c[0]; break;
    case GL_ALPHA: rgba[3] = c[0]; break;
    case GL_LUMINANCE_ALPHA: rgba[0] = rgba[1] = rgba[2] = c[0]; rgba[3] = c[1]; break;
    case GL_RG: case GL_RG_INTEGER: case GL_DEPTH_STENCIL: rgba[0] = c[0]; rgba[1] = c[1]; break;
    case GL_RGB: case GL_RGB_INTEGER: rgba[0] = c[0]; rgba[1] = c[1]; rgba[2] = c[2]; break;
    case GL_BGR: case GL_BGR_INTEGER: rgba[0] = c[2]; rgba[1] = c[1]; rgba[2] = c[0]; break;
    case GL_RGBA: case GL_RGBA_INTEGER: memcpy(rgba, c, 16); break;
    case GL_BGRA: case GL_BGRA_INTEGER: rgba[0] = c[2]; rgba[1] = c[1]; rgba[2] = c[0]; rgba[3] = c[3]; break;
    case GL_ABGR_EXT: rgba[0] = c[3]; rgba[1] = c[2]; rgba[2] = c[1]; rgba[3] = c[0]; break;
    }
}

/* Storage channel sources for luminance/alpha/intensity internal formats,
   derived from the sampling swizzle: channel k is filled from the first RGBA
   component whose swizzle reads it. */
static void select_channels(const struct glm_format_info *info, float *rgba)
{
    float out[4] = {0, 0, 0, 1};
    bool filled[4] = {false};
    for (int component = 0; component < 4; ++component) {
        int channel = info->swizzle[component];
        if (channel < 4 && !filled[channel]) {
            out[channel] = rgba[component];
            filled[channel] = true;
        }
    }
    for (int k = 0; k < 4; ++k)
        if (!filled[k]) out[k] = k == 3 ? 1 : 0;
    memcpy(rgba, out, sizeof out);
}

/* ---- block compression (uncompressed uploads into compressed formats) ---- */

enum { BC1 = 130, BC1_sRGB = 131, BC2 = 132, BC2_sRGB = 133, BC3 = 134, BC3_sRGB = 135, BC4U = 140, BC4S = 141,
       BC5U = 142, BC5S = 143 };

static uint16_t rgb565(const float *c)
{
    return (uint16_t)(to_unorm(c[0], 5) << 11 | to_unorm(c[1], 6) << 5 | to_unorm(c[2], 5));
}
static void from565(uint16_t v, float *c)
{
    c[0] = (float)(v >> 11) / 31.0f;
    c[1] = (float)((v >> 5) & 63) / 63.0f;
    c[2] = (float)(v & 31) / 31.0f;
}

/* BC1 colour block of 16 RGBA texels; `punch` uses the 3-colour mode with
   transparent black for texels with alpha below one half. */
static void encode_bc1(const float *px, uint8_t *out, bool punch)
{
    float lo[3] = {1, 1, 1}, hi[3] = {0, 0, 0}, mean[3] = {0, 0, 0};
    bool transparent = false;
    int n = 0;
    for (int i = 0; i < 16; ++i) {
        if (punch && px[i * 4 + 3] < 0.5f) { transparent = true; continue; }
        for (int c = 0; c < 3; ++c) {
            lo[c] = fminf(lo[c], px[i * 4 + c]);
            hi[c] = fmaxf(hi[c], px[i * 4 + c]);
            mean[c] += px[i * 4 + c];
        }
        ++n;
    }
    if (!n) { lo[0] = lo[1] = lo[2] = hi[0] = hi[1] = hi[2] = 0; }
    else for (int c = 0; c < 3; ++c) mean[c] /= (float)n;
    /* Endpoints along the bounding box diagonal the colours follow. */
    float cov_rg = 0, cov_rb = 0;
    for (int i = 0; i < 16; ++i) {
        if (punch && px[i * 4 + 3] < 0.5f) continue;
        cov_rg += (px[i * 4] - mean[0]) * (px[i * 4 + 1] - mean[1]);
        cov_rb += (px[i * 4] - mean[0]) * (px[i * 4 + 2] - mean[2]);
    }
    float e0[3] = {hi[0], hi[1], hi[2]}, e1[3] = {lo[0], lo[1], lo[2]};
    if (cov_rg < 0) { e0[1] = lo[1]; e1[1] = hi[1]; }
    if (cov_rb < 0) { e0[2] = lo[2]; e1[2] = hi[2]; }
    uint16_t a = rgb565(e0), b = rgb565(e1);
    bool three = transparent;
    if (three ? a > b : a < b) { uint16_t t = a; a = b; b = t; }
    if (!three && a == b) { /* one colour: indices all 0 */
        memcpy(out, &a, 2); memcpy(out + 2, &b, 2); memset(out + 4, 0, 4);
        return;
    }
    float palette[4][3];
    from565(a, palette[0]);
    from565(b, palette[1]);
    for (int c = 0; c < 3; ++c) {
        if (three) {
            palette[2][c] = (palette[0][c] + palette[1][c]) / 2;
            palette[3][c] = 0;
        } else {
            palette[2][c] = (2 * palette[0][c] + palette[1][c]) / 3;
            palette[3][c] = (palette[0][c] + 2 * palette[1][c]) / 3;
        }
    }
    uint32_t indices = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0;
        if (three && punch && px[i * 4 + 3] < 0.5f) best = 3;
        else {
            float best_d = 1e9f;
            for (int k = 0; k < (three ? 3 : 4); ++k) {
                float d = 0;
                for (int c = 0; c < 3; ++c) d += (px[i * 4 + c] - palette[k][c]) * (px[i * 4 + c] - palette[k][c]);
                if (d < best_d) { best_d = d; best = k; }
            }
        }
        indices |= (uint32_t)best << (2 * i);
    }
    memcpy(out, &a, 2);
    memcpy(out + 2, &b, 2);
    memcpy(out + 4, &indices, 4);
}

/* BC4 block of 16 values (channel `c` of RGBA texels), unsigned or signed. */
static void encode_bc4(const float *px, int c, bool is_signed, uint8_t *out)
{
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 16; ++i) { lo = fminf(lo, px[i * 4 + c]); hi = fmaxf(hi, px[i * 4 + c]); }
    int r0, r1;
    if (is_signed) {
        r0 = (int)lrintf(fmaxf(fminf(hi, 1), -1) * 127);
        r1 = (int)lrintf(fmaxf(fminf(lo, 1), -1) * 127);
        out[0] = (uint8_t)(int8_t)r0;
        out[1] = (uint8_t)(int8_t)r1;
    } else {
        r0 = (int)to_unorm(hi, 8);
        r1 = (int)to_unorm(lo, 8);
        out[0] = (uint8_t)r0;
        out[1] = (uint8_t)r1;
    }
    float scale = is_signed ? 127.0f : 255.0f, palette[8];
    palette[0] = (float)r0 / scale;
    palette[1] = (float)r1 / scale;
    for (int k = 1; k < 7; ++k) palette[k + 1] = ((float)(7 - k) * palette[0] + (float)k * palette[1]) / 7;
    uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0;
        float best_d = 1e9f;
        for (int k = 0; k < 8; ++k) {
            float d = fabsf(px[i * 4 + c] - palette[k]);
            if (d < best_d) { best_d = d; best = k; }
        }
        if (r0 == r1) best = 0;
        bits |= (uint64_t)best << (3 * i);
    }
    for (int k = 0; k < 6; ++k) out[2 + k] = (uint8_t)(bits >> (8 * k));
}

static void decode_bc1(const uint8_t *b, float *px, bool alpha_mode)
{
    uint16_t a, c;
    memcpy(&a, b, 2);
    memcpy(&c, b + 2, 2);
    float pal[4][4];
    from565(a, pal[0]);
    from565(c, pal[1]);
    pal[0][3] = pal[1][3] = 1;
    for (int k = 0; k < 3; ++k) {
        if (a > c || !alpha_mode) {
            pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
            pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
        } else {
            pal[2][k] = (pal[0][k] + pal[1][k]) / 2;
            pal[3][k] = 0;
        }
    }
    pal[2][3] = 1;
    pal[3][3] = a > c || !alpha_mode ? 1 : 0;
    uint32_t idx;
    memcpy(&idx, b + 4, 4);
    for (int i = 0; i < 16; ++i) memcpy(px + i * 4, pal[(idx >> (2 * i)) & 3], 16);
}

static void decode_bc4(const uint8_t *b, float *px, int channel, bool is_signed)
{
    float pal[8];
    pal[0] = is_signed ? fmaxf((float)(int8_t)b[0] / 127.0f, -1) : b[0] / 255.0f;
    pal[1] = is_signed ? fmaxf((float)(int8_t)b[1] / 127.0f, -1) : b[1] / 255.0f;
    bool eight = is_signed ? (int8_t)b[0] > (int8_t)b[1] : b[0] > b[1];
    if (eight)
        for (int k = 1; k < 7; ++k) pal[k + 1] = ((float)(7 - k) * pal[0] + (float)k * pal[1]) / 7;
    else {
        for (int k = 1; k < 5; ++k) pal[k + 1] = ((float)(5 - k) * pal[0] + (float)k * pal[1]) / 5;
        pal[6] = is_signed ? -1 : 0;
        pal[7] = 1;
    }
    uint64_t bits = 0;
    for (int k = 0; k < 6; ++k) bits |= (uint64_t)b[2 + k] << (8 * k);
    for (int i = 0; i < 16; ++i) px[i * 4 + channel] = pal[(bits >> (3 * i)) & 7];
}

/* Decodes BC1-BC5 blocks (`row_bytes` per block row) into RGBA floats;
   false for other formats. */
GLM_HIDDEN bool glm_decode_blocks(uint32_t f, const uint8_t *src, size_t row_bytes, size_t width, size_t height,
                                  float *rgba)
{
    if (f < BC1 || f > BC5S || (f > BC3_sRGB && f < BC4U)) return false;
    size_t size = f == BC1 || f == BC1_sRGB || f == BC4U || f == BC4S ? 8 : 16;
    for (size_t by = 0; by < (height + 3) / 4; ++by)
        for (size_t bx = 0; bx < (width + 3) / 4; ++bx) {
            const uint8_t *b = src + by * row_bytes + bx * size;
            float px[64];
            for (int i = 0; i < 16; ++i) { px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = 0; px[i * 4 + 3] = 1; }
            switch (f) {
            case BC1: case BC1_sRGB: decode_bc1(b, px, true); break;
            case BC2: case BC2_sRGB:
                decode_bc1(b + 8, px, false);
                for (int i = 0; i < 16; ++i) px[i * 4 + 3] = (float)((b[i / 2] >> (4 * (i & 1))) & 15) / 15.0f;
                break;
            case BC3: case BC3_sRGB: decode_bc1(b + 8, px, false); decode_bc4(b, px, 3, false); break;
            case BC4U: case BC4S: decode_bc4(b, px, 0, f == BC4S); break;
            case BC5U: case BC5S: decode_bc4(b, px, 0, f == BC5S); decode_bc4(b + 8, px, 1, f == BC5S); break;
            }
            for (int i = 0; i < 16; ++i) {
                size_t x = bx * 4 + (size_t)(i & 3), y = by * 4 + (size_t)(i >> 2);
                if (x < width && y < height) memcpy(rgba + (y * width + x) * 4, px + i * 4, 16);
            }
        }
    return true;
}

static size_t block_bytes(uint32_t f) { return f == BC1 || f == BC1_sRGB || f == BC4U || f == BC4S ? 8 : 16; }

/* RGBA float texels (already in storage channels) into blocks of `f`. */
static uint8_t *compress_blocks(uint32_t f, bool punch, const float *rgba, GLsizei width, GLsizei height, GLsizei depth,
                                size_t *row_bytes, size_t *image_bytes)
{
    size_t bw = (size_t)(width + 3) / 4, bh = (size_t)(height + 3) / 4, size = block_bytes(f);
    *row_bytes = bw * size;
    *image_bytes = *row_bytes * bh;
    uint8_t *out = calloc(*image_bytes * (size_t)(depth ? depth : 1) + 1, 1);
    if (!out) return NULL;
    for (GLsizei z = 0; z < (depth ? depth : 1); ++z)
        for (size_t by = 0; by < bh; ++by)
            for (size_t bx = 0; bx < bw; ++bx) {
                float px[64];
                for (int i = 0; i < 16; ++i) {
                    /* Texels past the edge repeat the last row / column. */
                    size_t x = MIN(bx * 4 + (size_t)(i & 3), (size_t)width - 1), y = MIN(by * 4 + (size_t)(i >> 2), (size_t)height - 1);
                    memcpy(px + i * 4, rgba + (((size_t)z * (size_t)height + y) * (size_t)width + x) * 4, 16);
                }
                uint8_t *block = out + (size_t)z * *image_bytes + by * *row_bytes + bx * size;
                switch (f) {
                case BC1: case BC1_sRGB: encode_bc1(px, block, punch); break;
                case BC2: case BC2_sRGB:
                    for (int i = 0; i < 16; i += 2)
                        block[i / 2] = (uint8_t)(to_unorm(px[i * 4 + 3], 4) | to_unorm(px[(i + 1) * 4 + 3], 4) << 4);
                    encode_bc1(px, block + 8, false);
                    break;
                case BC3: case BC3_sRGB: encode_bc4(px, 3, false, block); encode_bc1(px, block + 8, false); break;
                case BC4U: case BC4S: encode_bc4(px, 0, f == BC4S, block); break;
                case BC5U: case BC5S: encode_bc4(px, 0, f == BC5S, block); encode_bc4(px, 1, f == BC5S, block + 8); break;
                }
            }
    return out;
}

/* glDrawPixels of GL_DEPTH_COMPONENT, GL_STENCIL_INDEX or GL_DEPTH_STENCIL:
   the client image as depth values (0..1) and / or stencil indices, with
   the unpack state's layout. Either output may be NULL. */
GLM_HIDDEN bool glm_unpack_depth_stencil(struct glm_context *ctx, GLsizei width, GLsizei height, GLenum format, GLenum type,
                                         const void *pixels, float *depth, uint32_t *stencil)
{
    const struct glm_state *s = &ctx->state;
    size_t bytes = pixel_bytes(format, type);
    if (!bytes) return false;
    size_t row_pixels = s->unpack_row_length > 0 ? (size_t)s->unpack_row_length : (size_t)width;
    size_t alignment = (size_t)(s->unpack_alignment > 0 ? s->unpack_alignment : 4);
    size_t source_row = s->unpack_row_bytes > 0 ? (size_t)s->unpack_row_bytes
                                                : (row_pixels * bytes + alignment - 1) / alignment * alignment;
    const uint8_t *base = (const uint8_t *)pixels + (size_t)s->unpack_skip_rows * source_row + (size_t)s->unpack_skip_pixels * bytes;
    for (GLsizei y = 0; y < height; ++y)
        for (GLsizei x = 0; x < width; ++x) {
            uint8_t tmp[32];
            const uint8_t *p = swapped_pixel(base + (size_t)y * source_row + (size_t)x * bytes, bytes, type,
                                             s->unpack_swap_bytes, tmp);
            size_t i = (size_t)y * (size_t)width + (size_t)x;
            float d = 0;
            int64_t st = 0;
            if (format == GL_DEPTH_STENCIL) {
                uint32_t v;
                memcpy(&v, p, 4);
                if (type == GL_UNSIGNED_INT_24_8) {
                    d = (float)(v >> 8) / 16777215.0f;
                    st = v & 0xff;
                } else {
                    memcpy(&d, p, 4);
                    memcpy(&v, p + 4, 4);
                    st = v & 0xff;
                }
            } else if (format == GL_DEPTH_COMPONENT) {
                float c[4] = {0, 0, 0, 1};
                decode_pixel(GL_DEPTH_COMPONENT, type, p, c, 1, false);
                d = c[0];
            } else {
                int64_t c[4] = {0, 0, 0, 1};
                decode_pixel_int(type, p, c, 1);
                if (type == GL_FLOAT) { float f; memcpy(&f, p, 4); c[0] = (int64_t)f; }
                st = c[0];
            }
            d = d * s->depth_scale + s->depth_bias;
            if (depth) depth[i] = d < 0 ? 0 : d > 1 ? 1 : d;
            if (stencil) stencil[i] = glm_transfer_stencil(s, st);
        }
    return true;
}

/* RGBA floats (storage channels) as blocks of the compressed Metal format
   `f`; NULL for formats this does not encode. */
GLM_HIDDEN void *glm_compress_rgba(uint32_t f, bool punch, const float *rgba, GLsizei width, GLsizei height,
                                   size_t *row_bytes, size_t *image_bytes)
{
    if (f < BC1 || f > BC5S || (f > BC3_sRGB && f < BC4U)) return NULL;
    return compress_blocks(f, punch, rgba, width, height, 1, row_bytes, image_bytes);
}

/* ---- pixel transfer (glPixelTransfer, glPixelMap) ------------------------- */

/* A colour or stencil index after GL_INDEX_SHIFT and GL_INDEX_OFFSET. */
static int64_t shifted_index(const struct glm_state *s, int64_t index)
{
    index = s->index_shift >= 0 ? index * ((int64_t)1 << s->index_shift) : index >> -s->index_shift;
    return index + s->index_offset;
}

/* Entry of pixel map `m` for an index (masked to the map's size). */
static float map_index(const struct glm_state *s, int m, int64_t index)
{
    return s->pixel_map[m][(uint64_t)index & (uint64_t)(s->pixel_map_size[m] - 1)];
}

GLM_HIDDEN uint32_t glm_transfer_stencil(const struct glm_state *s, int64_t index)
{
    index = shifted_index(s, index);
    if (s->map_stencil) index = (int64_t)map_index(s, 1, index);
    return (uint32_t)index;
}

/* Colour indices to RGBA through the I_TO_R/G/B/A maps. */
static void index_to_rgba(const struct glm_state *s, int64_t index, float *rgba)
{
    index = shifted_index(s, index);
    for (int c = 0; c < 4; ++c) rgba[c] = map_index(s, 2 + c, index);
}

static bool transfer_identity(const struct glm_state *s)
{
    for (int c = 0; c < 4; ++c)
        if (s->pixel_scale[c] != 1 || s->pixel_bias[c] != 0) return false;
    return !s->map_color;
}

/* RGBA scale and bias, then GL_MAP_COLOR's R_TO_R .. A_TO_A lookups. */
static void transfer_rgba(const struct glm_state *s, float *rgba)
{
    for (int c = 0; c < 4; ++c) {
        float v = rgba[c] * s->pixel_scale[c] + s->pixel_bias[c];
        if (s->map_color) {
            int m = 6 + c, size = s->pixel_map_size[m];
            float clamped = v < 0 ? 0 : v > 1 ? 1 : v;
            v = s->pixel_map[m][(int)lrintf(clamped * (float)(size - 1))];
        }
        rgba[c] = v;
    }
}

/* One index of GL_BITMAP data (a bit per pixel, unpack state applied). */
static int bitmap_bit(const struct glm_state *s, const uint8_t *pixels, GLsizei width, GLint x, GLint y)
{
    size_t row_pixels = s->unpack_row_length > 0 ? (size_t)s->unpack_row_length : (size_t)width;
    size_t alignment = (size_t)(s->unpack_alignment > 0 ? s->unpack_alignment : 4);
    size_t row = ((row_pixels + 7) / 8 + alignment - 1) / alignment * alignment;
    size_t bit = (size_t)s->unpack_skip_pixels + (size_t)x;
    uint8_t byte = pixels[((size_t)s->unpack_skip_rows + (size_t)y) * row + bit / 8];
    return (byte >> (s->unpack_lsb_first ? bit % 8 : 7 - bit % 8)) & 1;
}

void *glm_convert_upload(struct glm_context *ctx, const struct glm_format_info *info, GLsizei width, GLsizei height,
                         GLsizei depth, GLenum format, GLenum type, const void *pixels, size_t *row_bytes,
                         size_t *image_bytes)
{
    const struct glm_state *s = &ctx->state;
    if (type == GL_BITMAP && format == GL_COLOR_INDEX && !info->compressed && !info->integer) {
        /* One bit per pixel: indices 0 and 1 through the colour maps. */
        size_t texel = glm_metal_format_bytes(info->metal_format);
        *row_bytes = texel * (size_t)width;
        *image_bytes = *row_bytes * (size_t)height;
        uint8_t *out = malloc(*image_bytes * (size_t)(depth ? depth : 1) + 1);
        float *row = malloc((size_t)width * 4 * sizeof(float));
        if (!out || !row) { free(out); free(row); return NULL; }
        for (GLsizei z = 0; z < (depth ? depth : 1); ++z)
            for (GLsizei y = 0; y < height; ++y) {
                for (GLsizei x = 0; x < width; ++x) {
                    index_to_rgba(s, bitmap_bit(s, pixels, width, x, y), row + x * 4);
                    select_channels(info, row + x * 4);
                }
                pack_texels(info->metal_format, row, (size_t)width, out + (size_t)z * *image_bytes + (size_t)y * *row_bytes);
            }
        free(row);
        return out;
    }
    size_t bytes = pixel_bytes(format, type);
    if (bytes && info->compressed && block_bytes(info->metal_format) &&
        (info->metal_format >= BC1 && info->metal_format <= BC5S)) {
        /* Uncompressed data for a compressed format: compressed here, as
           Apple's implementation does. */
        struct glm_format_info plain = *info;
        plain.metal_format = RGBA32Float;
        plain.compressed = false;
        size_t plain_row, plain_image;
        float *rgba = glm_convert_upload(ctx, &plain, width, height, depth, format, type, pixels, &plain_row, &plain_image);
        if (!rgba || !width || !height) { free(rgba); return NULL; }
        bool punch = (info->metal_format == BC1 || info->metal_format == BC1_sRGB) && info->swizzle[3] == 3;
        uint8_t *out = compress_blocks(info->metal_format, punch, rgba, width, height, depth, row_bytes, image_bytes);
        free(rgba);
        return out;
    }
    if (!bytes || info->compressed) return NULL;
    int components = format_components(format);
    size_t row_pixels = s->unpack_row_length > 0 ? (size_t)s->unpack_row_length : (size_t)width;
    size_t alignment = (size_t)(s->unpack_alignment > 0 ? s->unpack_alignment : 4);
    size_t source_row = s->unpack_row_bytes > 0 ? (size_t)s->unpack_row_bytes
                                                : (row_pixels * bytes + alignment - 1) / alignment * alignment;
    size_t image_rows = s->unpack_image_height > 0 ? (size_t)s->unpack_image_height : (size_t)height;
    size_t source_image = s->unpack_image_bytes > 0 ? (size_t)s->unpack_image_bytes : source_row * image_rows;
    const uint8_t *base = (const uint8_t *)pixels + (size_t)s->unpack_skip_images * source_image +
                          (size_t)s->unpack_skip_rows * source_row + (size_t)s->unpack_skip_pixels * bytes;
    size_t texel = glm_metal_format_bytes(info->metal_format);
    *row_bytes = texel * (size_t)width;
    *image_bytes = *row_bytes * (size_t)height;
    uint8_t *out = malloc(*image_bytes * (size_t)(depth ? depth : 1) + 1);
    /* 8-bit RGBA / BGRA into RGBA8 storage: copied or byte-swizzled, not
       decoded through floats (the common case for games' uploads). */
    bool rgba8_storage = (info->metal_format == RGBA8Unorm || info->metal_format == RGBA8Unorm_sRGB) &&
                         info->swizzle[0] == 0 && info->swizzle[1] == 1 && info->swizzle[2] == 2 && info->swizzle[3] == 3;
    bool bytes8 = type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_INT_8_8_8_8_REV;
    static int no_fast = -1;
    if (no_fast < 0) no_fast = getenv("GLMETAL_NO_FAST_UPLOAD") != NULL;
    /* Video planes and other byte textures already match R8/RG8 storage.
       Keep the unpack strides/skips, but avoid decoding every byte through
       float RGBA and packing it back to the identical normalized value. */
    bool r8_storage = info->metal_format == R8Unorm || info->metal_format == R8Unorm_sRGB;
    bool rg8_storage = info->metal_format == RG8Unorm || info->metal_format == RG8Unorm_sRGB;
    bool raw_narrow = type == GL_UNSIGNED_BYTE &&
        ((r8_storage &&
          (((format == GL_RED || format == GL_LUMINANCE) && info->swizzle[0] == 0) ||
           (format == GL_ALPHA && info->swizzle[0] != 0 && info->swizzle[1] != 0 &&
            info->swizzle[2] != 0 && info->swizzle[3] == 0))) ||
         (rg8_storage &&
          ((format == GL_RG && info->swizzle[0] == 0 && info->swizzle[1] == 1) ||
           (format == GL_LUMINANCE_ALPHA && info->swizzle[0] == 0 && info->swizzle[1] != 1 &&
            info->swizzle[2] != 1 && info->swizzle[3] == 1))));
    if (out && !no_fast && raw_narrow && transfer_identity(s)) {
        for (GLsizei z = 0; z < (depth ? depth : 1); ++z)
            for (GLsizei y = 0; y < height; ++y)
                memcpy(out + (size_t)z * *image_bytes + (size_t)y * *row_bytes,
                       base + (size_t)z * source_image + (size_t)y * source_row, *row_bytes);
        return out;
    }
    if (out && !no_fast && rgba8_storage && bytes8 && !s->unpack_swap_bytes && (format == GL_RGBA || format == GL_BGRA) &&
        transfer_identity(s)) {
        for (GLsizei z = 0; z < (depth ? depth : 1); ++z)
            for (GLsizei y = 0; y < height; ++y) {
                const uint8_t *src = base + (size_t)z * source_image + (size_t)y * source_row;
                uint8_t *dst = out + (size_t)z * *image_bytes + (size_t)y * *row_bytes;
                if (format == GL_RGBA) {
                    memcpy(dst, src, (size_t)width * 4);
                } else {
                    for (GLsizei x = 0; x < width; ++x) {
                        uint32_t v;
                        memcpy(&v, src + (size_t)x * 4, 4);
                        v = (v & 0xff00ff00u) | (v >> 16 & 0xffu) | (v & 0xffu) << 16;
                        memcpy(dst + (size_t)x * 4, &v, 4);
                    }
                }
            }
        return out;
    }
    uint32_t mf = info->metal_format;
    bool narrow = mf == R8Unorm || mf == R8Unorm_sRGB || mf == RG8Unorm || mf == RG8Unorm_sRGB || mf == RGBA8Unorm ||
                  mf == RGBA8Unorm_sRGB || mf == BGRA8Unorm || mf == BGRA8Unorm_sRGB || mf == B5G6R5Unorm ||
                  mf == A1BGR5Unorm || mf == ABGR4Unorm || mf == BGR5A1Unorm;
    float *row = malloc((size_t)width * 4 * sizeof(float));
    if (!out || !row) {
        free(out);
        free(row);
        return NULL;
    }
    /* Packed float client data in the same packing: copied bit for bit. */
    bool raw32 = (mf == RG11B10Float && type == GL_UNSIGNED_INT_10F_11F_11F_REV && format == GL_RGB) ||
                 (mf == RGB9E5Float && type == GL_UNSIGNED_INT_5_9_9_9_REV && format == GL_RGB);
    /* Integer formats: through 64-bit integers (floats lose 32-bit values). */
    bool integer_path = info->integer && is_integer_format(format);
    for (GLsizei z = 0; z < (depth ? depth : 1); ++z)
        for (GLsizei y = 0; y < height; ++y) {
            const uint8_t *src = base + (size_t)z * source_image + (size_t)y * source_row;
            uint8_t *dst = out + (size_t)z * *image_bytes + (size_t)y * *row_bytes;
            if (raw32 && !s->unpack_swap_bytes) {
                memcpy(dst, src, (size_t)width * 4);
                continue;
            }
            if (integer_path) {
                for (GLsizei x = 0; x < width; ++x) {
                    int64_t c[4] = {0, 0, 0, 1}, v[4];
                    uint8_t tmp[32];
                    decode_pixel_int(type, swapped_pixel(src + (size_t)x * bytes, bytes, type, s->unpack_swap_bytes, tmp), c,
                                     components);
                    int_to_rgba(format, c, v);
                    pack_texel_int(mf, v, dst + (size_t)x * texel);
                }
                continue;
            }
            for (GLsizei x = 0; x < width; ++x) {
                float c[4] = {0, 0, 0, 1};
                uint8_t tmp[32];
                const uint8_t *pixel = swapped_pixel(src + (size_t)x * bytes, bytes, type, s->unpack_swap_bytes, tmp);
                if (format == GL_COLOR_INDEX) {
                    int64_t index[4] = {0, 0, 0, 0};
                    if (type == GL_FLOAT) { float f; memcpy(&f, pixel, 4); index[0] = (int64_t)f; }
                    else decode_pixel_int(type, pixel, index, 1);
                    index_to_rgba(s, index[0], row + x * 4);
                    select_channels(info, row + x * 4);
                    continue;
                }
                decode_pixel(format, type, pixel, c, components, narrow && transfer_identity(s));
                to_rgba(format, c, row + x * 4);
                if (!transfer_identity(s)) transfer_rgba(s, row + x * 4);
                if (format == GL_LUMINANCE_ALPHA && info->swizzle[0] == 0 && info->swizzle[3] == 3) {
                    /* luminance-alpha data into an RGBA texture: L L L A */
                    row[x * 4 + 1] = row[x * 4 + 2] = row[x * 4];
                } else if (format == GL_LUMINANCE && info->swizzle[1] == 1) {
                    row[x * 4 + 1] = row[x * 4 + 2] = row[x * 4];
                }
                if (!info->depth && !info->stencil) select_channels(info, row + x * 4); /* depth, stencil as they are */
            }
            pack_texels(info->metal_format, row, (size_t)width, dst);
        }
    free(row);
    return out;
}

/* ---- pack (read back to client memory) ----------------------------------- */

/* Depth (rgba[0], 0..1) and stencil (rgba[1], an integer) values. Stencil
   indices pack as integers; depth as normalized values. */
static void encode_depth_stencil(GLenum format, GLenum type, const float *rgba, uint8_t *p)
{
    double d = fmin(fmax(rgba[0], 0.0), 1.0);
    uint32_t stencil = (uint32_t)rgba[1];
    if (format == GL_DEPTH_STENCIL) {
        if (type == GL_UNSIGNED_INT_24_8) {
            uint32_t v = (uint32_t)llround(d * 16777215.0) << 8 | (stencil & 0xffu);
            memcpy(p, &v, 4);
        } else if (type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV) {
            float f = rgba[0];
            memcpy(p, &f, 4);
            memcpy(p + 4, &stencil, 4);
        }
        return;
    }
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: p[0] = (uint8_t)stencil; return;
    case GL_UNSIGNED_SHORT: case GL_SHORT: { uint16_t s = (uint16_t)stencil; memcpy(p, &s, 2); return; }
    case GL_UNSIGNED_INT: case GL_INT: memcpy(p, &stencil, 4); return;
    case GL_FLOAT: { float f = (float)stencil; memcpy(p, &f, 4); return; }
    case GL_HALF_FLOAT: { uint16_t h = float_to_half((float)stencil); memcpy(p, &h, 2); return; }
    }
}

static void encode_pixel(GLenum format, GLenum type, const float *rgba, uint8_t *p)
{
    if (format == GL_STENCIL_INDEX || format == GL_DEPTH_STENCIL) return encode_depth_stencil(format, type, rgba, p);
    float c[4];
    int n = format_components(format);
    switch (format) {
    case GL_RED: case GL_RED_INTEGER: case GL_DEPTH_COMPONENT: c[0] = rgba[0]; break;
    case GL_GREEN: case GL_GREEN_INTEGER: c[0] = rgba[1]; break;
    case GL_BLUE: case GL_BLUE_INTEGER: c[0] = rgba[2]; break;
    case GL_ALPHA: c[0] = rgba[3]; break;
    case GL_LUMINANCE: c[0] = fminf(rgba[0] + rgba[1] + rgba[2], 1.0f); break;
    case GL_LUMINANCE_ALPHA: c[0] = fminf(rgba[0] + rgba[1] + rgba[2], 1.0f); c[1] = rgba[3]; break;
    case GL_RG: case GL_RG_INTEGER: c[0] = rgba[0]; c[1] = rgba[1]; break;
    case GL_RGB: case GL_RGB_INTEGER: c[0] = rgba[0]; c[1] = rgba[1]; c[2] = rgba[2]; break;
    case GL_BGR: case GL_BGR_INTEGER: c[0] = rgba[2]; c[1] = rgba[1]; c[2] = rgba[0]; break;
    case GL_BGRA: case GL_BGRA_INTEGER: c[0] = rgba[2]; c[1] = rgba[1]; c[2] = rgba[0]; c[3] = rgba[3]; break;
    case GL_ABGR_EXT: c[0] = rgba[3]; c[1] = rgba[2]; c[2] = rgba[1]; c[3] = rgba[0]; break;
    default: memcpy(c, rgba, 16); break;
    }
    bool integer = is_integer_format(format);
    switch (type) {
    case GL_UNSIGNED_BYTE: for (int i = 0; i < n; ++i) p[i] = integer ? (uint8_t)c[i] : (uint8_t)to_unorm(c[i], 8); return;
    case GL_BYTE: for (int i = 0; i < n; ++i) p[i] = (uint8_t)(integer ? (int8_t)c[i] : (int8_t)to_snorm(c[i], 8)); return;
    case GL_UNSIGNED_SHORT: for (int i = 0; i < n; ++i) { uint16_t s = integer ? (uint16_t)c[i] : (uint16_t)to_unorm(c[i], 16); memcpy(p + i * 2, &s, 2); } return;
    case GL_SHORT: for (int i = 0; i < n; ++i) { int16_t s = integer ? (int16_t)c[i] : (int16_t)to_snorm(c[i], 16); memcpy(p + i * 2, &s, 2); } return;
    case GL_UNSIGNED_INT: for (int i = 0; i < n; ++i) { uint32_t s = integer ? (uint32_t)c[i] : (uint32_t)llround(fmin(fmax(c[i], 0.0), 1.0) * 4294967295.0); memcpy(p + i * 4, &s, 4); } return;
    case GL_INT: for (int i = 0; i < n; ++i) { int32_t s = integer ? (int32_t)c[i] : (int32_t)lround(fmin(fmax(c[i], -1.0), 1.0) * 2147483647.0); memcpy(p + i * 4, &s, 4); } return;
    case GL_FLOAT: memcpy(p, c, (size_t)n * 4); return;
    case GL_HALF_FLOAT: for (int i = 0; i < n; ++i) { uint16_t s = float_to_half(c[i]); memcpy(p + i * 2, &s, 2); } return;
    }
    uint32_t v;
    uint16_t s;
    switch (type) {
    case GL_UNSIGNED_INT_8_8_8_8:
        v = to_unorm(c[0], 8) << 24 | to_unorm(c[1], 8) << 16 | to_unorm(c[2], 8) << 8 | to_unorm(c[3], 8);
        memcpy(p, &v, 4); return;
    case GL_UNSIGNED_INT_8_8_8_8_REV:
        v = to_unorm(c[0], 8) | to_unorm(c[1], 8) << 8 | to_unorm(c[2], 8) << 16 | to_unorm(c[3], 8) << 24;
        memcpy(p, &v, 4); return;
    case GL_UNSIGNED_INT_2_10_10_10_REV:
        v = to_unorm(c[0], 10) | to_unorm(c[1], 10) << 10 | to_unorm(c[2], 10) << 20 | to_unorm(c[3], 2) << 30;
        memcpy(p, &v, 4); return;
    case GL_UNSIGNED_INT_10_10_10_2:
        v = to_unorm(c[0], 10) << 22 | to_unorm(c[1], 10) << 12 | to_unorm(c[2], 10) << 2 | to_unorm(c[3], 2);
        memcpy(p, &v, 4); return;
    case GL_UNSIGNED_SHORT_5_6_5:
        s = (uint16_t)(to_unorm(c[0], 5) << 11 | to_unorm(c[1], 6) << 5 | to_unorm(c[2], 5));
        memcpy(p, &s, 2); return;
    case GL_UNSIGNED_SHORT_4_4_4_4:
        s = (uint16_t)(to_unorm(c[0], 4) << 12 | to_unorm(c[1], 4) << 8 | to_unorm(c[2], 4) << 4 | to_unorm(c[3], 4));
        memcpy(p, &s, 2); return;
    case GL_UNSIGNED_SHORT_1_5_5_5_REV:
        s = (uint16_t)(to_unorm(c[0], 5) | to_unorm(c[1], 5) << 5 | to_unorm(c[2], 5) << 10 | to_unorm(c[3], 1) << 15);
        memcpy(p, &s, 2); return;
    case GL_UNSIGNED_INT_10F_11F_11F_REV:
        v = to_small_float(c[0], 6) | to_small_float(c[1], 6) << 11 | to_small_float(c[2], 5) << 22;
        memcpy(p, &v, 4); return;
    case GL_UNSIGNED_INT_5_9_9_9_REV: v = to_rgb9e5(c); memcpy(p, &v, 4); return;
    case GL_UNSIGNED_BYTE_3_3_2: p[0] = (uint8_t)(to_unorm(c[0], 3) << 5 | to_unorm(c[1], 3) << 2 | to_unorm(c[2], 2)); return;
    case GL_UNSIGNED_BYTE_2_3_3_REV: p[0] = (uint8_t)(to_unorm(c[0], 3) | to_unorm(c[1], 3) << 3 | to_unorm(c[2], 2) << 6); return;
    case GL_UNSIGNED_SHORT_5_6_5_REV:
        s = (uint16_t)(to_unorm(c[0], 5) | to_unorm(c[1], 6) << 5 | to_unorm(c[2], 5) << 11);
        memcpy(p, &s, 2); return;
    case GL_UNSIGNED_SHORT_4_4_4_4_REV:
        s = (uint16_t)(to_unorm(c[0], 4) | to_unorm(c[1], 4) << 4 | to_unorm(c[2], 4) << 8 | to_unorm(c[3], 4) << 12);
        memcpy(p, &s, 2); return;
    case GL_UNSIGNED_SHORT_5_5_5_1:
        s = (uint16_t)(to_unorm(c[0], 5) << 11 | to_unorm(c[1], 5) << 6 | to_unorm(c[2], 5) << 1 | to_unorm(c[3], 1));
        memcpy(p, &s, 2); return;
    }
}

bool glm_convert_pack(struct glm_context *ctx, const float *rgba, GLsizei width, GLsizei height, GLenum format,
                      GLenum type, void *out)
{
    const struct glm_state *s = &ctx->state;
    size_t bytes = pixel_bytes(format, type);
    if (!bytes) return false;
    size_t row_pixels = s->pack_row_length > 0 ? (size_t)s->pack_row_length : (size_t)width;
    size_t alignment = (size_t)(s->pack_alignment > 0 ? s->pack_alignment : 4);
    size_t row = s->pack_row_bytes > 0 ? (size_t)s->pack_row_bytes : (row_pixels * bytes + alignment - 1) / alignment * alignment;
    uint8_t *base = (uint8_t *)out + (size_t)s->pack_skip_rows * row + (size_t)s->pack_skip_pixels * bytes;
    for (GLsizei y = 0; y < height; ++y)
        for (GLsizei x = 0; x < width; ++x) {
            uint8_t *p = base + (size_t)y * row + (size_t)x * bytes;
            encode_pixel(format, type, rgba + ((size_t)y * width + x) * 4, p);
            if (s->pack_swap_bytes) swap_output_pixel(p, bytes, type);
        }
    return true;
}

GLM_EXPORT void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels)
{
    GLM_CONTEXT(ctx);
    if (width < 0 || height < 0) return glm_error(ctx, GL_INVALID_VALUE);
    bool depth, stencil;
    /* Format and type first, then the framebuffer, then what it holds. */
    bool core = ctx->profile == GLM_PROFILE_CORE;
    GLenum type_error = glm_read_error((GLenum)-1, false, false, format, type, core, true);
    if (type_error) return glm_error(ctx, type_error);
    if (glm_framebuffer_status_of(ctx, ctx->read_framebuffer) != GL_FRAMEBUFFER_COMPLETE)
        return glm_error(ctx, GL_INVALID_FRAMEBUFFER_OPERATION);
    GLenum source = glm_read_buffer_format(ctx, &depth, &stencil);
    GLenum error = glm_read_error(source, depth, stencil, format, type, ctx->profile == GLM_PROFILE_CORE, true);
    if (error) return glm_error(ctx, error);
    if (!pixel_bytes(format, type)) return glm_error(ctx, GL_INVALID_ENUM);
    if (!width || !height) return;
    uint8_t *destination = pixels;
    if (ctx->pixel_pack_buffer) {
        struct glm_buffer *buffer = glm_buffer_get(ctx, ctx->pixel_pack_buffer);
        uint8_t *contents = buffer ? glm_backend_buffer_contents(ctx, buffer, true) : NULL;
        if (!contents) return glm_error(ctx, GL_INVALID_OPERATION);
        destination = contents + (uintptr_t)pixels;
    }
    glm_backend_read_pixels(ctx, x, y, width, height, format, type, destination);
}
