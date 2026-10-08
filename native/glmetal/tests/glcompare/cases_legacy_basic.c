/* Legacy profile: clears, primitives, rasterization and per-fragment state. */
#include "glc_gl_legacy.h"

#include "glcompare.h"

/* A triangle and a quad that overlap, drawn in immediate mode. Most
   per-fragment cases draw this scene with the state under test. */
static void overlap_scene(void)
{
    glBegin(GL_TRIANGLES);
    glColor4f(1, 0, 0, 0.75f); glVertex3f(-0.9f, -0.8f, -0.5f);
    glColor4f(0, 1, 0, 0.5f);  glVertex3f(0.8f, -0.9f, 0.2f);
    glColor4f(0, 0, 1, 1);     glVertex3f(0.1f, 0.9f, 0.6f);
    glEnd();
    glBegin(GL_QUADS);
    glColor4f(1, 1, 0, 0.4f);  glVertex3f(-0.5f, -0.5f, 0.0f);
    glColor4f(0, 1, 1, 0.6f);  glVertex3f(0.6f, -0.4f, 0.4f);
    glColor4f(1, 0, 1, 0.8f);  glVertex3f(0.5f, 0.55f, -0.3f);
    glColor4f(1, 1, 1, 0.2f);  glVertex3f(-0.6f, 0.5f, 0.1f);
    glEnd();
}

/* ---- clears -------------------------------------------------------------- */

static const char *const clear_colors[] = {"black", "white", "red", "half", "alpha0", "odd"};
GLC_CASE_VARIANTS(clear_color, clear_colors, .profile = GLC_LEGACY)
{
    static const float colors[][4] = {
        {0, 0, 0, 1}, {1, 1, 1, 1}, {1, 0, 0, 1}, {0.5f, 0.5f, 0.5f, 0.5f}, {0.2f, 0.4f, 0.6f, 0}, {0.1f, 0.33f, 0.77f, 0.9f},
    };
    const float *c = colors[glc_variant];
    glClearColor(c[0], c[1], c[2], c[3]);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLC_CASE(clear_scissor, .profile = GLC_LEGACY)
{
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(10, 20, 30, 17);
    glClearColor(1, 0.5f, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLC_CASE(clear_color_mask, .profile = GLC_LEGACY)
{
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);
    glClearColor(1, 1, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
}

/* ---- primitives ---------------------------------------------------------- */

static const char *const primitive_names[] = {
    "points", "lines", "line_strip", "line_loop", "triangles", "triangle_strip",
    "triangle_fan", "quads", "quad_strip", "polygon",
};
GLC_CASE_VARIANTS(primitive, primitive_names, .profile = GLC_LEGACY)
{
    static const GLenum modes[] = {GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_LINE_LOOP, GL_TRIANGLES,
                                   GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN, GL_QUADS, GL_QUAD_STRIP, GL_POLYGON};
    static const float v[][2] = {{-0.8f, -0.7f}, {-0.3f, 0.6f}, {0.1f, -0.5f}, {0.5f, 0.7f},
                                 {0.8f, -0.6f}, {0.3f, -0.9f}};
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glPointSize(3);
    glBegin(modes[glc_variant]);
    for (int i = 0; i < 6; ++i) {
        glColor3f(i & 1 ? 1.0f : 0.2f, i & 2 ? 1.0f : 0.3f, i & 4 ? 1.0f : 0.4f);
        glVertex2fv(v[i]);
    }
    glEnd();
}

static const char *const point_sizes[] = {"1", "2", "5", "9"};
GLC_CASE_VARIANTS(point_size, point_sizes, .profile = GLC_LEGACY)
{
    static const float sizes[] = {1, 2, 5, 9};
    glClear(GL_COLOR_BUFFER_BIT);
    glPointSize(sizes[glc_variant]);
    glBegin(GL_POINTS);
    for (int i = 0; i < 5; ++i) {
        glColor3f(1, (float)i / 4, 0);
        glVertex2f(-0.8f + 0.4f * i, 0.1f * i - 0.2f);
    }
    glEnd();
}

static const char *const shade_models[] = {"flat", "smooth"};
GLC_CASE_VARIANTS(shade_model, shade_models, .profile = GLC_LEGACY)
{
    glClear(GL_COLOR_BUFFER_BIT);
    glShadeModel(glc_variant ? GL_SMOOTH : GL_FLAT);
    overlap_scene();
}

static const char *const polygon_modes[] = {"fill", "line", "point"};
GLC_CASE_VARIANTS(polygon_mode, polygon_modes, .profile = GLC_LEGACY, .tolerance = 0, .outlier_fraction = 0.01)
{
    static const GLenum modes[] = {GL_FILL, GL_LINE, GL_POINT};
    glClear(GL_COLOR_BUFFER_BIT);
    glPolygonMode(GL_FRONT_AND_BACK, modes[glc_variant]);
    overlap_scene();
}

static const char *const cull_names[] = {"back_ccw", "front_ccw", "back_cw", "both"};
GLC_CASE_VARIANTS(cull_face, cull_names, .profile = GLC_LEGACY)
{
    static const GLenum faces[] = {GL_BACK, GL_FRONT, GL_BACK, GL_FRONT_AND_BACK};
    glClearColor(0.1f, 0.1f, 0.1f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_CULL_FACE);
    glCullFace(faces[glc_variant]);
    glFrontFace(glc_variant == 2 ? GL_CW : GL_CCW);
    overlap_scene();
}

/* ---- per-fragment state -------------------------------------------------- */

static const char *const depth_funcs[] = {"never", "less", "equal", "lequal", "greater", "notequal", "gequal", "always"};
GLC_CASE_VARIANTS(depth_func, depth_funcs, .profile = GLC_LEGACY)
{
    glClearDepth(0.5);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_NEVER + (GLenum)glc_variant);
    overlap_scene();
}

GLC_CASE(depth_mask_and_range, .profile = GLC_LEGACY)
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthRange(0.2, 0.7);
    glDepthMask(GL_FALSE);
    overlap_scene();
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_GREATER);
    glClearDepth(0.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    overlap_scene();
}

static const char *const blend_factor_names[] = {
    "zero", "one", "src_color", "one_minus_src_color", "src_alpha", "one_minus_src_alpha",
    "dst_alpha", "one_minus_dst_alpha", "dst_color", "one_minus_dst_color", "src_alpha_saturate",
    "constant_color", "one_minus_constant_alpha",
};
static const GLenum blend_factors[] = {
    GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
    GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE,
    GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_ALPHA,
};

static void blend_background(void)
{
    glClearColor(0.3f, 0.6f, 0.9f, 0.5f);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLC_CASE_VARIANTS(blend_src, blend_factor_names, .profile = GLC_LEGACY, .tolerance = 1)
{
    blend_background();
    glEnable(GL_BLEND);
    glBlendColor(0.2f, 0.4f, 0.6f, 0.8f);
    glBlendFunc(blend_factors[glc_variant], GL_ONE_MINUS_SRC_ALPHA);
    overlap_scene();
}

GLC_CASE_VARIANTS(blend_dst, blend_factor_names, .profile = GLC_LEGACY, .tolerance = 1)
{
    blend_background();
    glEnable(GL_BLEND);
    glBlendColor(0.2f, 0.4f, 0.6f, 0.8f);
    glBlendFunc(GL_SRC_ALPHA, blend_factors[glc_variant]);
    overlap_scene();
}

static const char *const blend_equations[] = {"add", "subtract", "reverse_subtract", "min", "max"};
GLC_CASE_VARIANTS(blend_equation, blend_equations, .profile = GLC_LEGACY, .tolerance = 1)
{
    static const GLenum equations[] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX};
    blend_background();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glBlendEquation(equations[glc_variant]);
    overlap_scene();
}

GLC_CASE(blend_separate, .profile = GLC_LEGACY, .tolerance = 1)
{
    blend_background();
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);
    overlap_scene();
}

