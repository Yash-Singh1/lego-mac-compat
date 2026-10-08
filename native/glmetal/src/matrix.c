/* Legacy matrix stacks. Matrices are column-major float[16], as in GL. */
#include "glm_internal.h"

#include <math.h>
#include <string.h>

void glm_mat4_identity(float *m)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1;
}

void glm_mat4_multiply(float *out, const float *a, const float *b)
{
    float r[16];
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            r[c * 4 + row] = a[row] * b[c * 4] + a[4 + row] * b[c * 4 + 1] + a[8 + row] * b[c * 4 + 2] +
                             a[12 + row] * b[c * 4 + 3];
    memcpy(out, r, sizeof r);
}

bool glm_mat4_invert(float *out, const float *m)
{
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0) {
        glm_mat4_identity(out);
        return false;
    }
    for (int i = 0; i < 16; ++i) out[i] = inv[i] / det;
    return true;
}

struct glm_matrix_stack *glm_matrix_stack(struct glm_context *ctx)
{
    struct glm_state *s = &ctx->state;
    /* Texture matrices exist for GL_MAX_TEXTURE_COORDS (8) units. */
    if (s->matrix_mode == GL_TEXTURE && s->active_texture - GL_TEXTURE0 >= 8) {
        glm_error(ctx, GL_INVALID_OPERATION);
        ctx->invalid_stack.depth = 0;
        return &ctx->invalid_stack;
    }
    switch (s->matrix_mode) {
    case GL_PROJECTION: return &s->projection;
    case GL_TEXTURE: return &s->texture[s->active_texture - GL_TEXTURE0];
    case GL_COLOR: return &s->color;
    default: return &s->modelview;
    }
}

float *glm_matrix_top(struct glm_context *ctx)
{
    struct glm_matrix_stack *stack = glm_matrix_stack(ctx);
    return stack->m[stack->depth];
}

static void multiply_current(struct glm_context *ctx, const float *m)
{
    float *top = glm_matrix_top(ctx);
    glm_mat4_multiply(top, top, m);
}

GLM_EXPORT void glMatrixMode(GLenum mode)
{
    GLM_CONTEXT(ctx);
    if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE && mode != GL_COLOR)
        return glm_error(ctx, GL_INVALID_ENUM);
    ctx->state.matrix_mode = mode;
}

GLM_EXPORT void glLoadIdentity(void) { GLM_CONTEXT(ctx); glm_mat4_identity(glm_matrix_top(ctx)); }

GLM_EXPORT void glLoadMatrixf(const GLfloat *m) { GLM_CONTEXT(ctx); memcpy(glm_matrix_top(ctx), m, 16 * sizeof(float)); }

GLM_EXPORT void glLoadMatrixd(const GLdouble *m)
{
    GLM_CONTEXT(ctx);
    float *top = glm_matrix_top(ctx);
    for (int i = 0; i < 16; ++i) top[i] = (float)m[i];
}

GLM_EXPORT void glMultMatrixf(const GLfloat *m) { GLM_CONTEXT(ctx); multiply_current(ctx, m); }

GLM_EXPORT void glMultMatrixd(const GLdouble *m)
{
    GLM_CONTEXT(ctx);
    float f[16];
    for (int i = 0; i < 16; ++i) f[i] = (float)m[i];
    multiply_current(ctx, f);
}

static void transpose(float *out, const float *in)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[c * 4 + r] = in[r * 4 + c];
}

GLM_EXPORT void glLoadTransposeMatrixf(const GLfloat *m) { float t[16]; transpose(t, m); glLoadMatrixf(t); }
GLM_EXPORT void glMultTransposeMatrixf(const GLfloat *m) { float t[16]; transpose(t, m); glMultMatrixf(t); }
GLM_EXPORT void glLoadTransposeMatrixd(const GLdouble *m)
{
    float f[16], t[16];
    for (int i = 0; i < 16; ++i) f[i] = (float)m[i];
    transpose(t, f);
    glLoadMatrixf(t);
}
GLM_EXPORT void glMultTransposeMatrixd(const GLdouble *m)
{
    float f[16], t[16];
    for (int i = 0; i < 16; ++i) f[i] = (float)m[i];
    transpose(t, f);
    glMultMatrixf(t);
}

