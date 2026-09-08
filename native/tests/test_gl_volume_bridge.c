#include "gl_volume_bridge.h"
#include "gl_misc_bridge.h"
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static unsigned char *guest;

static void check_volume(GLenum format, int width, int height, int depth, int pbo)
{
    size_t block_size = format <= GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ? 8 : 16;
    size_t slice_size = (size_t)((width + 3) / 4) * ((height + 3) / 4) * block_size;
    size_t byte_count = slice_size * depth, pixels = (size_t)width * height * depth * 4;
    unsigned char *reference = malloc(pixels), *actual = malloc(pixels);
    assert(reference && actual && byte_count < 65536);
    uint32_t seed = 0x19bc027d;
    for (size_t i = 0; i < byte_count; ++i) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        guest[i] = seed >> 24;
    }
    GLuint texture2d, texture3d, buffer = 0;
    glGenTextures(1, &texture2d); glBindTexture(GL_TEXTURE_2D, texture2d);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    /* Independent reference: have Apple's supported 2D S3TC path decode
       each animation frame. Compare all channels, including DXT alpha. */
    for (int z = 0; z < depth; ++z) {
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, slice_size, guest + z * slice_size);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, reference + (size_t)z * width * height * 4);
    }
    assert(glGetError() == GL_NO_ERROR);
    glGenTextures(1, &texture3d); glBindTexture(GL_TEXTURE_3D, texture3d);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    uint32_t arguments[] = {GL_TEXTURE_3D, 0, format, width, height, depth, 0, byte_count, (uint32_t)(uintptr_t)guest};
    if (pbo) {
        glGenBuffers(1, &buffer); glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, byte_count + 16, NULL, GL_STATIC_DRAW);
        glBufferSubData(GL_PIXEL_UNPACK_BUFFER, 16, byte_count, guest);
        arguments[8] = 16;
    }
    const GLenum state[] = {GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_IMAGE_HEIGHT,
        GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_IMAGES, GL_UNPACK_SWAP_BYTES,
        GL_UNPACK_CLIENT_STORAGE_APPLE};
    const GLint values[] = {8, 29, 31, 2, 3, 1, 1, 1};
    for (unsigned i = 0; i < 8; ++i) glPixelStorei(state[i], values[i]);
    uint64_t result = 0;
    assert(gl_misc_bridge32_dispatch(pbo ? "glCompressedTexImage3DARB" : "_glCompressedTexImage3D", arguments, &result));
    assert(glGetError() == GL_NO_ERROR);
    for (unsigned i = 0; i < 8; ++i) {
        GLint value = 0; glGetIntegerv(state[i], &value); assert(value == values[i]);
        glPixelStorei(state[i], i == 0 ? 4 : 0);
    }
    GLint bound = 0; glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &bound); assert(bound == (GLint)buffer);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glGetTexImage(GL_TEXTURE_3D, 0, GL_RGBA, GL_UNSIGNED_BYTE, actual);
    assert(glGetError() == GL_NO_ERROR);
    for (size_t i = 0; i < pixels; ++i) {
        /* Different legal endpoint/interpolation rounding can differ by 1. */
        if (abs((int)actual[i] - reference[i]) > 2) {
            fprintf(stderr, "volume format=%04x size=%dx%dx%d byte=%zu actual=%u reference=%u\n", format, width, height, depth, i, actual[i], reference[i]);
            abort();
        }
    }
    /* A subsequent mip upload must use the same fallback and preserve level 0. */
    arguments[1] = 1; arguments[3] = arguments[4] = arguments[5] = 1;
    arguments[7] = block_size; arguments[8] = (uint32_t)(uintptr_t)guest;
    assert(gl_volume_bridge32_upload(arguments));
    GLint stored = 0; glGetTexLevelParameteriv(GL_TEXTURE_3D, 1, GL_TEXTURE_WIDTH, &stored); assert(stored == 1);
    glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_DEPTH, &stored); assert(stored == depth);
    assert(glGetError() == GL_NO_ERROR);
    glDeleteTextures(1, &texture2d); glDeleteTextures(1, &texture3d);
    if (buffer) glDeleteBuffers(1, &buffer);
    free(reference); free(actual);
}

int main(void)
{
    guest = mmap((void *)0x20000000, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(guest != MAP_FAILED && (uintptr_t)guest + 65536 <= UINT32_MAX);
    CGLPixelFormatAttribute attributes[] = {kCGLPFAAccelerated, 0};
    CGLPixelFormatObj format = NULL; CGLContextObj context = NULL; GLint count = 0;
    assert(CGLChoosePixelFormat(attributes, &format, &count) == kCGLNoError);
    assert(CGLCreateContext(format, NULL, &context) == kCGLNoError);
    CGLDestroyPixelFormat(format); assert(CGLSetCurrentContext(context) == kCGLNoError);
    uint32_t arguments[] = {GL_TEXTURE_3D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 4, 4, 1, 0, 16, (uint32_t)(uintptr_t)guest};
    assert(!gl_volume_bridge32_upload(arguments)); // Opt-in is TFU-specific.
    gl_volume_bridge32_enable_tfu_compat();
    arguments[7] = 1; assert(!gl_volume_bridge32_upload(arguments)); // Truncated input.
    for (GLenum f = GL_COMPRESSED_RGB_S3TC_DXT1_EXT; f <= GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; ++f) {
        check_volume(f, 8, 8, 8, 0);
        check_volume(f, 3, 5, 3, 1);
        check_volume(f, 1, 1, 1, 0);
    }
    CGLSetCurrentContext(NULL); CGLDestroyContext(context); munmap(guest, 65536);
    puts("TFU volume textures: PASS (DXT1/3/5 vs native 2D decode, alpha, NPOT, depth slices, mip uploads, PBO and unpack state)");
    return 0;
}
