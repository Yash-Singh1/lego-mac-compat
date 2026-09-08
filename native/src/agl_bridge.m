#import <Cocoa/Cocoa.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl.h>
#include "agl_bridge.h"
#include "carbon_native.h"
#include "carbon_display.h"
#include "objc_bridge.h"
#include "compat_runtime.h"
#include "tfu_timing.h"
#include "movie_bridge.h"

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/* AGL uses a different pixel-format attribute list from NSOpenGL. In
   particular, AGL_RED_SIZE is not NSOpenGLPFAColorSize, and AGL_RGBA has no
   Cocoa equivalent. Translate before asking the host to select a format. */
static NSOpenGLPixelFormat *choose_format(const int32_t *guest, uint32_t display_mask) {
    if (!guest) return nil;
    NSOpenGLPixelFormatAttribute attributes[256];
    unsigned output = 0, index = 0;
    int color = 0, accum = 0, pixel_size = 0;
    bool terminated = false;
    while (index < 128 && output < 248) {
        int attribute = guest[index++];
        if (!attribute) { terminated = true; break; }
        switch (attribute) {
            case 4: case 54: case 78: case 90: break; // RGBA; composited fullscreen; MP-safe; FBO pbuffer
            case 2: case 3: if (guest[index++]) return nil; break;
            case 8: case 9: case 10: color += MAX(0, guest[index++]); break;
            case 14: case 15: case 16: case 17: accum += MAX(0, guest[index++]); break;
            case 50: pixel_size = guest[index++]; break;
            case 7: case 11: case 12: case 13: case 55: case 56: case 70: case 84:
                attributes[output++] = attribute; attributes[output++] = guest[index++]; break;
            case 1: case 5: case 6: case 51: case 52: case 53: case 57: case 58:
            case 59: case 60: case 61: case 71: case 72: case 73: case 74: case 75:
            case 76: case 80: case 81: case 83: case 91: case 96:
                attributes[output++] = attribute; break;
            default:
                fprintf(stderr, "AGL: unsupported pixel-format attribute %d\n", attribute);
                return nil;
        }
    }
    if (!terminated) return nil;
    if (color || pixel_size) { attributes[output++] = NSOpenGLPFAColorSize; attributes[output++] = MAX(color, pixel_size); }
    if (accum) { attributes[output++] = NSOpenGLPFAAccumSize; attributes[output++] = accum; }
    if (display_mask) { attributes[output++] = NSOpenGLPFAScreenMask; attributes[output++] = display_mask; }
    attributes[output] = 0;
    return [[[NSOpenGLPixelFormat alloc] initWithAttributes:attributes] autorelease];
}

/* CGLCreatePBuffer returns kCGLBadDrawable on this host. A shared texture
   stores the pixels and each attached context gets its own framebuffer.
   Keep the CGL owner alive until the shared GL resources are deleted. */
@interface LP32AGLPBuffer : NSObject {
@public
    GLsizei width, height;
    GLenum target, format;
    GLint max_level;
    GLuint texture, depth;
    CGLContextObj owner;
}
@end
@implementation LP32AGLPBuffer
- (void)dealloc {
    if (owner) {
        CGLContextObj previous = CGLGetCurrentContext();
        CGLSetCurrentContext(owner);
        if (texture) glDeleteTextures(1, &texture);
        if (depth) glDeleteRenderbuffersEXT(1, &depth);
        CGLSetCurrentContext(previous);
        CGLReleaseContext(owner);
    }
    [super dealloc];
}
@end

@interface LP32AGLContext : NSOpenGLContext {
@public
    uint32_t guest_handle, drawable;
    NSWindow *fullscreen_window;
    NSRect original_frame;
    NSUInteger original_style;
    int32_t fullscreen_size[3];
    int32_t buffer_name;
    bool owns_window;
    LP32AGLPBuffer *pbuffer;
    GLuint pbuffer_fbo;
    GLenum pbuffer_draw, pbuffer_read;
    uint64_t previous_swap_ns;
    uint64_t next_movie_swap_ns;
}
- (void)leaveFullscreen;
- (void)detachPBuffer;
@end
@implementation LP32AGLContext
- (void)leaveFullscreen {
        if (fullscreen_window) {
        [self clearDrawable];
        CGLDisable([self CGLContextObj], kCGLCESurfaceBackingSize);
        if (owns_window) [fullscreen_window close];
        else {
            [fullscreen_window setStyleMask:original_style];
            [fullscreen_window setFrame:original_frame display:YES];
        }
        [fullscreen_window release]; fullscreen_window = nil;
        memset(fullscreen_size, 0, sizeof(fullscreen_size)); owns_window = false;
    }
}
- (void)detachPBuffer {
    if (pbuffer_fbo) {
        CGLContextObj previous = CGLGetCurrentContext();
        CGLSetCurrentContext([self CGLContextObj]);
        glDeleteFramebuffersEXT(1, &pbuffer_fbo); pbuffer_fbo = 0;
        CGLSetCurrentContext(previous);
    }
    [pbuffer release]; pbuffer = nil;
}
- (void)dealloc { [self detachPBuffer]; [self leaveFullscreen]; [self clearDrawable]; [super dealloc]; }
@end

