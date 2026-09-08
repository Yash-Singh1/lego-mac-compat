#include "gl_misc_bridge.h"
#include "gl_volume_bridge.h"
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

static int tfu_compat;
void gl_misc_bridge32_enable_tfu_compat(void) { tfu_compat = 1; }

static int upload_tfu_packed_pixels(const uint32_t *a)
{
    /* TFU's packed BGRA unpack-buffer uploads can report success while
       leaving the texture zeroed on this Metal-backed legacy GL path.
       The buffer readback matches the DDS payload; a CPU-source transfer
       restores the exact texture. Keep float/deformation uploads native. */
    if (!tfu_compat || getenv("LP32_KEEP_TFU_PACKED_PBO") ||
        a[0] != GL_TEXTURE_2D || a[6] != GL_BGRA || a[7] != GL_UNSIGNED_INT_8_8_8_8_REV ||
        (int32_t)a[4] <= 0 || (int32_t)a[5] <= 0) return 0;
    GLint buffer, size, mapped, alignment, row, skip_rows, skip_pixels, client_storage;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &buffer);
    if (!buffer) return 0;
    glGetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SIZE, &size);
    glGetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_MAPPED, &mapped);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
    glGetIntegerv(GL_UNPACK_CLIENT_STORAGE_APPLE, &client_storage);
    size_t bytes = (size_t)a[4] * a[5] * 4;
    if (mapped || client_storage || (a[8] & 3) || size < 0 || a[8] > (unsigned)size || bytes > (unsigned)size - a[8] ||
        (row && row != (int32_t)a[4]) || skip_rows || skip_pixels ||
        !alignment || ((size_t)a[4] * 4) % alignment) return 0;
    void *pixels = malloc(bytes);
    if (!pixels) return 0;
    glGetBufferSubData(GL_PIXEL_UNPACK_BUFFER, a[8], bytes, pixels);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glTexSubImage2D(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], pixels);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
    free(pixels);
    return 1;
}

