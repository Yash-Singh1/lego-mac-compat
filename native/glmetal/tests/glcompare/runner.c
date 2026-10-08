/* glcompare runner.
 *
 *   glcompare --provider apple|PATH --cases libglcases.dylib --out DIR
 *             [--filter TEXT] [--only NAME] [--list]
 *       Loads one OpenGL implementation, then the case library (whose GL
 *       calls bind to that implementation), renders every case into an
 *       offscreen framebuffer and writes DIR/<case>.png and DIR/results.tsv.
 *
 *   glcompare --compare REFDIR TESTDIR --out DIR
 *       Compares two result directories pixel by pixel and writes
 *       DIR/compare.tsv and DIR/<case>.diff.png.
 *
 * Only one implementation is loaded per process, so the case library's
 * flat-namespace GL references are unambiguous. */
#define GL_SILENCE_DEPRECATION 1
#include <ApplicationServices/ApplicationServices.h>
#include <ImageIO/ImageIO.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "glcompare.h"

#define APPLE_OPENGL "/System/Library/Frameworks/OpenGL.framework/OpenGL"

int glc_variant;
int glc_width, glc_height;

static jmp_buf case_jump;
static char case_message[1024];

__attribute__((visibility("default"))) void glc_fail(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(case_message, sizeof case_message, format, args);
    va_end(args);
    longjmp(case_jump, 1);
}

/* The runner resolves its own GL and CGL calls from the loaded
   implementation explicitly; only the cases use flat lookup. */
static struct {
    CGLError (*ChoosePixelFormat)(const CGLPixelFormatAttribute *, CGLPixelFormatObj *, GLint *);
    CGLError (*CreateContext)(CGLPixelFormatObj, CGLContextObj, CGLContextObj *);
    CGLError (*DestroyPixelFormat)(CGLPixelFormatObj);
    CGLError (*DestroyContext)(CGLContextObj);
    CGLError (*SetCurrentContext)(CGLContextObj);
    void (*GenFramebuffers)(GLsizei, GLuint *);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*DeleteFramebuffers)(GLsizei, const GLuint *);
    void (*GenRenderbuffers)(GLsizei, GLuint *);
    void (*BindRenderbuffer)(GLenum, GLuint);
    void (*DeleteRenderbuffers)(GLsizei, const GLuint *);
    void (*RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
    void (*RenderbufferStorageMultisample)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
    void (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
    GLenum (*CheckFramebufferStatus)(GLenum);
    void (*BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    void (*PixelStorei)(GLenum, GLint);
    void (*Finish)(void);
    GLenum (*GetError)(void);
    const GLubyte *(*GetString)(GLenum);
} gl;

static void *provider;

__attribute__((visibility("default"))) void *glc_lookup(const char *name)
{
    return dlsym(provider, name);
}

static void load_provider(const char *path)
{
    provider = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!provider) {
        fprintf(stderr, "glcompare: cannot load %s: %s\n", path, dlerror());
        exit(2);
    }
    /* Implementations that need explicit start-up before the first CGL
       call export an initializer. */
    static const char *const initializers[] = {"glmetal_initialize"};
    for (size_t i = 0; i < sizeof initializers / sizeof initializers[0]; ++i) {
        bool (*initialize)(void) = (bool (*)(void))dlsym(provider, initializers[i]);
        if (initialize && !initialize()) {
            fprintf(stderr, "glcompare: %s failed\n", initializers[i]);
            exit(2);
        }
    }
#define L(field, name)                                                         \
    do {                                                                       \
        *(void **)&gl.field = dlsym(provider, name);                           \
        if (!gl.field) {                                                       \
            fprintf(stderr, "glcompare: %s lacks %s\n", path, name);           \
            exit(2);                                                           \
        }                                                                      \
    } while (0)
    L(ChoosePixelFormat, "CGLChoosePixelFormat");
    L(CreateContext, "CGLCreateContext");
    L(DestroyPixelFormat, "CGLDestroyPixelFormat");
    L(DestroyContext, "CGLDestroyContext");
    L(SetCurrentContext, "CGLSetCurrentContext");
    L(GenFramebuffers, "glGenFramebuffers");
    L(BindFramebuffer, "glBindFramebuffer");
    L(DeleteFramebuffers, "glDeleteFramebuffers");
    L(GenRenderbuffers, "glGenRenderbuffers");
    L(BindRenderbuffer, "glBindRenderbuffer");
    L(DeleteRenderbuffers, "glDeleteRenderbuffers");
    L(RenderbufferStorage, "glRenderbufferStorage");
    L(RenderbufferStorageMultisample, "glRenderbufferStorageMultisample");
    L(FramebufferRenderbuffer, "glFramebufferRenderbuffer");
    L(CheckFramebufferStatus, "glCheckFramebufferStatus");
    L(BlitFramebuffer, "glBlitFramebuffer");
    L(Viewport, "glViewport");
    L(ReadPixels, "glReadPixels");
    L(PixelStorei, "glPixelStorei");
    L(Finish, "glFinish");
    L(GetError, "glGetError");
    L(GetString, "glGetString");
#undef L
}

/* ---- case discovery ------------------------------------------------------ */

static const struct glc_case *cases;
static size_t case_count;

static void load_cases(const char *path)
{
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "glcompare: cannot load cases %s: %s\n", path, dlerror());
        exit(2);
    }
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) snprintf(resolved, sizeof resolved, "%s", path);
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
        char image[PATH_MAX];
        if (!realpath(_dyld_get_image_name(i), image)) continue;
        if (strcmp(image, resolved)) continue;
        unsigned long size = 0;
        cases = (const struct glc_case *)getsectiondata(
            (const struct mach_header_64 *)_dyld_get_image_header(i), "__DATA", "__glc_cases", &size);
        case_count = size / sizeof *cases;
        return;
    }
    fprintf(stderr, "glcompare: %s has no cases\n", path);
    exit(2);
}