GLM_EXPORT void glPushMatrix(void)
{
    GLM_CONTEXT(ctx);
    struct glm_matrix_stack *stack = glm_matrix_stack(ctx);
    if (stack->depth + 1 >= GLM_MATRIX_STACK_DEPTH) return glm_error(ctx, GL_STACK_OVERFLOW);
    memcpy(stack->m[stack->depth + 1], stack->m[stack->depth], 16 * sizeof(float));
    ++stack->depth;
}

GLM_EXPORT void glPopMatrix(void)
{
    GLM_CONTEXT(ctx);
    struct glm_matrix_stack *stack = glm_matrix_stack(ctx);
    if (stack->depth == 0) return glm_error(ctx, GL_STACK_UNDERFLOW);
    --stack->depth;
}

GLM_EXPORT void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
    GLM_CONTEXT(ctx);
    float m[16];
    glm_mat4_identity(m);
    m[12] = x; m[13] = y; m[14] = z;
    multiply_current(ctx, m);
}
GLM_EXPORT void glTranslated(GLdouble x, GLdouble y, GLdouble z) { glTranslatef((float)x, (float)y, (float)z); }

GLM_EXPORT void glScalef(GLfloat x, GLfloat y, GLfloat z)
{
    GLM_CONTEXT(ctx);
    float m[16];
    glm_mat4_identity(m);
    m[0] = x; m[5] = y; m[10] = z;
    multiply_current(ctx, m);
}
GLM_EXPORT void glScaled(GLdouble x, GLdouble y, GLdouble z) { glScalef((float)x, (float)y, (float)z); }

GLM_EXPORT void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
    GLM_CONTEXT(ctx);
    float length = sqrtf(x * x + y * y + z * z);
    if (length == 0) return;
    x /= length; y /= length; z /= length;
    float radians = angle * (float)M_PI / 180.0f;
    float c = cosf(radians), s = sinf(radians), t = 1 - c;
    float m[16] = {
        x * x * t + c,     y * x * t + z * s, z * x * t - y * s, 0,
        x * y * t - z * s, y * y * t + c,     z * y * t + x * s, 0,
        x * z * t + y * s, y * z * t - x * s, z * z * t + c,     0,
        0, 0, 0, 1,
    };
    multiply_current(ctx, m);
}
GLM_EXPORT void glRotated(GLdouble angle, GLdouble x, GLdouble y, GLdouble z) { glRotatef((float)angle, (float)x, (float)y, (float)z); }

GLM_EXPORT void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    GLM_CONTEXT(ctx);
    if (n <= 0 || f <= 0 || l == r || b == t || n == f) return glm_error(ctx, GL_INVALID_VALUE);
    float m[16] = {0};
    m[0] = (float)(2 * n / (r - l));
    m[5] = (float)(2 * n / (t - b));
    m[8] = (float)((r + l) / (r - l));
    m[9] = (float)((t + b) / (t - b));
    m[10] = (float)(-(f + n) / (f - n));
    m[11] = -1;
    m[14] = (float)(-2 * f * n / (f - n));
    multiply_current(ctx, m);
}

GLM_EXPORT void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    GLM_CONTEXT(ctx);
    if (l == r || b == t || n == f) return glm_error(ctx, GL_INVALID_VALUE);
    float m[16] = {0};
    m[0] = (float)(2 / (r - l));
    m[5] = (float)(2 / (t - b));
    m[10] = (float)(-2 / (f - n));
    m[12] = (float)(-(r + l) / (r - l));
    m[13] = (float)(-(t + b) / (t - b));
    m[14] = (float)(-(f + n) / (f - n));
    m[15] = 1;
    multiply_current(ctx, m);
}