static id object(uint32_t handle) { return objc_bridge32_host_object(handle); }
static void release_handle(uint32_t handle) {
    uint64_t ignored; objc_bridge32_dispatch("_CFRelease", &handle, &ignored);
}
static LP32AGLContext *context(uint32_t handle) {
    id value = object(handle); return [value isKindOfClass:[LP32AGLContext class]] ? value : nil;
}
static NSView *drawable_view(uint32_t handle) {
    id value = object(handle);
    if ([value isKindOfClass:[NSView class]]) return value;
    return [(NSWindow *)carbon_native32_cocoa_window(handle) contentView];
}

static BOOL attach_pbuffer(LP32AGLContext *ctx, LP32AGLPBuffer *buffer, GLint face, GLint level, GLint screen) {
    if (face || level || screen != [ctx currentVirtualScreen]) return NO;
    CGLContextObj previous = CGLGetCurrentContext(), native = [ctx CGLContextObj];
    CGLSetCurrentContext(native);
    if (!buffer->owner) {
        GLint previous_texture, previous_renderbuffer, previous_unpack_buffer;
        glGetIntegerv(buffer->target == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D : GL_TEXTURE_BINDING_RECTANGLE_ARB, &previous_texture);
        glGetIntegerv(GL_RENDERBUFFER_BINDING_EXT, &previous_renderbuffer);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &previous_unpack_buffer);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glGenTextures(1, &buffer->texture); glBindTexture(buffer->target, buffer->texture);
        glTexParameteri(buffer->target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(buffer->target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(buffer->target, 0, buffer->format == GL_RGB ? GL_RGB8 : GL_RGBA8,
            buffer->width, buffer->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenRenderbuffersEXT(1, &buffer->depth); glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, buffer->depth);
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, buffer->width, buffer->height);
        glBindTexture(buffer->target, previous_texture); glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, previous_renderbuffer);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, previous_unpack_buffer);
        buffer->owner = CGLRetainContext(native);
    }
    if (!glIsTexture(buffer->texture)) { CGLSetCurrentContext(previous); return NO; }
    [ctx detachPBuffer];
    [ctx clearDrawable];
    glGenFramebuffersEXT(1, &ctx->pbuffer_fbo); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, ctx->pbuffer_fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, buffer->target, buffer->texture, 0);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, buffer->depth);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, buffer->depth);
    BOOL success = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT;
    if (success) {
        ctx->pbuffer = [buffer retain]; ctx->pbuffer_draw = ctx->pbuffer_read = GL_FRONT;
        glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT); glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
    } else [ctx detachPBuffer];
    CGLSetCurrentContext(previous); return success;
}

static BOOL texture_from_pbuffer(LP32AGLContext *ctx, LP32AGLPBuffer *buffer, GLenum source) {
    if (!buffer->owner || (source != GL_FRONT && source != GL_BACK)) return NO;
    CGLContextObj previous = CGLGetCurrentContext(); CGLSetCurrentContext([ctx CGLContextObj]);
    if (!glIsTexture(buffer->texture)) { CGLSetCurrentContext(previous); return NO; }
    GLint framebuffer; glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &framebuffer);
    GLuint copy_fbo; glGenFramebuffersEXT(1, &copy_fbo); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, copy_fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, buffer->target, buffer->texture, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
    BOOL success = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT;
    if (success) glCopyTexImage2D(buffer->target, 0, buffer->format, 0, 0, buffer->width, buffer->height, 0);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, framebuffer); glDeleteFramebuffersEXT(1, &copy_fbo);
    CGLSetCurrentContext(previous); return success;
}