static void case_label(const struct glc_case *c, int variant, char *out, size_t size)
{
    if (c->variant_count) snprintf(out, size, "%s.%s", c->name, c->variant_names[variant]);
    else snprintf(out, size, "%s", c->name);
}

/* ---- PNG ----------------------------------------------------------------- */

static bool write_png(const char *path, const unsigned char *rgba, int width, int height)
{
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    /* Straight alpha is what GL wrote; an unpremultiplied image keeps the
       bytes untouched. */
    CGDataProviderRef data = CGDataProviderCreateWithData(NULL, rgba, (size_t)width * height * 4, NULL);
    CGImageRef image = CGImageCreate((size_t)width, (size_t)height, 8, 32, (size_t)width * 4, space,
                                     kCGImageAlphaLast | kCGBitmapByteOrderDefault, data, NULL,
                                     false, kCGRenderingIntentDefault);
    CFStringRef string = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, string, kCFURLPOSIXPathStyle, false);
    CGImageDestinationRef destination = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    bool ok = false;
    if (destination && image) {
        CGImageDestinationAddImage(destination, image, NULL);
        ok = CGImageDestinationFinalize(destination);
    }
    if (destination) CFRelease(destination);
    CFRelease(url);
    CFRelease(string);
    if (image) CGImageRelease(image);
    CGDataProviderRelease(data);
    CGColorSpaceRelease(space);
    return ok;
}

/* Returns malloc'd RGBA8 (straight alpha, top row first) or NULL. */
static unsigned char *read_png(const char *path, int *width, int *height)
{
    CFStringRef string = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, string, kCFURLPOSIXPathStyle, false);
    CGImageSourceRef source = CGImageSourceCreateWithURL(url, NULL);
    CFRelease(url);
    CFRelease(string);
    if (!source) return NULL;
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, NULL);
    CFRelease(source);
    if (!image) return NULL;
    *width = (int)CGImageGetWidth(image);
    *height = (int)CGImageGetHeight(image);
    CFDataRef data = CGDataProviderCopyData(CGImageGetDataProvider(image));
    unsigned char *pixels = NULL;
    if (data && CGImageGetBitsPerPixel(image) == 32 && CGImageGetBitsPerComponent(image) == 8) {
        size_t row = CGImageGetBytesPerRow(image);
        pixels = malloc((size_t)*width * *height * 4);
        for (int y = 0; y < *height; ++y)
            memcpy(pixels + (size_t)y * *width * 4, CFDataGetBytePtr(data) + (size_t)y * row,
                   (size_t)*width * 4);
    }
    if (data) CFRelease(data);
    CGImageRelease(image);
    return pixels;
}