static const char *const alpha_funcs[] = {"never", "less", "equal", "lequal", "greater", "notequal", "gequal", "always"};
GLC_CASE_VARIANTS(alpha_test, alpha_funcs, .profile = GLC_LEGACY)
{
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_NEVER + (GLenum)glc_variant, 0.5f);
    overlap_scene();
}

static const char *const stencil_ops[] = {"keep", "zero", "replace", "incr", "decr", "invert", "incr_wrap", "decr_wrap"};
GLC_CASE_VARIANTS(stencil_op, stencil_ops, .profile = GLC_LEGACY)
{
    static const GLenum ops[] = {GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};
    glClearStencil(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_STENCIL_TEST);
    /* Pass 1 writes stencil with the op under test (twice for overlap). */
    glStencilFunc(GL_ALWAYS, 3, 0xff);
    glStencilOp(GL_KEEP, GL_KEEP, ops[glc_variant]);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    overlap_scene();
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    /* Pass 2 visualizes stencil values 0..4 and 0xfe..0xff as colors. */
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    static const unsigned values[] = {0, 1, 2, 3, 4, 0xfe, 0xff};
    for (int i = 0; i < 7; ++i) {
        glStencilFunc(GL_EQUAL, (GLint)values[i], 0xff);
        glColor3f((i & 1) ? 1.0f : 0.25f, (i & 2) ? 1.0f : 0.25f, (i & 4) ? 1.0f : 0.25f);
        glRectf(-1, -1, 1, 1);
    }
}

static const char *const stencil_funcs[] = {"never", "less", "equal", "lequal", "greater", "notequal", "gequal", "always"};
GLC_CASE_VARIANTS(stencil_func, stencil_funcs, .profile = GLC_LEGACY)
{
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 2, 0xff);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColor3f(0.2f, 0.2f, 0.2f);
    glRectf(-0.6f, -0.6f, 0.2f, 0.7f);
    glStencilFunc(GL_NEVER + (GLenum)glc_variant, 2, 0x3);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    overlap_scene();
}