void gl_misc_bridge32_trace_texture_upload(const char *operation, uint32_t target,
                                         int32_t level, uint32_t source)
{
    const char *directory = getenv("LP32_TRACE_TFU_TEXTURE_UPLOADS");
    if (!directory || !*directory || target != GL_TEXTURE_2D || level != 0) return;
    /* Optional error draining is only for isolated diagnostic replays. */
    GLenum upload_error = getenv("LP32_TRACE_TFU_TEXTURE_ERRORS") ? glGetError() : GL_NO_ERROR;
    GLint width, height, format, texture, unpack;
    glGetTexLevelParameteriv(target, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(target, 0, GL_TEXTURE_HEIGHT, &height);
    glGetTexLevelParameteriv(target, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
    if (width != 64 || height != 64 || format != GL_RGBA8) return;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
    GLint mapped = 0, serialized = 0, flushing = 0, mipmap = 0, active;
    if (unpack) {
        glGetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_MAPPED, &mapped);
        glGetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SERIALIZED_MODIFY_APPLE, &serialized);
        glGetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_FLUSHING_UNMAP_APPLE, &flushing);
    }
    glGetTexParameteriv(target, GL_GENERATE_MIPMAP, &mipmap);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    fprintf(stderr, "compat32: texture-upload context texture=%d operation=%s error=%04x active=%x mapped=%d serialized=%d flushing=%d auto-mipmap=%d\n",
        texture, operation, upload_error, active, mapped, serialized, flushing, mipmap);
    const GLenum unpack_names[] = {GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH,
        GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS, GL_UNPACK_IMAGE_HEIGHT,
        GL_UNPACK_SKIP_IMAGES, GL_UNPACK_SWAP_BYTES, GL_UNPACK_CLIENT_STORAGE_APPLE};
    GLint unpack_values[8];
    for (unsigned i = 0; i < 8; ++i) glGetIntegerv(unpack_names[i], &unpack_values[i]);
    fprintf(stderr, "compat32: texture-upload state texture=%d align=%d row=%d skip=%d,%d image-height=%d skip-images=%d swap=%d client=%d\n",
        texture, unpack_values[0], unpack_values[1], unpack_values[2], unpack_values[3],
        unpack_values[4], unpack_values[5], unpack_values[6], unpack_values[7]);
    if (!strcmp(operation, "image") && !source && !unpack) {
        /* NULL allocation has undefined contents and may be lazily backed
           by the driver. Only inspect it after the real upload. */
        fprintf(stderr, "compat32: texture-upload allocation texture=%d size=64x64\n", texture);
        return;
    }
    /* Readback must not inherit a guest pack buffer, row stride or skips.
       Do not alter unpack state or consume errors belonging to the guest. */
    const GLenum names[] = {GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH,
        GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS, GL_PACK_IMAGE_HEIGHT,
        GL_PACK_SKIP_IMAGES, GL_PACK_SWAP_BYTES, GL_PACK_LSB_FIRST};
    GLint saved[8], pack;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    for (unsigned i = 0; i < 8; ++i) {
        glGetIntegerv(names[i], &saved[i]); glPixelStorei(names[i], i ? 0 : 1);
    }
    unsigned char pixels[64 * 64 * 4];
    memset(pixels, 0xa7, sizeof(pixels));
    glGetTexImage(target, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    for (unsigned i = 0; i < 8; ++i) glPixelStorei(names[i], saved[i]);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
    unsigned nonzero = 0, changed = 0;
    for (unsigned i = 0; i < sizeof(pixels); ++i) {
        nonzero += pixels[i] != 0; changed += pixels[i] != 0xa7;
    }
    static unsigned sequence;
    unsigned capture = ++sequence;
    unsigned char unpack_pixels[sizeof(pixels)];
    GLint unpack_size = 0;
    unsigned unpack_nonzero = 0;
    if (unpack) glGetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SIZE, &unpack_size);
    int readable_unpack = unpack_size >= 0 && source <= (unsigned)unpack_size &&
        sizeof(unpack_pixels) <= (unsigned)unpack_size - source;
    if (unpack && readable_unpack) {
        memset(unpack_pixels, 0xa7, sizeof(unpack_pixels));
        glGetBufferSubData(GL_PIXEL_UNPACK_BUFFER, source, sizeof(unpack_pixels), unpack_pixels);
        for (unsigned i = 0; i < sizeof(unpack_pixels); ++i) unpack_nonzero += unpack_pixels[i] != 0;
    }
    fprintf(stderr, "compat32: texture-upload sequence=%u operation=%s texture=%d source=%08x unpack=%d nonzero=%u readback-changed=%u\n",
        capture, operation, texture, source, unpack, nonzero, changed);
    if (unpack) fprintf(stderr, "compat32: texture-upload unpack sequence=%u bytes=%d readable=%d nonzero=%u\n",
        capture, unpack_size, readable_unpack, unpack_nonzero);
    if (capture <= 1024) {
        mkdir(directory, 0700);
        char path[PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%d-%u-tex%d-%s.rgba", directory,
            getpid(), capture, texture, operation);
        if (n > 0 && (size_t)n < sizeof(path)) {
            FILE *file = fopen(path, "wb");
            if (file) { fwrite(pixels, 1, sizeof(pixels), file); fclose(file); }
        }
        if (unpack && readable_unpack) {
            n = snprintf(path, sizeof(path), "%s/%d-%u-tex%d-%s.pbo", directory,
                getpid(), capture, texture, operation);
            if (n > 0 && (size_t)n < sizeof(path)) {
                FILE *file = fopen(path, "wb");
                if (file) { fwrite(unpack_pixels, 1, sizeof(unpack_pixels), file); fclose(file); }
            }
        }
    }
}
static GLfloat float_arg(uint32_t word)
{
    GLfloat value; memcpy(&value, &word, sizeof(value)); return value;
}