/* ---- running ------------------------------------------------------------- */

struct result {
    const char *status;
    GLenum error;
    double milliseconds;
};

static bool make_context(const struct glc_case *c, CGLContextObj *out)
{
    CGLPixelFormatAttribute attributes[] = {
        kCGLPFAOpenGLProfile,
        (CGLPixelFormatAttribute)(c->profile == GLC_CORE ? kCGLOGLPVersion_3_2_Core
                                                         : kCGLOGLPVersion_Legacy),
        kCGLPFAColorSize, 24, kCGLPFAAlphaSize, 8, kCGLPFADepthSize, 24, kCGLPFAStencilSize, 8,
        kCGLPFAAccelerated, 0,
    };
    CGLPixelFormatObj format = NULL;
    GLint count = 0;
    if (gl.ChoosePixelFormat(attributes, &format, &count) != kCGLNoError || !format) return false;
    CGLError error = gl.CreateContext(format, NULL, out);
    gl.DestroyPixelFormat(format);
    if (error != kCGLNoError) return false;
    return gl.SetCurrentContext(*out) == kCGLNoError;
}

static void flip_rows(unsigned char *pixels, int width, int height)
{
    size_t row = (size_t)width * 4;
    unsigned char *temp = malloc(row);
    for (int y = 0; y < height / 2; ++y) {
        memcpy(temp, pixels + y * row, row);
        memcpy(pixels + y * row, pixels + (height - 1 - y) * row, row);
        memcpy(pixels + (height - 1 - y) * row, temp, row);
    }
    free(temp);
}

static struct result run_case(const struct glc_case *c, int variant, const char *label,
                              const char *out_dir)
{
    struct result result = {"ok", GL_NO_ERROR, 0};
    CGLContextObj context = NULL;
    if (!make_context(c, &context)) {
        snprintf(case_message, sizeof case_message, "no %s context", c->profile == GLC_CORE ? "core" : "legacy");
        result.status = "no-context";
        return result;
    }
    int width = c->width, height = c->height;
    GLuint framebuffers[2] = {0}, renderbuffers[4] = {0};
    gl.GenFramebuffers(2, framebuffers);
    gl.GenRenderbuffers(4, renderbuffers);
    /* [0]: what the case draws into (multisampled when asked); [1]: the
       single-sample resolve target that is read back. */
    for (int i = 0; i < 2; ++i) {
        int samples = i == 0 ? c->samples : 0;
        if (i == 1 && !c->samples) break;
        gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffers[i]);
        gl.BindRenderbuffer(GL_RENDERBUFFER, renderbuffers[i * 2]);
        if (samples) gl.RenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, width, height);
        else gl.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
        gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffers[i * 2]);
        gl.BindRenderbuffer(GL_RENDERBUFFER, renderbuffers[i * 2 + 1]);
        if (samples) gl.RenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, width, height);
        else gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
        gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, renderbuffers[i * 2 + 1]);
        gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, renderbuffers[i * 2 + 1]);
        if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            snprintf(case_message, sizeof case_message, "offscreen framebuffer incomplete");
            result.status = "no-framebuffer";
            gl.SetCurrentContext(NULL);
            gl.DestroyContext(context);
            return result;
        }
    }
    gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffers[0]);
    gl.Viewport(0, 0, width, height);
    while (gl.GetError() != GL_NO_ERROR) {}

    glc_variant = variant;
    glc_width = width;
    glc_height = height;
    case_message[0] = 0;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    if (setjmp(case_jump) == 0) {
        c->run();
        result.error = gl.GetError();
    } else {
        result.status = "failed";
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    result.milliseconds = (double)(end.tv_sec - start.tv_sec) * 1e3 + (double)(end.tv_nsec - start.tv_nsec) / 1e6;

    if (c->samples) {
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, framebuffers[0]);
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers[1]);
        gl.BlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffers[1]);
    } else {
        gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffers[0]);
    }
    unsigned char *pixels = calloc((size_t)width * height, 4);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl.ReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    gl.Finish();
    flip_rows(pixels, width, height);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s.png", out_dir, label);
    if (!write_png(path, pixels, width, height) && !strcmp(result.status, "ok")) result.status = "no-image";
    free(pixels);

    gl.DeleteFramebuffers(2, framebuffers);
    gl.DeleteRenderbuffers(4, renderbuffers);
    gl.SetCurrentContext(NULL);
    gl.DestroyContext(context);
    return result;
}

