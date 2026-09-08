#include "gl_volume_bridge.h"
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int tfu_compat;
void gl_volume_bridge32_enable_tfu_compat(void) { tfu_compat = 1; }

/* S3TC is supported for 2D textures on the current Apple driver, but TFU
   also uploads DXT-compressed animation slices as a 3D texture. Expand
   those uploads once to RGBA8; sampling and interpolation stay on the GPU.
   Aspyr supplies ordinary DXT slices in Z order (not NVIDIA VTC tiles).
   Block definitions: OpenGL EXT_texture_compression_s3tc, Appendix.
   https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_compression_s3tc.txt */
static unsigned read16(const unsigned char *p) { return p[0] | p[1] << 8; }
static uint64_t read_bits(const unsigned char *p, unsigned n)
{
    uint64_t bits = 0;
    for (unsigned i = 0; i < n; ++i) bits |= (uint64_t)p[i] << (i * 8);
    return bits;
}

static void decode_block(const unsigned char *block, GLenum format, unsigned char pixels[64])
{
    int dxt1 = format == GL_COMPRESSED_RGB_S3TC_DXT1_EXT ||
               format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
    const unsigned char *color = block + (dxt1 ? 0 : 8);
    unsigned endpoints[2] = {read16(color), read16(color + 2)};
    unsigned char palette[4][4] = {{0}};
    for (unsigned i = 0; i < 2; ++i) {
        unsigned r = endpoints[i] >> 11, g = (endpoints[i] >> 5) & 63, b = endpoints[i] & 31;
        palette[i][0] = (r << 3) | (r >> 2);
        palette[i][1] = (g << 2) | (g >> 4);
        palette[i][2] = (b << 3) | (b >> 2);
        palette[i][3] = 255;
    }
    if (!dxt1 || endpoints[0] > endpoints[1]) {
        for (unsigned c = 0; c < 3; ++c) {
            palette[2][c] = (2 * palette[0][c] + palette[1][c]) / 3;
            palette[3][c] = (palette[0][c] + 2 * palette[1][c]) / 3;
        }
        palette[2][3] = palette[3][3] = 255;
    } else {
        for (unsigned c = 0; c < 3; ++c) palette[2][c] = (palette[0][c] + palette[1][c]) / 2;
        palette[2][3] = 255;
        palette[3][3] = format == GL_COMPRESSED_RGB_S3TC_DXT1_EXT ? 255 : 0;
    }
    unsigned char alpha[8] = {block[0], block[1], 0};
    if (format == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT) {
        if (alpha[0] > alpha[1]) {
            for (unsigned i = 1; i <= 6; ++i) alpha[i + 1] = ((7 - i) * alpha[0] + i * alpha[1]) / 7;
        } else {
            for (unsigned i = 1; i <= 4; ++i) alpha[i + 1] = ((5 - i) * alpha[0] + i * alpha[1]) / 5;
            alpha[6] = 0; alpha[7] = 255;
        }
    }
    uint64_t indices = read_bits(color + 4, 4);
    uint64_t alpha_bits = read_bits(block + (format == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT ? 2 : 0),
                                    format == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT ? 6 : 8);
    for (unsigned i = 0; i < 16; ++i) {
        memcpy(pixels + i * 4, palette[(indices >> (2 * i)) & 3], 4);
        if (format == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT) pixels[i * 4 + 3] = ((alpha_bits >> (4 * i)) & 15) * 17;
        if (format == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT) pixels[i * 4 + 3] = alpha[(alpha_bits >> (3 * i)) & 7];
    }
}

int gl_volume_bridge32_upload(const uint32_t *a)
{
    GLenum format = a[2];
    if (!tfu_compat || a[0] != GL_TEXTURE_3D || format < GL_COMPRESSED_RGB_S3TC_DXT1_EXT ||
        format > GL_COMPRESSED_RGBA_S3TC_DXT5_EXT || getenv("LP32_KEEP_TFU_COMPRESSED_VOLUME")) return 0;
    GLsizei width = a[3], height = a[4], depth = a[5];
    if (width <= 0 || height <= 0 || depth <= 0 || width > 4096 || height > 4096 || depth > 4096 || a[6]) return 0;
    size_t bx = ((size_t)width + 3) / 4, by = ((size_t)height + 3) / 4;
    size_t block_size = format <= GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ? 8 : 16;
    size_t encoded_size = bx * by * depth * block_size;
    size_t decoded_size = (size_t)width * height * depth * 4;
    if (encoded_size != a[7] || decoded_size > 256 * 1024 * 1024) return 0;
    GLint unpack_buffer = 0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
    unsigned char *encoded = NULL, *decoded = NULL;
    const unsigned char *source = (const void *)(uintptr_t)a[8];
    if (unpack_buffer) {
        GLint size = 0; glGetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SIZE, &size);
        if (size < 0 || (size_t)a[8] > (size_t)size || encoded_size > (size_t)size - a[8]) return 0;
        encoded = malloc(encoded_size);
        if (!encoded) return 0;
        glGetBufferSubData(GL_PIXEL_UNPACK_BUFFER, a[8], encoded_size, encoded);
        source = encoded;
    }
    if (source) {
        decoded = malloc(decoded_size);
        if (!decoded) { free(encoded); return 0; }
        for (int z = 0; z < depth; ++z) for (size_t y = 0; y < by; ++y) for (size_t x = 0; x < bx; ++x) {
            size_t index = ((size_t)z * by + y) * bx + x;
            unsigned char pixels[64];
            decode_block(source + index * block_size, format, pixels);
            for (int py = 0; py < 4 && y * 4 + py < (size_t)height; ++py)
                for (int px = 0; px < 4 && x * 4 + px < (size_t)width; ++px)
                    memcpy(decoded + (((size_t)z * height + y * 4 + py) * width + x * 4 + px) * 4,
                           pixels + (py * 4 + px) * 4, 4);
        }
    }
    /* Compressed uploads ignore unpack row/skip state. Our RGBA upload must
       do the same and restore it, including any bound pixel-unpack buffer. */
    const GLenum parameters[] = {GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_IMAGE_HEIGHT,
        GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_IMAGES, GL_UNPACK_SWAP_BYTES,
        GL_UNPACK_CLIENT_STORAGE_APPLE};
    GLint values[8];
    for (unsigned i = 0; i < 8; ++i) { glGetIntegerv(parameters[i], &values[i]); glPixelStorei(parameters[i], i == 0 ? 1 : 0); }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glTexImage3D(GL_TEXTURE_3D, a[1], GL_RGBA8, width, height, depth, 0, GL_RGBA, GL_UNSIGNED_BYTE, decoded);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack_buffer);
    for (unsigned i = 0; i < 8; ++i) glPixelStorei(parameters[i], values[i]);
    if (getenv("LP32_DUMP_GLSL")) fprintf(stderr, "compat32: expanded TFU volume level=%u format=%04x size=%dx%dx%d bytes=%zu\n", a[1], format, width, height, depth, encoded_size);
    free(encoded); free(decoded);
    return 1;
}