/* An AGL pbuffer is framebuffer zero to the guest. Translate just the GL
   operations that refer to the default framebuffer; regular game FBOs
   continue to use their own draw/read attachment state. */
int agl_bridge32_gl_dispatch(const char *name, const uint32_t *a, uint64_t *result) {
    if (*name == '_') ++name;
    bool get = !strcmp(name, "glGetIntegerv") || !strcmp(name, "glGetFloatv") || !strcmp(name, "glGetDoublev") || !strcmp(name, "glGetBooleanv");
    bool bind = !strcmp(name, "glBindFramebuffer") || !strcmp(name, "glBindFramebufferEXT");
    bool draw_read = !strcmp(name, "glDrawBuffer") || !strcmp(name, "glReadBuffer") ||
        !strcmp(name, "glDrawBuffers") || !strcmp(name, "glDrawBuffersARB");
    if (!get && !bind && !draw_read) return 0;
    if (get && a[0] != GL_FRAMEBUFFER_BINDING_EXT && a[0] != GL_DRAW_BUFFER && a[0] != GL_DRAW_BUFFER0_ARB && a[0] != GL_READ_BUFFER) return 0;
    LP32AGLContext *ctx = (id)[NSOpenGLContext currentContext];
    if (![ctx isKindOfClass:[LP32AGLContext class]] || !ctx->pbuffer_fbo) return 0;
    *result = 0;
    if (!strcmp(name, "glBindFramebuffer") || !strcmp(name, "glBindFramebufferEXT")) {
        glBindFramebufferEXT(a[0], a[1] ?: ctx->pbuffer_fbo); return 1;
    }
    GLint binding; glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &binding);
    if ((GLuint)binding != ctx->pbuffer_fbo) return 0;
    if (!strcmp(name, "glDrawBuffer") || !strcmp(name, "glReadBuffer")) {
        GLenum mode = a[0], host = mode == GL_FRONT || mode == GL_BACK || mode == GL_FRONT_AND_BACK ? GL_COLOR_ATTACHMENT0_EXT : mode;
        if (!strcmp(name, "glDrawBuffer")) { glDrawBuffer(host); ctx->pbuffer_draw = mode; }
        else { glReadBuffer(host); ctx->pbuffer_read = mode; }
        return 1;
    }
    if (!strcmp(name, "glDrawBuffers") || !strcmp(name, "glDrawBuffersARB")) {
        if (a[0] == 1) {
            GLenum mode = *(GLenum *)(uintptr_t)a[1];
            GLenum host = mode == GL_FRONT || mode == GL_BACK ? GL_COLOR_ATTACHMENT0_EXT : mode;
            glDrawBuffersARB(1, &host); ctx->pbuffer_draw = mode; return 1;
        }
        return 0;
    }
    if (!strcmp(name, "glGetIntegerv") || !strcmp(name, "glGetFloatv") || !strcmp(name, "glGetDoublev") || !strcmp(name, "glGetBooleanv")) {
        GLint value;
        if (a[0] == GL_FRAMEBUFFER_BINDING_EXT) value = 0;
        else if (a[0] == GL_DRAW_BUFFER || a[0] == GL_DRAW_BUFFER0_ARB) value = ctx->pbuffer_draw;
        else if (a[0] == GL_READ_BUFFER) value = ctx->pbuffer_read;
        else return 0;
        void *output = (void *)(uintptr_t)a[1];
        if (!strcmp(name, "glGetIntegerv")) *(GLint *)output = value;
        else if (!strcmp(name, "glGetFloatv")) *(GLfloat *)output = value;
        else if (!strcmp(name, "glGetDoublev")) *(GLdouble *)output = value;
        else *(GLboolean *)output = !!value;
        return 1;
    }
    return 0;
}
static BOOL fullscreen(LP32AGLContext *ctx, const uint32_t *a) {
    if (!ctx || !a[1] || !a[2]) return NO;
    if (getenv("LP32_HEADLESS")) {
        fprintf(stderr, "AGL: headless boundary before fullscreen presentation\n"); _Exit(77);
    }
    NSApplication *app = [NSApplication sharedApplication];
    NSScreen *screen = objc_bridge32_game_screen();
    if (!screen) return NO;
    if (!ctx->fullscreen_window) {
        NSWindow *window = [[ctx view] window];
        if (!window) {
            /* Keep a native Carbon WindowRef as the input event target. */
            OSStatus (*create)(uint32_t, uint32_t, const Rect *, void **) = carbon_native32_symbol("CreateNewWindow");
            if (!create) return NO;
            Rect bounds = {100, 100, 580, 740}; void *native_window = NULL;
            if (create(13, 1u << 19, &bounds, &native_window)) return NO;
            window = carbon_native32_cocoa_window(carbon_native32_handle(native_window));
            ctx->owns_window = true;
        }
        if (!window) return NO;
        ctx->fullscreen_window = [window retain];
        ctx->original_frame = [window frame]; ctx->original_style = [window styleMask];
    }
    NSWindow *window = ctx->fullscreen_window;
    [window setStyleMask:NSWindowStyleMaskBorderless];
    [window setFrame:[screen frame] display:YES];
    [[window contentView] setWantsBestResolutionOpenGLSurface:NO];
    [ctx setView:[window contentView]];
    GLint backing[] = {(GLint)a[1], (GLint)a[2]};
    CGLSetParameter([ctx CGLContextObj], kCGLCPSurfaceBackingSize, backing);
    CGLEnable([ctx CGLContextObj], kCGLCESurfaceBackingSize);
    ctx->fullscreen_size[0] = a[1]; ctx->fullscreen_size[1] = a[2]; ctx->fullscreen_size[2] = a[3];
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    if (getenv("LP32_BACKGROUND_TEST")) [window orderBack:nil];
    else [window makeKeyAndOrderFront:nil];
    objc_bridge32_observe_cursor_window(window);
    [ctx update]; return YES;
}