static int run_all(const char *out_dir, const char *filter, const char *only, bool list)
{
    mkdir(out_dir, 0755);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/results.tsv", out_dir);
    FILE *tsv = list ? NULL : fopen(path, only ? "a" : "w");
    if (!list && !tsv) {
        perror(path);
        return 2;
    }
    if (tsv && !only)
        fprintf(tsv, "case\tstatus\tgl_error\ttolerance\toutlier_fraction\twidth\theight\tms\tmessage\n");
    int failures = 0;
    /* Pace GPU cases after their contexts have finished and been destroyed.
       Keep this outside the measured case time. */
    const char *delay_text = getenv("GLCOMPARE_CASE_DELAY_MS");
    long delay_ms = delay_text ? strtol(delay_text, NULL, 10) : 50;
    if (delay_ms < 0) delay_ms = 0;
    if (delay_ms > 1000) delay_ms = 1000;
    for (size_t i = 0; i < case_count; ++i) {
        const struct glc_case *c = &cases[i];
        int variants = c->variant_count ? c->variant_count : 1;
        for (int v = 0; v < variants; ++v) {
            char label[256];
            case_label(c, v, label, sizeof label);
            if (filter && !strstr(label, filter)) continue;
            if (only && strcmp(label, only)) continue;
            if (list) {
                printf("%s\n", label);
                continue;
            }
            struct result r = run_case(c, v, label, out_dir);
            if (strcmp(r.status, "ok")) ++failures;
            fprintf(tsv, "%s\t%s\t0x%04x\t%d\t%g\t%d\t%d\t%.2f\t%s\n", label, r.status, r.error,
                    c->tolerance, c->outlier_fraction, c->width, c->height, r.milliseconds,
                    case_message);
            fflush(tsv);
            if (strcmp(r.status, "ok") || r.error)
                fprintf(stderr, "%-48s %s gl_error=0x%04x %s\n", label, r.status, r.error, case_message);
            if (delay_ms) {
                struct timespec pause = {delay_ms / 1000, (delay_ms % 1000) * 1000000};
                nanosleep(&pause, NULL);
            }
        }
    }
    if (tsv) fclose(tsv);
    return failures;
}

/* ---- comparison ---------------------------------------------------------- */

struct expectation {
    char name[256];
    int tolerance;
    double outliers;
};

