/* Brings up the Mesa path without the CGL layer: window, CAMetalLayer, Zink
 * context, a few presented frames, and a readback check. */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#include "../src/lp32gl_dri.h"

typedef void (*clear_color_fn)(float, float, float, float);
typedef void (*clear_fn)(unsigned);
typedef const unsigned char *(*get_string_fn)(unsigned);
typedef void (*read_pixels_fn)(int, int, int, int, unsigned, unsigned, void *);
typedef void (*finish_fn)(void);

int main(void)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 640, 400)
            styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        NSView *view = window.contentView;
        view.wantsLayer = YES;
        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.frame = view.bounds;
        layer.contentsScale = 1;
        [view.layer addSublayer:layer];
        [window makeKeyAndOrderFront:nil];

        char error[256];
        if (!lp32_dri_initialize(error, sizeof error)) { fprintf(stderr, "init: %s\n", error); return 1; }
        const void *chosen = NULL;
        for (unsigned i = 0; i < lp32_dri_config_count(); ++i) {
            struct lp32_dri_config_desc d;
            lp32_dri_describe_config(lp32_dri_config_at(i), &d);
            if (d.red == 8 && d.alpha == 8 && d.depth == 24 && d.stencil == 8 && d.double_buffer &&
                !d.samples && !d.float_color) { chosen = lp32_dri_config_at(i); break; }
        }
        printf("configs=%u chosen=%p\n", lp32_dri_config_count(), chosen);
        unsigned cerr = 0;
        void *ctx = lp32_dri_create_context(chosen, NULL, LP32_DRI_API_COMPAT, 2, 1, false, false, &cerr);
        printf("context=%p err=%u\n", ctx, cerr);
        struct lp32_dri_surface surface = {(__bridge void *)layer, 640, 400, 1};
        void *draw = lp32_dri_create_window_drawable(chosen, &surface);
        printf("drawable=%p bind=%d\n", draw, lp32_dri_make_current(ctx, draw, draw));
        get_string_fn get_string = lp32_dri_proc_address("glGetString");
        clear_color_fn clear_color = lp32_dri_proc_address("glClearColor");
        clear_fn clear = lp32_dri_proc_address("glClear");
        read_pixels_fn read_pixels = lp32_dri_proc_address("glReadPixels");
        printf("GL_VENDOR=%s\nGL_RENDERER=%s\nGL_VERSION=%s\nGLSL=%s\n", get_string(0x1F00),
               get_string(0x1F01), get_string(0x1F02), get_string(0x8B8C));
        for (int frame = 0; frame < 120; ++frame) {
            clear_color(frame / 120.0f, 0.25f, 0.5f, 1);
            clear(0x4000 | 0x100);
            if (frame == 119) {
                unsigned char px[4] = {0};
                read_pixels(10, 10, 1, 1, 0x1908, 0x1401, px);
                printf("pixel=%u,%u,%u,%u\n", px[0], px[1], px[2], px[3]);
            }
            lp32_dri_swap_buffers(draw);
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
        }
        lp32_dri_make_current(NULL, NULL, NULL);
        printf("done\n");
    }
    return 0;
}