int agl_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *result) {
    if (!strcmp(name, "_CGLSetOption")) { *result = CGLSetOption((CGLGlobalOption)a[0], (GLint)a[1]); return 1; }
    if (!strcmp(name, "_CGLGetOption")) { *result = CGLGetOption((CGLGlobalOption)a[0], (GLint *)(uintptr_t)a[1]); return 1; }
    if (strncmp(name, "_agl", 4)) return 0;
    @autoreleasepool {
#define IS(s) (!strcmp(name, "_" s))
#define PTR(i) ((void *)(uintptr_t)a[i])
#define RETURN(v) do { *result = (uint64_t)(v); return 1; } while (0)
    if (getenv("LP32_TRACE_AGL")) fprintf(stderr, "AGL: %s %08x %08x %08x %08x\n", name, a[0], a[1], a[2], a[3]);
    if (IS("aglChoosePixelFormat")) {
        uint32_t mask = 0;
        if (a[1] > 32 || (a[1] && !a[0])) RETURN(0);
        const uint32_t *devices = PTR(0);
        for (unsigned i = 0; i < a[1]; ++i) {
            CGDirectDisplayID display = carbon_display32_id(devices[i]);
            if (!display) RETURN(0);
            mask |= CGDisplayIDToOpenGLDisplayMask(display);
        }
        RETURN(objc_bridge32_owned_object(choose_format(PTR(2), mask)));
    }
    if (IS("aglDestroyPixelFormat") || IS("aglDestroyPBuffer")) { release_handle(a[0]); RETURN(IS("aglDestroyPBuffer")); }
    if (IS("aglCreateContext")) {
        NSOpenGLPixelFormat *format = object(a[0]); LP32AGLContext *share = context(a[1]);
        if (![format isKindOfClass:[NSOpenGLPixelFormat class]] || (a[1] && !share)) RETURN(0);
        LP32AGLContext *ctx = [[LP32AGLContext alloc] initWithFormat:format shareContext:share];
        if (!ctx) RETURN(0);
        ctx->guest_handle = objc_bridge32_owned_object(ctx);
        uint32_t handle = ctx->guest_handle; [ctx release]; RETURN(handle);
    }
    if (IS("aglGetCurrentContext")) {
        LP32AGLContext *ctx = (id)[NSOpenGLContext currentContext];
        RETURN([ctx isKindOfClass:[LP32AGLContext class]] ? ctx->guest_handle : 0);
    }
    if (IS("aglCreatePBuffer")) {
        if (!a[5]) RETURN(0);
        *(uint32_t *)PTR(5) = 0;
        if (!a[0] || !a[1] || a[0] > 16384 || a[1] > 16384 ||
            (a[2] != GL_TEXTURE_2D && a[2] != GL_TEXTURE_RECTANGLE_ARB) ||
            (a[3] != GL_RGB && a[3] != GL_RGBA) || a[4]) RETURN(0);
        LP32AGLPBuffer *buffer = [[LP32AGLPBuffer alloc] init];
        buffer->width = a[0]; buffer->height = a[1]; buffer->target = a[2]; buffer->format = a[3]; buffer->max_level = a[4];
        *(uint32_t *)PTR(5) = objc_bridge32_owned_object(buffer);
        bool success = buffer != nil; [buffer release]; RETURN(success);
    }
    LP32AGLContext *ctx = context(a[0]);
    if (IS("aglSetCurrentContext")) {
        if (!a[0]) { [NSOpenGLContext clearCurrentContext]; RETURN(1); }
        if (!ctx) RETURN(0); [ctx makeCurrentContext]; RETURN([NSOpenGLContext currentContext] == ctx);
    }
    if (!ctx) RETURN(0);
    if (IS("aglDestroyContext")) {
        if ([NSOpenGLContext currentContext] == ctx) [NSOpenGLContext clearCurrentContext];
        [ctx detachPBuffer]; [ctx leaveFullscreen]; [ctx clearDrawable]; release_handle(a[0]); RETURN(1);
    }
    if (IS("aglGetDrawable")) RETURN(ctx->drawable);
    if (IS("aglSetDrawable")) {
        NSView *view = a[1] ? drawable_view(a[1]) : nil;
        if (a[1] && !view) RETURN(0);
        [ctx detachPBuffer]; [ctx leaveFullscreen]; [ctx clearDrawable];
        ctx->drawable = a[1];
        if (view) {
            [view setWantsBestResolutionOpenGLSurface:NO]; [ctx setView:view];
            int32_t size[2];
            if (carbon_native32_surface_size([view window], size)) {
                CGLSetParameter([ctx CGLContextObj], kCGLCPSurfaceBackingSize, size);
                CGLEnable([ctx CGLContextObj], kCGLCESurfaceBackingSize);
            } else CGLDisable([ctx CGLContextObj], kCGLCESurfaceBackingSize);
            objc_bridge32_observe_cursor_window([view window]);
            carbon_native32_report_windows();
        }
        RETURN(1);
    }
    if (IS("aglSetFullScreen")) RETURN(fullscreen(ctx, a));
    if (IS("aglUpdateContext")) { [ctx update]; RETURN(1); }
    if (IS("aglGetVirtualScreen")) RETURN([ctx currentVirtualScreen]);
    if (IS("aglSetPBuffer")) {
        LP32AGLPBuffer *buffer = object(a[1]);
        if (![buffer isKindOfClass:[LP32AGLPBuffer class]]) RETURN(0);
        RETURN(attach_pbuffer(ctx, buffer, a[2], a[3], a[4]));
    }
    if (IS("aglTexImagePBuffer")) {
        LP32AGLPBuffer *buffer = object(a[1]);
        if (![buffer isKindOfClass:[LP32AGLPBuffer class]]) RETURN(0);
        RETURN(texture_from_pbuffer(ctx, buffer, a[2]));
    }
    if (IS("aglSetInteger") || IS("aglGetInteger")) {
        if (!a[2]) RETURN(0);
        if (IS("aglGetInteger") && a[1] == 54) {
            memcpy(PTR(2), ctx->fullscreen_size, sizeof(ctx->fullscreen_size)); RETURN(1);
        }
        if (a[1] == 231) {
            if (IS("aglSetInteger")) ctx->buffer_name = *(int32_t *)PTR(2);
            else *(int32_t *)PTR(2) = ctx->buffer_name;
            RETURN(1);
        }
        CGLContextParameter parameter;
        switch (a[1]) {
            case 200: parameter = kCGLCPSwapRectangle; break;
            case 222: parameter = kCGLCPSwapInterval; break;
            case 235: parameter = kCGLCPSurfaceOrder; break;
            case 236: parameter = kCGLCPSurfaceOpacity; break;
            case 304: parameter = kCGLCPSurfaceBackingSize; break;
            default: fprintf(stderr, "AGL: unsupported context parameter %u\n", a[1]); RETURN(0);
        }
        CGLError status = IS("aglSetInteger") ? CGLSetParameter([ctx CGLContextObj], parameter, PTR(2)) :
            CGLGetParameter([ctx CGLContextObj], parameter, PTR(2));
        RETURN(status == kCGLNoError);
    }
    if (IS("aglSwapBuffers")) {
        uint64_t start = tfu_time_ns();
        if (ctx->previous_swap_ns)
            tfu_log_slow("between-swaps", a[0], ctx->previous_swap_ns, start);
        /* TFU's movie loop has no game-frame limiter. Without pacing it
           submits the same decoded image over 1,000 times per second,
           contending with the guest loader and flooding WindowServer.
           AVPlayer keeps the media clock; only redundant presentation waits.
           Keep deadlines per context and reset immediately outside movies. */
        static int pace_movies = -1;
        if (pace_movies < 0) pace_movies = !getenv("LP32_NO_TFU_MOVIE_PACING");
        if (pace_movies && movie_bridge32_active()) {
            uint64_t now = start;
            while (now < ctx->next_movie_swap_ns) {
                uint64_t remaining = ctx->next_movie_swap_ns - now;
                struct timespec wait = {remaining / 1000000000, remaining % 1000000000};
                nanosleep(&wait, NULL);
                now = tfu_time_ns();
            }
            ctx->next_movie_swap_ns = now + 1000000000 / 60;
        } else ctx->next_movie_swap_ns = 0;
        /* Optional app framebuffer readback; never captures the desktop. */
        static unsigned long long swaps;
        static unsigned interval;
        if (!interval) {
            const char *value = getenv("LP32_AGL_CAPTURE_EVERY");
            long requested = value ? strtol(value, NULL, 10) : 300;
            interval = requested > 0 && requested <= 100000 ? (unsigned)requested : 300;
        }
        const char *path = getenv("LP32_AGL_CAPTURE_FRAME");
        if (path && ++swaps % interval == 0) {
            CGLContextObj previous = CGLGetCurrentContext();
            CGLSetCurrentContext([ctx CGLContextObj]);
            GLint viewport[4], read_fbo, read_buffer, pack_buffer, alignment, row_length, skip_rows, skip_pixels;
            glGetIntegerv(GL_VIEWPORT, viewport);
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_EXT, &read_fbo);
            glGetIntegerv(GL_READ_BUFFER, &read_buffer);
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
            glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
            glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
            glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
            glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
            glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, 0);
            glReadBuffer(GL_BACK);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glPixelStorei(GL_PACK_ROW_LENGTH, 0);
            glPixelStorei(GL_PACK_SKIP_ROWS, 0);
            glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
            if (viewport[2] > 0 && viewport[3] > 0 && viewport[2] <= 16384 && viewport[3] <= 16384) {
                size_t row = (size_t)viewport[2] * 3;
                void *pixels = malloc(row * viewport[3]);
                if (pixels) {
                    glReadPixels(viewport[0], viewport[1], viewport[2], viewport[3], GL_RGB, GL_UNSIGNED_BYTE, pixels);
                    NSString *temporary = [[NSString stringWithUTF8String:path] stringByAppendingString:@".tmp"];
                    FILE *file = fopen([temporary fileSystemRepresentation], "wb");
                    if (file) {
                        fprintf(file, "P6\n%d %d\n255\n", viewport[2], viewport[3]);
                        for (int y = viewport[3] - 1; y >= 0; --y) fwrite((char *)pixels + y * row, row, 1, file);
                        bool complete = !ferror(file);
                        if (fclose(file)) complete = false;
                        if (complete) rename([temporary fileSystemRepresentation], path);
                    }
                    free(pixels);
                }
            }
            glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, read_fbo);
            glReadBuffer(read_buffer);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buffer);
            glPixelStorei(GL_PACK_ALIGNMENT, alignment);
            glPixelStorei(GL_PACK_ROW_LENGTH, row_length);
            glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows);
            glPixelStorei(GL_PACK_SKIP_PIXELS, skip_pixels);
            CGLSetCurrentContext(previous);
        }
        uint64_t flush_start = tfu_time_ns();
        objc_bridge32_flush_context(ctx);
        uint64_t end = tfu_time_ns();
        tfu_log_slow("swap-flush", a[0], flush_start, end);
        ctx->previous_swap_ns = end;
        RETURN(0);
    }
    // Font display lists need a CoreText rasterization adapter; report failure.
    if (IS("aglUseFont")) { fprintf(stderr, "AGL: font display lists are not implemented\n"); RETURN(0); }
    }
    return 0;