static int compare_dirs(const char *reference, const char *test, const char *out_dir)
{
    mkdir(out_dir, 0755);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/results.tsv", reference);
    FILE *in = fopen(path, "r");
    if (!in) {
        fprintf(stderr, "glcompare: no %s\n", path);
        return 2;
    }
    snprintf(path, sizeof path, "%s/compare.tsv", out_dir);
    FILE *out = fopen(path, "w");
    if (!out) {
        perror(path);
        fclose(in);
        return 2;
    }
    fprintf(out, "case\tverdict\tmax_diff\tbad_pixels\ttotal_pixels\n");
    char line[4096];
    fgets(line, sizeof line, in); /* header */
    int mismatches = 0, total = 0;
    while (fgets(line, sizeof line, in)) {
        struct expectation e = {0};
        char status[64];
        if (sscanf(line, "%255[^\t]\t%63[^\t]\t%*[^\t]\t%d\t%lf", e.name, status, &e.tolerance, &e.outliers) < 4)
            continue;
        ++total;
        char a_path[PATH_MAX], b_path[PATH_MAX];
        snprintf(a_path, sizeof a_path, "%s/%s.png", reference, e.name);
        snprintf(b_path, sizeof b_path, "%s/%s.png", test, e.name);
        int aw = 0, ah = 0, bw = 0, bh = 0;
        unsigned char *a = read_png(a_path, &aw, &ah);
        unsigned char *b = read_png(b_path, &bw, &bh);
        const char *verdict;
        int max_diff = 0;
        long bad = 0, pixels = (long)aw * ah;
        if (!a) verdict = "no-reference";
        else if (!b) verdict = "missing";
        else if (aw != bw || ah != bh) verdict = "size-mismatch";
        else {
            unsigned char *diff = calloc((size_t)pixels, 4);
            for (long p = 0; p < pixels; ++p) {
                int worst = 0;
                for (int ch = 0; ch < 4; ++ch) {
                    int d = abs((int)a[p * 4 + ch] - (int)b[p * 4 + ch]);
                    if (d > worst) worst = d;
                }
                if (worst > max_diff) max_diff = worst;
                bool out_of_tolerance = worst > e.tolerance;
                if (out_of_tolerance) ++bad;
                /* Diff image: matching pixels as a dim copy of the
                   reference, mismatches in red scaled by size. */
                if (out_of_tolerance) {
                    int level = 128 + worst / 2;
                    diff[p * 4] = (unsigned char)(level > 255 ? 255 : level);
                    diff[p * 4 + 1] = 0;
                    diff[p * 4 + 2] = 0;
                } else {
                    for (int ch = 0; ch < 3; ++ch) diff[p * 4 + ch] = a[p * 4 + ch] / 4;
                }
                diff[p * 4 + 3] = 255;
            }
            verdict = (double)bad <= e.outliers * (double)pixels ? "match" : "mismatch";
            if (bad) {
                char diff_path[PATH_MAX];
                snprintf(diff_path, sizeof diff_path, "%s/%s.diff.png", out_dir, e.name);
                write_png(diff_path, diff, aw, ah);
            }
            free(diff);
        }
        if (strcmp(verdict, "match")) ++mismatches;
        fprintf(out, "%s\t%s\t%d\t%ld\t%ld\n", e.name, verdict, max_diff, bad, pixels);
        free(a);
        free(b);
    }
    fclose(in);
    fclose(out);
    printf("glcompare: %d/%d cases match\n", total - mismatches, total);
    return mismatches ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *provider_path = NULL, *cases_path = NULL, *out_dir = NULL;
    const char *filter = NULL, *only = NULL, *compare_a = NULL, *compare_b = NULL;
    bool list = false, info = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--provider") && i + 1 < argc) provider_path = argv[++i];
        else if (!strcmp(argv[i], "--cases") && i + 1 < argc) cases_path = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
        else if (!strcmp(argv[i], "--filter") && i + 1 < argc) filter = argv[++i];
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--list")) list = true;
        else if (!strcmp(argv[i], "--info")) info = true;
        else if (!strcmp(argv[i], "--compare") && i + 2 < argc) {
            compare_a = argv[++i];
            compare_b = argv[++i];
        } else {
            fprintf(stderr, "glcompare: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (compare_a) return compare_dirs(compare_a, compare_b, out_dir ? out_dir : compare_b);
    if (!provider_path || !cases_path || (!out_dir && !list && !info)) {
        fprintf(stderr, "usage: glcompare --provider apple|PATH --cases LIB --out DIR [--filter T] [--only NAME] [--list]\n"
                        "       glcompare --compare REFDIR TESTDIR --out DIR\n");
        return 2;
    }
    load_provider(strcmp(provider_path, "apple") ? provider_path : APPLE_OPENGL);
    load_cases(cases_path);
    if (info) {
        struct glc_case probe = {.profile = GLC_LEGACY, .width = 1, .height = 1};
        for (int p = 0; p < 2; ++p) {
            probe.profile = p;
            CGLContextObj context;
            if (!make_context(&probe, &context)) continue;
            printf("%s: %s | %s | %s\n", p ? "core" : "legacy", gl.GetString(GL_VENDOR),
                   gl.GetString(GL_RENDERER), gl.GetString(GL_VERSION));
            gl.SetCurrentContext(NULL);
            gl.DestroyContext(context);
        }
        return 0;
    }
    int failures = run_all(out_dir ? out_dir : ".", filter, only, list);
    return failures ? 1 : 0;
}