GLC_CASE(stencil_two_sided, .profile = GLC_LEGACY)
{
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_STENCIL_TEST);
    glStencilFuncSeparate(GL_FRONT, GL_ALWAYS, 1, 0xff);
    glStencilFuncSeparate(GL_BACK, GL_ALWAYS, 2, 0xff);
    glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_REPLACE);
    glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_REPLACE);
    glBegin(GL_TRIANGLES);
    glVertex2f(-0.9f, -0.9f); glVertex2f(0.0f, -0.9f); glVertex2f(-0.45f, 0.8f); /* front */
    glVertex2f(0.1f, -0.9f); glVertex2f(0.55f, 0.8f); glVertex2f(0.9f, -0.9f);  /* back */
    glEnd();
    glStencilFunc(GL_EQUAL, 1, 0xff);
    glColor3f(1, 0, 0);
    glRectf(-1, -1, 1, 1);
    glStencilFunc(GL_EQUAL, 2, 0xff);
    glColor3f(0, 1, 0);
    glRectf(-1, -1, 1, 1);
}

static const char *const logic_ops[] = {"clear", "and", "copy", "xor", "or", "invert", "nand", "set"};
GLC_CASE_VARIANTS(logic_op, logic_ops, .profile = GLC_LEGACY)
{
    static const GLenum ops[] = {GL_CLEAR, GL_AND, GL_COPY, GL_XOR, GL_OR, GL_INVERT, GL_NAND, GL_SET};
    blend_background();
    glEnable(GL_COLOR_LOGIC_OP);
    glLogicOp(ops[glc_variant]);
    overlap_scene();
}

GLC_CASE(polygon_offset, .profile = GLC_LEGACY)
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glColor3f(0, 0, 1);
    glRectf(-0.8f, -0.8f, 0.8f, 0.8f);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1, -2);
    glDepthFunc(GL_LESS);
    glColor3f(1, 1, 0);
    glRectf(-0.5f, -0.5f, 0.9f, 0.5f);
}

GLC_CASE(dither_off_gradient, .profile = GLC_LEGACY)
{
    glDisable(GL_DITHER);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glColor3f(0, 0, 0); glVertex2f(-1, -1);
    glColor3f(1, 0, 0); glVertex2f(1, -1);
    glColor3f(1, 1, 1); glVertex2f(1, 1);
    glColor3f(0, 0, 1); glVertex2f(-1, 1);
    glEnd();
}

GLC_CASE(line_width, .profile = GLC_LEGACY, .outlier_fraction = 0.02)
{
    glClear(GL_COLOR_BUFFER_BIT);
    for (int i = 1; i <= 4; ++i) {
        glLineWidth((float)i);
        glBegin(GL_LINES);
        glColor3f(1, 0.25f * i, 0);
        glVertex2f(-0.9f, -0.8f + 0.35f * i);
        glVertex2f(0.9f, -0.7f + 0.3f * i);
        glEnd();
    }
}

/* ---- transforms ---------------------------------------------------------- */

GLC_CASE(matrix_stack, .profile = GLC_LEGACY)
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1, 1, -1, 1, 1.5, 10);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0, 0, -4);
    for (int i = 0; i < 3; ++i) {
        glPushMatrix();
        glRotatef(35.0f * i + 10, 0.3f, 1, 0.2f);
        glTranslatef(-1.0f + i, 0, 0);
        glScalef(0.6f, 0.6f, 0.6f);
        glBegin(GL_QUADS);
        glColor3f(1, 0, 0); glVertex3f(-1, -1, 1); glVertex3f(1, -1, 1); glVertex3f(1, 1, 1); glVertex3f(-1, 1, 1);
        glColor3f(0, 1, 0); glVertex3f(1, -1, -1); glVertex3f(-1, -1, -1); glVertex3f(-1, 1, -1); glVertex3f(1, 1, -1);
        glColor3f(0, 0, 1); glVertex3f(-1, -1, -1); glVertex3f(-1, -1, 1); glVertex3f(-1, 1, 1); glVertex3f(-1, 1, -1);
        glColor3f(1, 1, 0); glVertex3f(1, -1, 1); glVertex3f(1, -1, -1); glVertex3f(1, 1, -1); glVertex3f(1, 1, 1);
        glEnd();
        glPopMatrix();
    }
}

GLC_CASE(ortho_viewport, .profile = GLC_LEGACY)
{
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 64, 0, 64, -1, 1);
    glViewport(8, 4, 40, 50);
    glColor3f(1, 0.5f, 0.25f);
    glRectf(4, 4, 60, 30);
    glColor3f(0.25f, 0.5f, 1);
    glRectf(10, 20, 30, 62);
}

GLC_CASE(clip_plane, .profile = GLC_LEGACY)
{
    glClear(GL_COLOR_BUFFER_BIT);
    const double plane[4] = {1, 1, 0, 0.2};
    glClipPlane(GL_CLIP_PLANE0, plane);
    glEnable(GL_CLIP_PLANE0);
    overlap_scene();
}
