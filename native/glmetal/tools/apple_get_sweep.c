/* Records every glGetIntegerv pname Apple's OpenGL accepts in a new
 * context of each profile, with the values it returns, and every
 * capability glIsEnabled accepts (and whether glEnable may set it): the
 * reference GLMetal's getters and glEnable follow (data/apple-get-reference.txt, built into
 * build/gen/glm_apple_gets.h by tools/gen_get_reference.py).
 *
 *   make build/apple_get_sweep   (make check-reference diffs GLMetal against the data)
 *   build/apple_get_sweep > data/apple-get-reference.txt
 *
 * With a dylib argument it sweeps that implementation instead (diffing). */
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    void *lib = dlopen(argc > 1 ? argv[1] : "/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_NOW);
    if (!lib) return 1;
    bool (*init)(void) = (bool (*)(void))dlsym(lib, "glmetal_initialize");
    if (init) init();
    CGLError (*choose)(const CGLPixelFormatAttribute *, CGLPixelFormatObj *, GLint *) = dlsym(lib, "CGLChoosePixelFormat");
    CGLError (*create)(CGLPixelFormatObj, CGLContextObj, CGLContextObj *) = dlsym(lib, "CGLCreateContext");
    CGLError (*make_current)(CGLContextObj) = dlsym(lib, "CGLSetCurrentContext");
    void (*get)(GLenum, GLint *) = dlsym(lib, "glGetIntegerv");
    GLboolean (*is_enabled)(GLenum) = dlsym(lib, "glIsEnabled");
    void (*gen_renderbuffers)(GLsizei, GLuint *) = dlsym(lib, "glGenRenderbuffers");
    void (*bind_renderbuffer)(GLenum, GLuint) = dlsym(lib, "glBindRenderbuffer");
    void (*renderbuffer_storage)(GLenum, GLenum, GLsizei, GLsizei) = dlsym(lib, "glRenderbufferStorage");
    void (*renderbuffer_parameter)(GLenum, GLenum, GLint *) = dlsym(lib, "glGetRenderbufferParameteriv");
    void (*delete_renderbuffers)(GLsizei, const GLuint *) = dlsym(lib, "glDeleteRenderbuffers");
    void (*gen_textures)(GLsizei, GLuint *) = dlsym(lib, "glGenTextures");
    void (*bind_texture)(GLenum, GLuint) = dlsym(lib, "glBindTexture");
    void (*delete_textures)(GLsizei, const GLuint *) = dlsym(lib, "glDeleteTextures");
    void (*tex_image)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *) = dlsym(lib, "glTexImage2D");
    void (*level_parameter)(GLenum, GLint, GLenum, GLint *) = dlsym(lib, "glGetTexLevelParameteriv");
    void (*gen_framebuffers)(GLsizei, GLuint *) = dlsym(lib, "glGenFramebuffers");
    void (*bind_framebuffer)(GLenum, GLuint) = dlsym(lib, "glBindFramebuffer");
    void (*delete_framebuffers)(GLsizei, const GLuint *) = dlsym(lib, "glDeleteFramebuffers");
    void (*framebuffer_texture)(GLenum, GLenum, GLenum, GLuint, GLint) = dlsym(lib, "glFramebufferTexture2D");
    void (*attachment_parameter)(GLenum, GLenum, GLenum, GLint *) = dlsym(lib, "glGetFramebufferAttachmentParameteriv");
    GLenum (*check_status)(GLenum) = dlsym(lib, "glCheckFramebufferStatus");
    void (*draw_buffer)(GLenum) = dlsym(lib, "glDrawBuffer");
    void (*read_buffer)(GLenum) = dlsym(lib, "glReadBuffer");
    void (*enable)(GLenum) = dlsym(lib, "glEnable");
    void (*disable)(GLenum) = dlsym(lib, "glDisable");
    GLenum (*error)(void) = dlsym(lib, "glGetError");
    printf("# glGetIntegerv in a new context (RGBA8, depth 24, double-buffered, no drawable):\n"
           "# profile pname count values...  /  profile-cap pname settable-by-glEnable\n"
           "# profile-rb internalformat reported-internalformat red green blue alpha depth stencil (glRenderbufferStorage)\n"
           "# profile-tex internalformat: glGetTexLevelParameteriv internal-format, red green blue alpha luminance\n"
           "#   intensity depth stencil shared-exponent sizes, red green blue alpha luminance intensity depth types,\n"
           "#   compressed; then attached to a framebuffer (colour, depth or stencil by its sizes) the attachment's\n"
           "#   encoding, component type, red green blue alpha depth stencil sizes; x = GL error; then\n"
           "#   glCheckFramebufferStatus with it alone as colour 0, depth and stencil (draw/read buffers NONE)\n");
    for (int core = 0; core < 2; ++core) {
        CGLPixelFormatAttribute attributes[] = {
            kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24, kCGLPFADepthSize, (CGLPixelFormatAttribute)24,
            kCGLPFADoubleBuffer, kCGLPFAOpenGLProfile,
            (CGLPixelFormatAttribute)(core ? kCGLOGLPVersion_GL4_Core : kCGLOGLPVersion_Legacy), 0};
        CGLPixelFormatObj format;
        GLint formats;
        CGLContextObj context;
        if (choose(attributes, &format, &formats) || create(format, NULL, &context) || make_current(context)) return 1;
        for (GLenum pname = 0; pname < 0x10000; ++pname) {
            GLint v[32];
            for (int i = 0; i < 32; ++i) v[i] = 0x7eadbeef;
            while (error()) {}
            get(pname, v);
            if (error() != GL_NO_ERROR) continue;
            int n = 0;
            while (n < 32 && v[n] != 0x7eadbeef) ++n;
            printf("%s %04x %d", core ? "core" : "legacy", pname, n);
            for (int i = 0; i < n; ++i) printf(" %d", v[i]);
            printf("\n");
        }
        for (GLenum cap = 0; cap < 0x10000; ++cap) {
            while (error()) {}
            GLboolean was = is_enabled(cap);
            if (error() != GL_NO_ERROR) continue;
            enable(cap);
            bool settable = error() == GL_NO_ERROR;
            if (was) enable(cap); else disable(cap);
            while (error()) {}
            printf("%s-cap %04x %d\n", core ? "core" : "legacy", cap, settable);
        }
        for (GLenum format = 0; format < 0x10000; ++format) {
            GLuint rb;
            gen_renderbuffers(1, &rb);
            bind_renderbuffer(GL_RENDERBUFFER, rb);
            while (error()) {}
            renderbuffer_storage(GL_RENDERBUFFER, format, 16, 16);
            if (error() == GL_NO_ERROR) {
                static const GLenum pnames[] = {GL_RENDERBUFFER_INTERNAL_FORMAT, GL_RENDERBUFFER_RED_SIZE,
                                                GL_RENDERBUFFER_GREEN_SIZE, GL_RENDERBUFFER_BLUE_SIZE,
                                                GL_RENDERBUFFER_ALPHA_SIZE, GL_RENDERBUFFER_DEPTH_SIZE,
                                                GL_RENDERBUFFER_STENCIL_SIZE};
                GLint v[7] = {0};
                for (int i = 0; i < 7; ++i) renderbuffer_parameter(GL_RENDERBUFFER, pnames[i], &v[i]);
                printf("%s-rb %04x %04x %d %d %d %d %d %d\n", core ? "core" : "legacy", format, v[0], v[1], v[2], v[3], v[4],
                       v[5], v[6]);
            }
            delete_renderbuffers(1, &rb);
        }
        /* Textures: glTexImage2D with the first client format/type it takes. */
        static const GLenum clients[][2] = {
            {GL_RGBA, GL_UNSIGNED_BYTE}, {GL_RGBA, GL_FLOAT}, {GL_DEPTH_COMPONENT, GL_FLOAT},
            {GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8}, {GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV},
            {GL_RGBA_INTEGER, GL_INT}, {GL_RGBA_INTEGER, GL_UNSIGNED_INT}, {GL_STENCIL_INDEX, GL_UNSIGNED_BYTE},
            {GL_RGB, GL_UNSIGNED_BYTE}, {GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV}};
        static const GLenum levels[] = {GL_TEXTURE_INTERNAL_FORMAT, GL_TEXTURE_RED_SIZE, GL_TEXTURE_GREEN_SIZE,
            GL_TEXTURE_BLUE_SIZE, GL_TEXTURE_ALPHA_SIZE, 0x8060 /* LUMINANCE_SIZE */, 0x8061 /* INTENSITY_SIZE */,
            GL_TEXTURE_DEPTH_SIZE, GL_TEXTURE_STENCIL_SIZE, GL_TEXTURE_SHARED_SIZE, GL_TEXTURE_RED_TYPE,
            GL_TEXTURE_GREEN_TYPE, GL_TEXTURE_BLUE_TYPE, GL_TEXTURE_ALPHA_TYPE, 0x8C14 /* LUMINANCE_TYPE */,
            0x8C15 /* INTENSITY_TYPE */, GL_TEXTURE_DEPTH_TYPE, GL_TEXTURE_COMPRESSED};
        static const GLenum attachment_pnames[] = {GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING,
            GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE,
            GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE, GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE,
            GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE,
            GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE};
        GLuint fb;
        gen_framebuffers(1, &fb);
        for (GLenum format = 0; format < 0x10000; ++format) {
            /* Formats glGetTexLevelParameteriv asserts on in Apple's driver. */
            static const GLenum crashes[] = {0x8A50};
            bool skip = false;
            for (size_t k = 0; k < sizeof crashes / sizeof *crashes; ++k) skip |= format == crashes[k];
            if (skip) continue;
            GLuint texture;
            gen_textures(1, &texture);
            bind_texture(GL_TEXTURE_2D, texture);
            bool ok = false;
            for (size_t c = 0; c < sizeof clients / sizeof *clients && !ok; ++c) {
                while (error()) {}
                tex_image(GL_TEXTURE_2D, 0, (GLint)format, 4, 4, 0, clients[c][0], clients[c][1], NULL);
                ok = error() == GL_NO_ERROR;
            }
            if (ok) {
                printf("%s-tex %04x", core ? "core" : "legacy", format);
                GLint v[18];
                for (size_t i = 0; i < sizeof levels / sizeof *levels; ++i) {
                    v[i] = 0;
                    level_parameter(GL_TEXTURE_2D, 0, levels[i], &v[i]);
                    if (error()) printf(" x"); else printf(i == 0 || (i >= 10 && i < 17) ? " %04x" : " %d", v[i]);
                }
                GLenum attachment = v[7] ? GL_DEPTH_ATTACHMENT : v[8] ? GL_STENCIL_ATTACHMENT : GL_COLOR_ATTACHMENT0;
                bind_framebuffer(GL_FRAMEBUFFER, fb);
                framebuffer_texture(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, texture, 0);
                while (error()) {}
                for (size_t i = 0; i < sizeof attachment_pnames / sizeof *attachment_pnames; ++i) {
                    GLint a = 0;
                    attachment_parameter(GL_FRAMEBUFFER, attachment, attachment_pnames[i], &a);
                    if (error()) printf(" x"); else printf(i < 2 ? " %04x" : " %d", a);
                }
                framebuffer_texture(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, 0, 0);
                static const GLenum points[] = {GL_COLOR_ATTACHMENT0, GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
                for (int k = 0; k < 3; ++k) {
                    /* Colour 0 draws and reads; the others alone have no colour buffer. */
                    draw_buffer(k ? GL_NONE : GL_COLOR_ATTACHMENT0);
                    read_buffer(k ? GL_NONE : GL_COLOR_ATTACHMENT0);
                    framebuffer_texture(GL_FRAMEBUFFER, points[k], GL_TEXTURE_2D, texture, 0);
                    GLenum status = check_status(GL_FRAMEBUFFER);
                    framebuffer_texture(GL_FRAMEBUFFER, points[k], GL_TEXTURE_2D, 0, 0);
                    while (error()) {}
                    printf(" %04x", status);
                }
                bind_framebuffer(GL_FRAMEBUFFER, 0);
                printf("\n");
            }
            while (error()) {}
            delete_textures(1, &texture);
        }
        delete_framebuffers(1, &fb);
        make_current(NULL);
    }
    return 0;
}
