#include "gl_misc_bridge.h"
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    CGLPixelFormatAttribute attributes[] = {kCGLPFAAccelerated, 0};
    CGLPixelFormatObj format; CGLContextObj context; GLint count;
    assert(CGLChoosePixelFormat(attributes, &format, &count) == kCGLNoError);
    assert(CGLCreateContext(format, NULL, &context) == kCGLNoError);
    CGLDestroyPixelFormat(format);
    assert(CGLSetCurrentContext(context) == kCGLNoError);
    char directory[] = "/tmp/tfu-texture-provenance-XXXXXX";
    assert(mkdtemp(directory));
    assert(!setenv("LP32_TRACE_TFU_TEXTURE_UPLOADS", directory, 1));
    unsigned char initial[16384], updated[16384], packed[16384], actual[16384];
    for (unsigned i = 0; i < sizeof(initial); ++i) {
        initial[i] = i * 13 + i / 256;
        updated[i] = 255 - initial[i];
    }
    for (unsigned i = 0; i < sizeof(updated); i += 4) {
        packed[i] = updated[i + 2]; packed[i + 1] = updated[i + 1];
        packed[i + 2] = updated[i]; packed[i + 3] = updated[i + 3];
    }
    GLuint texture, pack, unpack;
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, initial);
    glGenBuffers(1, &pack); glBindBuffer(GL_PIXEL_PACK_BUFFER, pack);
    glBufferData(GL_PIXEL_PACK_BUFFER, 16, NULL, GL_STATIC_DRAW);
    glGenBuffers(1, &unpack); glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, sizeof(updated), updated, GL_STATIC_DRAW);
    const GLenum parameters[] = {GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH,
        GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS, GL_PACK_IMAGE_HEIGHT,
        GL_PACK_SKIP_IMAGES, GL_PACK_SWAP_BYTES, GL_PACK_LSB_FIRST};
    const GLint values[] = {8, 79, 3, 7, 83, 2, 1, 1};
    for (unsigned i = 0; i < 8; ++i) glPixelStorei(parameters[i], values[i]);
    gl_misc_bridge32_trace_texture_upload("image", GL_TEXTURE_2D, 0, 0);
    uint32_t args[] = {GL_TEXTURE_2D, 0, 0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, 0};
    uint64_t result;
    assert(gl_misc_bridge32_dispatch("_glTexSubImage2D", args, &result));
    gl_misc_bridge32_enable_tfu_compat();
    args[6] = GL_BGRA; args[7] = GL_UNSIGNED_INT_8_8_8_8_REV;
    assert(gl_misc_bridge32_dispatch("_glTexSubImage2D", args, &result));
    glBufferData(GL_PIXEL_UNPACK_BUFFER, sizeof(updated) + 32, NULL, GL_STATIC_DRAW);
    glBufferSubData(GL_PIXEL_UNPACK_BUFFER, 16, sizeof(updated), updated);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 64);
    args[8] = 16;
    assert(gl_misc_bridge32_dispatch("_glTexSubImage2D", args, &result));
    assert(glGetError() == GL_NO_ERROR);
    for (unsigned i = 0; i < 8; ++i) {
        GLint value; glGetIntegerv(parameters[i], &value); assert(value == values[i]);
    }
    GLint bound;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &bound); assert(bound == (GLint)pack);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &bound); assert(bound == (GLint)unpack);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &bound); assert(bound == 64);
    for (unsigned i = 0; i < 4; ++i) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%d-%u-tex%u-%s.rgba", directory,
            getpid(), i + 1, texture, i ? "subimage" : "image");
        FILE *file = fopen(path, "rb"); assert(file);
        assert(fread(actual, 1, sizeof(actual), file) == sizeof(actual));
        assert(fgetc(file) == EOF); fclose(file);
        assert(!memcmp(actual, i >= 2 ? packed : i ? updated : initial, sizeof(actual)));
        assert(!unlink(path));
        snprintf(path, sizeof(path), "%s/%d-%u-tex%u-%s.pbo", directory,
            getpid(), i + 1, texture, i ? "subimage" : "image");
        file = fopen(path, "rb"); assert(file);
        assert(fread(actual, 1, sizeof(actual), file) == sizeof(actual)); fclose(file);
        assert(!memcmp(actual, updated, sizeof(actual))); assert(!unlink(path));
    }
    unsetenv("LP32_TRACE_TFU_TEXTURE_UPLOADS"); assert(!rmdir(directory));
    args[8] = 17; // In-bounds but misaligned packed PBO offset remains invalid.
    assert(gl_misc_bridge32_dispatch("_glTexSubImage2D", args, &result));
    assert(glGetError() == GL_INVALID_OPERATION);
    glDeleteBuffers(1, &pack); glDeleteBuffers(1, &unpack); glDeleteTextures(1, &texture);
    CGLSetCurrentContext(NULL); CGLDestroyContext(context);
    puts("TFU texture provenance: PASS (CPU/PBO uploads, exact pixels, pack/unpack state preserved)");
    return 0;
}