int gl_misc_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *r)
{
    if (*name == '_') ++name;
#define IS(s) (!strcmp(name, s))
#define PTR(i) ((void *)(uintptr_t)a[i])
    *r = 0;
    if (IS("glPixelStorei")) glPixelStorei(a[0], a[1]);
    else if (IS("glFogf")) glFogf(a[0], float_arg(a[1]));
    else if (IS("glFogfv")) glFogfv(a[0], PTR(1));
    else if (IS("glFogi")) glFogi(a[0], a[1]);
    else if (IS("glHint")) glHint(a[0], a[1]);
    else if (IS("glShadeModel")) glShadeModel(a[0]);
    else if (IS("glColorMaterial")) glColorMaterial(a[0], a[1]);
    else if (IS("glLightModelfv")) glLightModelfv(a[0], PTR(1));
    else if (IS("glLightModeli")) glLightModeli(a[0], a[1]);
    else if (IS("glLightf")) glLightf(a[0], a[1], float_arg(a[2]));
    else if (IS("glLightfv")) glLightfv(a[0], a[1], PTR(2));
    else if (IS("glLighti")) glLighti(a[0], a[1], a[2]);
    else if (IS("glMaterialf")) glMaterialf(a[0], a[1], float_arg(a[2]));
    else if (IS("glMaterialfv")) glMaterialfv(a[0], a[1], PTR(2));
    else if (IS("glTexEnvfv")) glTexEnvfv(a[0], a[1], PTR(2));
    else if (IS("glTexEnvi")) glTexEnvi(a[0], a[1], a[2]);
    else if (IS("glGetTexEnvfv")) glGetTexEnvfv(a[0], a[1], PTR(2));
    else if (IS("glGetTexEnviv")) glGetTexEnviv(a[0], a[1], PTR(2));
    else if (IS("glTexGenfv")) glTexGenfv(a[0], a[1], PTR(2));
    else if (IS("glTexGeni")) glTexGeni(a[0], a[1], a[2]);
    else if (IS("glLoadMatrixf")) glLoadMatrixf(PTR(0));
    else if (IS("glMultMatrixf")) glMultMatrixf(PTR(0));
    else if (IS("glPushMatrix")) glPushMatrix();
    else if (IS("glPopMatrix")) glPopMatrix();
    else if (IS("glScalef")) glScalef(float_arg(a[0]), float_arg(a[1]), float_arg(a[2]));
    else if (IS("glTranslatef")) glTranslatef(float_arg(a[0]), float_arg(a[1]), float_arg(a[2]));
    else if (IS("glColorPointer")) glColorPointer(a[0], a[1], a[2], PTR(3));
    else if (IS("glNormalPointer")) glNormalPointer(a[0], a[1], PTR(2));
    else if (IS("glSecondaryColorPointer")) glSecondaryColorPointer(a[0], a[1], a[2], PTR(3));
    else if (IS("glSecondaryColor3f")) glSecondaryColor3f(float_arg(a[0]), float_arg(a[1]), float_arg(a[2]));
    else if (IS("glSecondaryColor3ub")) glSecondaryColor3ub(a[0], a[1], a[2]);
    else if (IS("glColor4fv")) glColor4fv(PTR(0));
    else if (IS("glColor4ub")) glColor4ub(a[0], a[1], a[2], a[3]);
    else if (IS("glVertex2f")) glVertex2f(float_arg(a[0]), float_arg(a[1]));
    else if (IS("glVertex4f")) glVertex4f(float_arg(a[0]), float_arg(a[1]), float_arg(a[2]), float_arg(a[3]));
    else if (IS("glTexCoord4f")) glTexCoord4f(float_arg(a[0]), float_arg(a[1]), float_arg(a[2]), float_arg(a[3]));
    else if (IS("glMultiTexCoord1fvARB")) glMultiTexCoord1fv(a[0], PTR(1));
    else if (IS("glMultiTexCoord2fvARB")) glMultiTexCoord2fv(a[0], PTR(1));
    else if (IS("glMultiTexCoord3fvARB")) glMultiTexCoord3fv(a[0], PTR(1));
    else if (IS("glMultiTexCoord4fvARB")) glMultiTexCoord4fv(a[0], PTR(1));
    else if (IS("glGenLists")) *r = glGenLists(a[0]);
    else if (IS("glDeleteLists")) glDeleteLists(a[0], a[1]);
    else if (IS("glListBase")) glListBase(a[0]);
    else if (IS("glCallLists")) glCallLists(a[0], a[1], PTR(2));
    else if (IS("glRasterPos2i")) glRasterPos2i(a[0], a[1]);
    else if (IS("glWindowPos2i")) glWindowPos2i(a[0], a[1]);
    else if (IS("glPixelZoom")) glPixelZoom(float_arg(a[0]), float_arg(a[1]));
    else if (IS("glCopyPixels")) glCopyPixels(a[0], a[1], a[2], a[3], a[4]);
    else if (IS("glActiveStencilFaceEXT")) glActiveStencilFaceEXT(a[0]);
    else if (IS("glStencilFuncSeparateATI")) glStencilFuncSeparateATI(a[0], a[1], a[2], a[3]);
    else if (IS("glGetBooleanv")) glGetBooleanv(a[0], PTR(1));
    else if (IS("glGetDoublev")) glGetDoublev(a[0], PTR(1));
    else if (IS("glGetTexLevelParameteriv")) glGetTexLevelParameteriv(a[0], a[1], a[2], PTR(3));
    else if (IS("glGetTexImage")) glGetTexImage(a[0], a[1], a[2], a[3], PTR(4));
    else if (IS("glGetCompressedTexImage") || IS("glGetCompressedTexImageARB")) glGetCompressedTexImage(a[0], a[1], PTR(2));
    else if (IS("glTexSubImage2D")) {
        if (getenv("LP32_TRACE_TFU_TEXTURE_UPLOADS") && a[4] == 64 && a[5] == 64)
            fprintf(stderr, "compat32: texture-subimage target=%04x level=%u offset=%u,%u format=%04x type=%04x source=%08x\n",
                a[0], a[1], a[2], a[3], a[6], a[7], a[8]);
        if (!upload_tfu_packed_pixels(a))
            glTexSubImage2D(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], PTR(8));
        gl_misc_bridge32_trace_texture_upload("subimage", a[0], a[1], a[8]);
    }
    else if (IS("glCompressedTexImage3D") || IS("glCompressedTexImage3DARB")) {
        if (!gl_volume_bridge32_upload(a)) glCompressedTexImage3D(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], PTR(8));
    }
    else if (IS("glIsEnabled")) *r = glIsEnabled(a[0]);
    else if (IS("glIsTexture")) *r = glIsTexture(a[0]);
    else if (IS("glPolygonMode")) glPolygonMode(a[0], a[1]);
    else if (IS("glPushAttrib")) glPushAttrib(a[0]);
    else if (IS("glPopAttrib")) glPopAttrib();
    else if (IS("glEnableClientState")) glEnableClientState(a[0]);
    else if (IS("glDisableClientState")) glDisableClientState(a[0]);
    else if (IS("glVertexPointer")) glVertexPointer(a[0], a[1], a[2], PTR(3));
    else if (IS("glTexCoordPointer")) glTexCoordPointer(a[0], a[1], a[2], PTR(3));
    else if (IS("glTexCoord2f")) glTexCoord2f(float_arg(a[0]), float_arg(a[1]));
    else if (IS("glColor4sv")) glColor4sv(PTR(0));
    else if (IS("glClipPlane")) glClipPlane(a[0], PTR(1));
    else if (IS("glDepthRange")) {
        GLdouble values[2]; memcpy(values, a, sizeof(values)); glDepthRange(values[0], values[1]);
    } else if (IS("glOrtho")) {
        GLdouble values[6]; memcpy(values, a, sizeof(values));
        glOrtho(values[0], values[1], values[2], values[3], values[4], values[5]);
    } else if (IS("glColorMaskIndexedEXT")) glColorMaskIndexedEXT(a[0], a[1], a[2], a[3], a[4]);
    else if (IS("glEnableIndexedEXT")) glEnableIndexedEXT(a[0], a[1]);
    else if (IS("glDisableIndexedEXT")) glDisableIndexedEXT(a[0], a[1]);
    else if (IS("glGetBooleanIndexedvEXT")) glGetBooleanIndexedvEXT(a[0], a[1], PTR(2));
    else if (IS("glFramebufferTexture3D") || IS("glFramebufferTexture3DEXT")) glFramebufferTexture3DEXT(a[0], a[1], a[2], a[3], a[4], a[5]);
    else if (IS("glRenderbufferStorageMultisample") || IS("glRenderbufferStorageMultisampleEXT")) glRenderbufferStorageMultisampleEXT(a[0], a[1], a[2], a[3], a[4]);
    else if (IS("glGenFencesAPPLE")) glGenFencesAPPLE(a[0], PTR(1));
    else if (IS("glDeleteFencesAPPLE")) glDeleteFencesAPPLE(a[0], PTR(1));
    else if (IS("glSetFenceAPPLE")) glSetFenceAPPLE(a[0]);
    else if (IS("glTestFenceAPPLE")) *r = glTestFenceAPPLE(a[0]);
    else if (IS("glFinishFenceAPPLE")) glFinishFenceAPPLE(a[0]);
    else if (IS("glTextureRangeAPPLE")) glTextureRangeAPPLE(a[0], a[1], PTR(2));
    else if (IS("glGetTexParameterPointervAPPLE")) {
        void *value = NULL; glGetTexParameterPointervAPPLE(a[0], a[1], &value);
        if ((uintptr_t)value > UINT32_MAX) return 0;
        *(uint32_t *)PTR(2) = (uint32_t)(uintptr_t)value;
    } else if (IS("glUniformBufferEXT")) glUniformBufferEXT(a[0], a[1], a[2]);
    else return 0;
    return 1;
#undef IS
#undef PTR
}