#undef IS
#undef PTR
#undef RETURN
}

int agl_bridge32_self_test(void) {
    @autoreleasepool {
        uint32_t memory = compat_runtime32_allocate(4096, 1);
        if (!memory) return -1;
        uint32_t *words = (void *)(uintptr_t)memory;
        int32_t attributes[] = {4, 8, 8, 9, 8, 10, 8, 11, 8, 12, 24, 73, 90, 0};
        memcpy(words, attributes, sizeof(attributes));
        uint32_t a[8] = {0, 0, memory}; uint64_t r = 0;
#define CALL(s) agl_bridge32_dispatch("_" s, a, &r)
        carbon_display32_dispatch("_GetMainDevice", a, &r); words[38] = r;
        a[0] = memory + 152; a[1] = 1;
        CALL("aglChoosePixelFormat"); uint32_t format = r;
        a[0] = format; a[1] = 0; CALL("aglCreateContext"); uint32_t ctx = r;
        int failed = !format || !ctx;
        uint32_t buffer = 0, shared = 0;
        if (ctx) {
            a[0] = ctx; CALL("aglSetCurrentContext"); failed |= !r;
            CALL("aglGetCurrentContext"); failed |= r != ctx;
            a[0] = 16; a[1] = 16; a[2] = GL_TEXTURE_2D; a[3] = GL_RGBA; a[4] = 0; a[5] = memory + 128;
            CALL("aglCreatePBuffer"); buffer = words[32]; failed |= !r || !buffer;
            if (buffer) {
                a[0] = ctx; a[1] = buffer; a[2] = 0; a[3] = 0; a[4] = 0; CALL("aglSetPBuffer"); failed |= !r;
                glViewport(0, 0, 16, 16);
                uint32_t gl_args[] = {GL_FRONT, memory + 144}; uint64_t ignored;
                agl_bridge32_gl_dispatch("_glDrawBuffer", gl_args, &ignored);
                agl_bridge32_gl_dispatch("_glReadBuffer", gl_args, &ignored);
                gl_args[0] = GL_FRAMEBUFFER_BINDING_EXT; words[36] = 0x12345678;
                agl_bridge32_gl_dispatch("_glGetIntegerv", gl_args, &ignored); failed |= words[36] != 0;
                glClearColor(.25f, .5f, .75f, 1); glClear(GL_COLOR_BUFFER_BIT);
                uint8_t pixel[4] = {0}; glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
                failed |= abs(pixel[0] - 64) > 1 || abs(pixel[1] - 128) > 1 || abs(pixel[2] - 191) > 1 || pixel[3] != 255;
                fprintf(stderr, "AGL self-test: renderer=%s pixel={%u,%u,%u,%u}\n", glGetString(GL_RENDERER), pixel[0], pixel[1], pixel[2], pixel[3]);
                GLuint texture; glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
                a[2] = GL_FRONT; CALL("aglTexImagePBuffer"); failed |= !r;
                uint8_t pixels[16 * 16 * 4]; glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                failed |= memcmp(pixels + (8 * 16 + 8) * 4, pixel, 4) != 0;
                a[0] = format; a[1] = ctx; CALL("aglCreateContext"); shared = r; failed |= !shared;
                if (shared) {
                    a[0] = shared; CALL("aglSetCurrentContext"); failed |= !r || !glIsTexture(texture);
                    CALL("aglDestroyContext"); failed |= !r;
                }
                a[0] = ctx; CALL("aglSetCurrentContext"); glDeleteTextures(1, &texture);
                failed |= glGetError() != GL_NO_ERROR;
            }
            words[35] = 0; a[0] = ctx; a[1] = 222; a[2] = memory + 140; CALL("aglSetInteger"); failed |= !r;
            words[35] = 1234; words[36] = 0xdecafbad; CALL("aglGetInteger"); failed |= !r || words[35] || words[36] != 0xdecafbad;
            a[0] = ctx; CALL("aglDestroyContext"); failed |= !r;
            CALL("aglGetCurrentContext"); failed |= r != 0;
        }
        if (buffer) { a[0] = buffer; CALL("aglDestroyPBuffer"); }
        if (format) { a[0] = format; CALL("aglDestroyPixelFormat"); }
        compat_runtime32_deallocate(memory);
        fprintf(stderr, "AGL self-test: %s pixel format, offscreen rendering, texture transfer, shared context, parameters, cleanup\n", failed ? "FAIL" : "PASS");
        return failed ? -1 : 0;
#undef CALL
    }
}
