/* glcompare: pixel comparison cases for OpenGL implementations.
 *
 * A case is a C function that draws into the current framebuffer with plain
 * GL calls. The runner creates a context of the requested profile, binds an
 * offscreen framebuffer of the requested size, calls the case, and saves the
 * RGBA result. The same case library runs against Apple's OpenGL and against
 * GLMetal; compare.py reports where the images differ.
 *
 *   GLC_CASE(clear_red, .profile = GLC_LEGACY) {
 *       glClearColor(1, 0, 0, 1);
 *       glClear(GL_COLOR_BUFFER_BIT);
 *   }
 *
 * Variants run one function several times with a different index:
 *
 *   static const char *const funcs[] = {"never", "less", ...};
 *   GLC_CASE_VARIANTS(depth_func, funcs, .profile = GLC_LEGACY) {
 *       glDepthFunc(GL_NEVER + glc_variant);
 *       ...
 *   }
 */
#ifndef GLCOMPARE_H
#define GLCOMPARE_H

#include <stdbool.h>
#include <stddef.h>

enum glc_profile {
    GLC_LEGACY = 0, /* 2.1, fixed function available */
    GLC_CORE = 1,   /* 3.2+ core (Apple reports 4.1) */
};

struct glc_case {
    const char *name;
    void (*run)(void);
    enum glc_profile profile;
    int width, height;
    /* Largest per-channel difference (0-255) that still counts as a match. */
    int tolerance;
    /* Share of pixels (0-1) allowed to exceed the tolerance, for cases whose
       edges legitimately differ by a pixel. */
    double outlier_fraction;
    int samples;                     /* MSAA samples of the target, 0: none */
    const char *const *variant_names; /* NULL: a single case */
    int variant_count;
};

/* Index of the variant being run (0 for single cases). */
extern int glc_variant;
/* Size of the framebuffer the case draws into. */
extern int glc_width, glc_height;

#define GLC_SECTION __attribute__((used, section("__DATA,__glc_cases")))

#define GLC_CASE(case_name, ...)                                               \
    static void glc_run_##case_name(void);                                     \
    static const struct glc_case glc_case_##case_name GLC_SECTION = {          \
        .name = #case_name, .run = glc_run_##case_name, .width = 64,           \
        .height = 64, __VA_ARGS__};                                            \
    static void glc_run_##case_name(void)

#define GLC_CASE_VARIANTS(case_name, names, ...)                               \
    GLC_CASE(case_name, .variant_names = names,                                \
             .variant_count = (int)(sizeof(names) / sizeof((names)[0])),       \
             __VA_ARGS__)

/* ---- helpers (glc_util.c) ------------------------------------------------ */

/* Compiles and links a GLSL program; aborts the case with a message on
   failure. `attributes` is a NULL-terminated list bound to locations 0.. */
unsigned glc_program(const char *vertex, const char *fragment,
                     const char *const *attributes);
/* Loads an ARB vertex or fragment program into a new program object bound to
   `target`. */
unsigned glc_arb_program(unsigned target, const char *source);
/* Fails the case (the runner records the message and moves on). */
void glc_fail(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));
/* Deterministic test pattern: a WxH RGBA8 image with distinct texels. */
void glc_pattern_rgba8(unsigned char *out, int width, int height, unsigned seed);

#endif
