/* Smaller GL 3.x/4.x entry points: integer, packed and 64-bit generic
 * attributes and their queries, the remaining uniform getters, integer
 * texture parameters, point parameters, ES2 compatibility, program
 * binaries (no formats, like Apple's) and internal format queries. */
#include "glm_internal.h"
#include "programs.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

GLM_HIDDEN void glm_generic_attrib(GLuint index, float x, float y, float z, float w);
/* Implemented in programs.c; the GL headers here predate GL 4.1. */
void glProgramUniform1fv(GLuint, GLint, GLsizei, const GLfloat *);
void glProgramUniform2fv(GLuint, GLint, GLsizei, const GLfloat *);
void glProgramUniform3fv(GLuint, GLint, GLsizei, const GLfloat *);
void glProgramUniform4fv(GLuint, GLint, GLsizei, const GLfloat *);
#define PROGRAM_MATRIX_PROTOTYPE(suffix) void glProgramUniformMatrix##suffix##fv(GLuint, GLint, GLsizei, GLboolean, const GLfloat *);
PROGRAM_MATRIX_PROTOTYPE(2) PROGRAM_MATRIX_PROTOTYPE(3) PROGRAM_MATRIX_PROTOTYPE(4) PROGRAM_MATRIX_PROTOTYPE(2x3)
PROGRAM_MATRIX_PROTOTYPE(2x4) PROGRAM_MATRIX_PROTOTYPE(3x2) PROGRAM_MATRIX_PROTOTYPE(3x4) PROGRAM_MATRIX_PROTOTYPE(4x2)
PROGRAM_MATRIX_PROTOTYPE(4x3)
GLM_HIDDEN void *glm_backend_device(void);
GLM_HIDDEN bool glm_backend_supports_samples(unsigned samples);

/* ---- integer and 64-bit generic attributes -------------------------------- */

/* Current integer values are kept as numbers, like float ones: the vertex
   fetch converts them for integer shader inputs. */
#define ATTRIB_I(suffix, T, n, ...) \
    GLM_EXPORT void glVertexAttrib##suffix(GLuint index, __VA_ARGS__)
GLM_EXPORT void glVertexAttribI1i(GLuint i, GLint x) { glm_generic_attrib(i, (float)x, 0, 0, 1); }
GLM_EXPORT void glVertexAttribI2i(GLuint i, GLint x, GLint y) { glm_generic_attrib(i, (float)x, (float)y, 0, 1); }
GLM_EXPORT void glVertexAttribI3i(GLuint i, GLint x, GLint y, GLint z) { glm_generic_attrib(i, (float)x, (float)y, (float)z, 1); }
GLM_EXPORT void glVertexAttribI4i(GLuint i, GLint x, GLint y, GLint z, GLint w)
{
    glm_generic_attrib(i, (float)x, (float)y, (float)z, (float)w);
}
GLM_EXPORT void glVertexAttribI1ui(GLuint i, GLuint x) { glm_generic_attrib(i, (float)x, 0, 0, 1); }
GLM_EXPORT void glVertexAttribI2ui(GLuint i, GLuint x, GLuint y) { glm_generic_attrib(i, (float)x, (float)y, 0, 1); }
GLM_EXPORT void glVertexAttribI3ui(GLuint i, GLuint x, GLuint y, GLuint z) { glm_generic_attrib(i, (float)x, (float)y, (float)z, 1); }
GLM_EXPORT void glVertexAttribI4ui(GLuint i, GLuint x, GLuint y, GLuint z, GLuint w)
{
    glm_generic_attrib(i, (float)x, (float)y, (float)z, (float)w);
}
GLM_EXPORT void glVertexAttribI1iv(GLuint i, const GLint *v) { glVertexAttribI1i(i, v[0]); }
GLM_EXPORT void glVertexAttribI2iv(GLuint i, const GLint *v) { glVertexAttribI2i(i, v[0], v[1]); }
GLM_EXPORT void glVertexAttribI3iv(GLuint i, const GLint *v) { glVertexAttribI3i(i, v[0], v[1], v[2]); }
GLM_EXPORT void glVertexAttribI4iv(GLuint i, const GLint *v) { glVertexAttribI4i(i, v[0], v[1], v[2], v[3]); }
GLM_EXPORT void glVertexAttribI1uiv(GLuint i, const GLuint *v) { glVertexAttribI1ui(i, v[0]); }
GLM_EXPORT void glVertexAttribI2uiv(GLuint i, const GLuint *v) { glVertexAttribI2ui(i, v[0], v[1]); }
GLM_EXPORT void glVertexAttribI3uiv(GLuint i, const GLuint *v) { glVertexAttribI3ui(i, v[0], v[1], v[2]); }
GLM_EXPORT void glVertexAttribI4uiv(GLuint i, const GLuint *v) { glVertexAttribI4ui(i, v[0], v[1], v[2], v[3]); }
GLM_EXPORT void glVertexAttribI4bv(GLuint i, const GLbyte *v) { glVertexAttribI4i(i, v[0], v[1], v[2], v[3]); }
GLM_EXPORT void glVertexAttribI4sv(GLuint i, const GLshort *v) { glVertexAttribI4i(i, v[0], v[1], v[2], v[3]); }
GLM_EXPORT void glVertexAttribI4ubv(GLuint i, const GLubyte *v) { glVertexAttribI4ui(i, v[0], v[1], v[2], v[3]); }
GLM_EXPORT void glVertexAttribI4usv(GLuint i, const GLushort *v) { glVertexAttribI4ui(i, v[0], v[1], v[2], v[3]); }

/* Doubles become floats: Metal shaders have no 64-bit floats (the shader
   compiler lowers double types to float). */
GLM_EXPORT void glVertexAttribL1d(GLuint i, GLdouble x) { glm_generic_attrib(i, (float)x, 0, 0, 1); }
GLM_EXPORT void glVertexAttribL2d(GLuint i, GLdouble x, GLdouble y) { glm_generic_attrib(i, (float)x, (float)y, 0, 1); }
GLM_EXPORT void glVertexAttribL3d(GLuint i, GLdouble x, GLdouble y, GLdouble z)
{
    glm_generic_attrib(i, (float)x, (float)y, (float)z, 1);
}
GLM_EXPORT void glVertexAttribL4d(GLuint i, GLdouble x, GLdouble y, GLdouble z, GLdouble w)
{
    glm_generic_attrib(i, (float)x, (float)y, (float)z, (float)w);
}
GLM_EXPORT void glVertexAttribL1dv(GLuint i, const GLdouble *v) { glVertexAttribL1d(i, v[0]); }
GLM_EXPORT void glVertexAttribL2dv(GLuint i, const GLdouble *v) { glVertexAttribL2d(i, v[0], v[1]); }
GLM_EXPORT void glVertexAttribL3dv(GLuint i, const GLdouble *v) { glVertexAttribL3d(i, v[0], v[1], v[2]); }
GLM_EXPORT void glVertexAttribL4dv(GLuint i, const GLdouble *v) { glVertexAttribL4d(i, v[0], v[1], v[2], v[3]); }
GLM_EXPORT void glVertexAttribLPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
    GLM_CONTEXT(ctx);
    if (index >= GLM_MAX_ATTRIBS || size < 1 || size > 4 || stride < 0) return glm_error(ctx, GL_INVALID_VALUE);
    if (type != GL_DOUBLE) return glm_error(ctx, GL_INVALID_ENUM);
    if (ctx->profile == GLM_PROFILE_CORE && (!ctx->vao->name || (!ctx->array_buffer && pointer)))
        return glm_error(ctx, GL_INVALID_OPERATION);
    glVertexAttribPointer(index, size, type, GL_FALSE, stride, pointer);
}

/* ---- packed generic attributes (ARB_vertex_type_2_10_10_10_rev) --------- */

static float packed_component(uint32_t bits, int width, bool is_signed, bool normalized)
{
    if (is_signed) {
        int32_t v = (int32_t)(bits << (32 - width)) >> (32 - width);
        if (!normalized) return (float)v;
        float f = (float)v / (float)((1 << (width - 1)) - 1);
        return f < -1 ? -1 : f;
    }
    return normalized ? (float)bits / (float)((1u << width) - 1) : (float)bits;
}

static void packed_attrib(GLuint index, GLenum type, GLboolean normalized, GLuint value, int n)
{
    GLM_CONTEXT(ctx);
    if (type != GL_INT_2_10_10_10_REV && type != GL_UNSIGNED_INT_2_10_10_10_REV) return glm_error(ctx, GL_INVALID_ENUM);
    bool is_signed = type == GL_INT_2_10_10_10_REV;
    float c[4] = {packed_component(value & 1023, 10, is_signed, normalized),
                  packed_component((value >> 10) & 1023, 10, is_signed, normalized),
                  packed_component((value >> 20) & 1023, 10, is_signed, normalized),
                  packed_component(value >> 30, 2, is_signed, normalized)};
    glm_generic_attrib(index, c[0], n > 1 ? c[1] : 0, n > 2 ? c[2] : 0, n > 3 ? c[3] : 1);
}

GLM_EXPORT void glVertexAttribP1ui(GLuint i, GLenum t, GLboolean n, GLuint v) { packed_attrib(i, t, n, v, 1); }
GLM_EXPORT void glVertexAttribP2ui(GLuint i, GLenum t, GLboolean n, GLuint v) { packed_attrib(i, t, n, v, 2); }
GLM_EXPORT void glVertexAttribP3ui(GLuint i, GLenum t, GLboolean n, GLuint v) { packed_attrib(i, t, n, v, 3); }
GLM_EXPORT void glVertexAttribP4ui(GLuint i, GLenum t, GLboolean n, GLuint v) { packed_attrib(i, t, n, v, 4); }
GLM_EXPORT void glVertexAttribP1uiv(GLuint i, GLenum t, GLboolean n, const GLuint *v) { packed_attrib(i, t, n, v[0], 1); }
GLM_EXPORT void glVertexAttribP2uiv(GLuint i, GLenum t, GLboolean n, const GLuint *v) { packed_attrib(i, t, n, v[0], 2); }
GLM_EXPORT void glVertexAttribP3uiv(GLuint i, GLenum t, GLboolean n, const GLuint *v) { packed_attrib(i, t, n, v[0], 3); }
GLM_EXPORT void glVertexAttribP4uiv(GLuint i, GLenum t, GLboolean n, const GLuint *v) { packed_attrib(i, t, n, v[0], 4); }

/* ---- generic attribute queries ------------------------------------------- */

/* Up to 4 values of `pname` for attribute `index`; 0 when unknown. */
static int vertex_attrib_query(struct glm_context *ctx, GLuint index, GLenum pname, double *v)
{
    if (index >= GLM_MAX_ATTRIBS) {
        glm_error(ctx, GL_INVALID_VALUE);
        return 0;
    }
    const struct glm_array *a = &ctx->vao->arrays[index];
    switch (pname) {
    case GL_VERTEX_ATTRIB_ARRAY_ENABLED: v[0] = a->enabled; return 1;
    case GL_VERTEX_ATTRIB_ARRAY_SIZE: v[0] = a->size ? (a->size == GL_BGRA ? GL_BGRA : a->size) : 4; return 1;
    case GL_VERTEX_ATTRIB_ARRAY_STRIDE: v[0] = a->stride; return 1;
    case GL_VERTEX_ATTRIB_ARRAY_TYPE: v[0] = a->type ? a->type : GL_FLOAT; return 1;
    case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED: v[0] = a->normalized; return 1;
    case GL_VERTEX_ATTRIB_ARRAY_INTEGER: v[0] = a->integer; return 1;
    case GL_VERTEX_ATTRIB_ARRAY_DIVISOR: v[0] = a->divisor; return 1;
    case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING: v[0] = a->buffer; return 1;
    case GL_CURRENT_VERTEX_ATTRIB:
        if (index == 0 && ctx->profile == GLM_PROFILE_CORE) {
            glm_error(ctx, GL_INVALID_OPERATION);
            return 0;
        }
        for (int c = 0; c < 4; ++c) v[c] = ctx->state.current[index][c];
        return 4;
    }
    glm_error(ctx, GL_INVALID_ENUM);
    return 0;
}

#define GET_VERTEX_ATTRIB(suffix, T)                                                  \
    GLM_EXPORT void glGetVertexAttrib##suffix(GLuint index, GLenum pname, T *params) \
    {                                                                                 \
        GLM_CONTEXT(ctx);                                                             \
        double v[4];                                                                  \
        int n = vertex_attrib_query(ctx, index, pname, v);                            \
        for (int i = 0; i < n; ++i) params[i] = (T)v[i];                              \
    }
GET_VERTEX_ATTRIB(fv, GLfloat)
GET_VERTEX_ATTRIB(iv, GLint)
GET_VERTEX_ATTRIB(Iiv, GLint)
GET_VERTEX_ATTRIB(Iuiv, GLuint)
GET_VERTEX_ATTRIB(dv, GLdouble)
GET_VERTEX_ATTRIB(Ldv, GLdouble)
GLM_EXPORT void glGetVertexAttribfvARB(GLuint i, GLenum p, GLfloat *v) { glGetVertexAttribfv(i, p, v); }
GLM_EXPORT void glGetVertexAttribivARB(GLuint i, GLenum p, GLint *v) { glGetVertexAttribiv(i, p, v); }
GLM_EXPORT void glGetVertexAttribdvARB(GLuint i, GLenum p, GLdouble *v) { glGetVertexAttribdv(i, p, v); }

GLM_EXPORT void glGetVertexAttribPointerv(GLuint index, GLenum pname, GLvoid **pointer)
{
    GLM_CONTEXT(ctx);
    if (index >= GLM_MAX_ATTRIBS) return glm_error(ctx, GL_INVALID_VALUE);
    if (pname != GL_VERTEX_ATTRIB_ARRAY_POINTER) return glm_error(ctx, GL_INVALID_ENUM);
    *pointer = (GLvoid *)ctx->vao->arrays[index].pointer;
}
GLM_EXPORT void glGetVertexAttribPointervARB(GLuint i, GLenum p, GLvoid **v) { glGetVertexAttribPointerv(i, p, v); }

/* ---- uniform getters and double uniforms ---------------------------------- */

GLM_EXPORT void glGetUniformuiv(GLuint program, GLint location, GLuint *params)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = glm_program_get(ctx, program);
    GLdouble doubles[16];
    if (glm_program_get_double(p, location, doubles)) {
        uint32_t type = p->result.uniforms[p->locations[location].uniform].type;
        int n = type == GL_DOUBLE_VEC4 ? 4 : type == GL_DOUBLE_VEC3 ? 3 : type == GL_DOUBLE_VEC2 ? 2 : 1;
        switch (type) {
        case GL_DOUBLE_MAT2: n = 4; break;
        case GL_DOUBLE_MAT3: n = 9; break;
        case GL_DOUBLE_MAT4: n = 16; break;
        case GL_DOUBLE_MAT2x3: case GL_DOUBLE_MAT3x2: n = 6; break;
        case GL_DOUBLE_MAT2x4: case GL_DOUBLE_MAT4x2: n = 8; break;
        case GL_DOUBLE_MAT3x4: case GL_DOUBLE_MAT4x3: n = 12; break;
        }
        for (int i = 0; i < n; ++i) params[i] = (GLuint)doubles[i];
        return;
    }
    GLint v[16];
    glGetUniformiv(program, location, v);
    if (!p || location < 0 || location >= p->location_count) return;
    uint32_t type = p->result.uniforms[p->locations[location].uniform].type;
    int n = type == GL_UNSIGNED_INT_VEC4 || type == GL_INT_VEC4 ? 4 : type == GL_UNSIGNED_INT_VEC3 || type == GL_INT_VEC3 ? 3
          : type == GL_UNSIGNED_INT_VEC2 || type == GL_INT_VEC2 ? 2 : 1;
    for (int i = 0; i < n; ++i) params[i] = (GLuint)v[i];
}

GLM_EXPORT void glGetUniformdv(GLuint program, GLint location, GLdouble *params)
{
    GLM_CONTEXT(ctx);
    struct glm_program *p = glm_program_get(ctx, program);
    if (glm_program_get_double(p, location, params)) return;
    GLfloat v[16] = {0};
    glGetUniformfv(program, location, v);
    if (!p || location < 0 || location >= p->location_count) return;
    uint32_t type = p->result.uniforms[p->locations[location].uniform].type;
    int n = type == GL_FLOAT_MAT4 ? 16 : 4;
    for (int i = 0; i < n; ++i) params[i] = v[i];
}

/* Genuine double reflection uses raw storage; lowered shaders retain the float path. */
#define UNIFORM_D(n, ...)                                        \
    do {                                                          \
        const double d[] = {__VA_ARGS__};                         \
        GLM_CONTEXT(ctx);                                        \
        if (glm_program_set_double(ctx, glm_program_uniform_target(ctx), location, 1, n, 0, GL_FALSE, d)) return; \
        GLfloat f[4];                                             \
        for (int i = 0; i < n; ++i) f[i] = (GLfloat)d[i];         \
        glUniform##n##fv(location, 1, f);                         \
    } while (0)
GLM_EXPORT void glUniform1d(GLint location, GLdouble x) { UNIFORM_D(1, x); }
GLM_EXPORT void glUniform2d(GLint location, GLdouble x, GLdouble y) { UNIFORM_D(2, x, y); }
GLM_EXPORT void glUniform3d(GLint location, GLdouble x, GLdouble y, GLdouble z) { UNIFORM_D(3, x, y, z); }
GLM_EXPORT void glUniform4d(GLint location, GLdouble x, GLdouble y, GLdouble z, GLdouble w) { UNIFORM_D(4, x, y, z, w); }

static GLfloat *to_floats(const GLdouble *v, size_t n)
{
    GLfloat *f = malloc(n * sizeof *f + 1);
    for (size_t i = 0; i < n; ++i) f[i] = (GLfloat)v[i];
    return f;
}

#define UNIFORM_DV(n)                                                                  \
    GLM_EXPORT void glUniform##n##dv(GLint location, GLsizei count, const GLdouble *v) \
    {                                                                                  \
        GLM_CONTEXT(ctx); \
        if (glm_program_set_double(ctx, glm_program_uniform_target(ctx), location, count, n, 0, GL_FALSE, v)) return; \
        if (count <= 0) return;                                                        \
        GLfloat *f = to_floats(v, (size_t)count * n);                                  \
        glUniform##n##fv(location, count, f);                                          \
        free(f);                                                                       \
    }
UNIFORM_DV(1) UNIFORM_DV(2) UNIFORM_DV(3) UNIFORM_DV(4)

#define UNIFORM_MATRIX_D(suffix, c, r)                                                                         \
    GLM_EXPORT void glUniformMatrix##suffix##dv(GLint location, GLsizei count, GLboolean t, const GLdouble *v) \
    {                                                                                                          \
        GLM_CONTEXT(ctx); \
        if (glm_program_set_double(ctx, glm_program_uniform_target(ctx), location, count, c, r, t, v)) return; \
        if (count <= 0) return;                                                                                \
        GLfloat *f = to_floats(v, (size_t)count * c * r);                                                      \
        glUniformMatrix##suffix##fv(location, count, t, f);                                                    \
        free(f);                                                                                               \
    }
UNIFORM_MATRIX_D(2, 2, 2) UNIFORM_MATRIX_D(3, 3, 3) UNIFORM_MATRIX_D(4, 4, 4) UNIFORM_MATRIX_D(2x3, 2, 3)
UNIFORM_MATRIX_D(2x4, 2, 4) UNIFORM_MATRIX_D(3x2, 3, 2) UNIFORM_MATRIX_D(3x4, 3, 4) UNIFORM_MATRIX_D(4x2, 4, 2)
UNIFORM_MATRIX_D(4x3, 4, 3)

#define PROGRAM_UNIFORM_D(n)                                                                                 \
    GLM_EXPORT void glProgramUniform##n##dv(GLuint program, GLint location, GLsizei count, const GLdouble *v) \
    {                                                                                                        \
        GLM_CONTEXT(ctx); \
        if (glm_program_set_double(ctx, glm_program_get(ctx, program), location, count, n, 0, GL_FALSE, v)) return; \
        if (count <= 0) return;                                                                              \
        GLfloat *f = to_floats(v, (size_t)count * n);                                                        \
        glProgramUniform##n##fv(program, location, count, f);                                                \
        free(f);                                                                                             \
    }
PROGRAM_UNIFORM_D(1) PROGRAM_UNIFORM_D(2) PROGRAM_UNIFORM_D(3) PROGRAM_UNIFORM_D(4)
GLM_EXPORT void glProgramUniform1d(GLuint p, GLint l, GLdouble x) { glProgramUniform1dv(p, l, 1, &x); }
GLM_EXPORT void glProgramUniform2d(GLuint p, GLint l, GLdouble x, GLdouble y)
{
    const GLdouble v[] = {x, y};
    glProgramUniform2dv(p, l, 1, v);
}
GLM_EXPORT void glProgramUniform3d(GLuint p, GLint l, GLdouble x, GLdouble y, GLdouble z)
{
    const GLdouble v[] = {x, y, z};
    glProgramUniform3dv(p, l, 1, v);
}
GLM_EXPORT void glProgramUniform4d(GLuint p, GLint l, GLdouble x, GLdouble y, GLdouble z, GLdouble w)
{
    const GLdouble v[] = {x, y, z, w};
    glProgramUniform4dv(p, l, 1, v);
}
#define PROGRAM_MATRIX_D(suffix, c, r)                                                                              \
    GLM_EXPORT void glProgramUniformMatrix##suffix##dv(GLuint program, GLint location, GLsizei count, GLboolean t, \
                                                       const GLdouble *v)                                           \
    {                                                                                                               \
        GLM_CONTEXT(ctx); \
        if (glm_program_set_double(ctx, glm_program_get(ctx, program), location, count, c, r, t, v)) return; \
        if (count <= 0) return;                                                                                     \
        GLfloat *f = to_floats(v, (size_t)count * c * r);                                                           \
        glProgramUniformMatrix##suffix##fv(program, location, count, t, f);                                         \
        free(f);                                                                                                    \
    }
PROGRAM_MATRIX_D(2, 2, 2) PROGRAM_MATRIX_D(3, 3, 3) PROGRAM_MATRIX_D(4, 4, 4) PROGRAM_MATRIX_D(2x3, 2, 3)
PROGRAM_MATRIX_D(2x4, 2, 4) PROGRAM_MATRIX_D(3x2, 3, 2) PROGRAM_MATRIX_D(3x4, 3, 4) PROGRAM_MATRIX_D(4x2, 4, 2)
PROGRAM_MATRIX_D(4x3, 4, 3)

/* ---- integer texture parameters -------------------------------------------- */

GLM_EXPORT void glTexParameterIiv(GLenum target, GLenum pname, const GLint *params)
{
    if (pname == GL_TEXTURE_BORDER_COLOR) {
        const GLfloat f[4] = {(GLfloat)params[0], (GLfloat)params[1], (GLfloat)params[2], (GLfloat)params[3]};
        return glTexParameterfv(target, pname, f);
    }
    glTexParameteriv(target, pname, params);
}

GLM_EXPORT void glTexParameterIuiv(GLenum target, GLenum pname, const GLuint *params)
{
    if (pname == GL_TEXTURE_BORDER_COLOR) {
        const GLfloat f[4] = {(GLfloat)params[0], (GLfloat)params[1], (GLfloat)params[2], (GLfloat)params[3]};
        return glTexParameterfv(target, pname, f);
    }
    GLint i[4] = {(GLint)params[0]};
    if (pname == GL_TEXTURE_SWIZZLE_RGBA)
        for (int k = 1; k < 4; ++k) i[k] = (GLint)params[k];
    glTexParameteriv(target, pname, i);
}

GLM_EXPORT void glGetTexParameterIiv(GLenum target, GLenum pname, GLint *params)
{
    if (pname == GL_TEXTURE_BORDER_COLOR) {
        GLfloat f[4];
        glGetTexParameterfv(target, pname, f);
        for (int i = 0; i < 4; ++i) params[i] = (GLint)f[i];
        return;
    }
    glGetTexParameteriv(target, pname, params);
}

GLM_EXPORT void glGetTexParameterIuiv(GLenum target, GLenum pname, GLuint *params)
{
    GLint v[4] = {0};
    glGetTexParameterIiv(target, pname, v);
    for (int i = 0; i < (pname == GL_TEXTURE_BORDER_COLOR || pname == GL_TEXTURE_SWIZZLE_RGBA ? 4 : 1); ++i)
        params[i] = (GLuint)v[i];
}

/* ---- buffers, colour clamping ---------------------------------------------- */

GLM_EXPORT void glGetBufferParameteri64v(GLenum target, GLenum pname, GLint64 *params)
{
    GLint v = 0;
    glGetBufferParameteriv(target, pname, &v);
    *params = v;
}

GLM_EXPORT void glClampColor(GLenum target, GLenum clamp)
{
    GLM_CONTEXT(ctx);
    if (target != GL_CLAMP_READ_COLOR) return glm_error(ctx, GL_INVALID_ENUM);
    if (clamp != GL_TRUE && clamp != GL_FALSE && clamp != GL_FIXED_ONLY) return glm_error(ctx, GL_INVALID_ENUM);
    ctx->clamp_read_color = clamp;
}
GLM_EXPORT void glClampColorARB(GLenum target, GLenum clamp) { glClampColor(target, clamp); }

/* ---- point parameters (GL 1.4) -------------------------------------------- */

GLM_EXPORT void glPointParameterfv(GLenum pname, const GLfloat *v)
{
    GLM_CONTEXT(ctx);
    switch (pname) {
    case GL_POINT_SIZE_MIN: if (v[0] < 0) return glm_error(ctx, GL_INVALID_VALUE); ctx->point_size_min = v[0]; break;
    case GL_POINT_SIZE_MAX: if (v[0] < 0) return glm_error(ctx, GL_INVALID_VALUE); ctx->point_size_max = v[0]; break;
    case GL_POINT_FADE_THRESHOLD_SIZE:
        if (v[0] < 0) return glm_error(ctx, GL_INVALID_VALUE);
        ctx->point_fade_threshold = v[0];
        break;
    case GL_POINT_DISTANCE_ATTENUATION: memcpy(ctx->point_distance, v, sizeof ctx->point_distance); break;
    case GL_POINT_SPRITE_COORD_ORIGIN:
        if (v[0] != GL_LOWER_LEFT && v[0] != GL_UPPER_LEFT) return glm_error(ctx, GL_INVALID_ENUM);
        ctx->point_sprite_origin = (GLenum)v[0];
        break;
    default: return glm_error(ctx, GL_INVALID_ENUM);
    }
}
GLM_EXPORT void glPointParameterf(GLenum pname, GLfloat v) { glPointParameterfv(pname, &v); }
GLM_EXPORT void glPointParameteri(GLenum pname, GLint v)
{
    GLfloat f = (GLfloat)v;
    glPointParameterfv(pname, &f);
}
GLM_EXPORT void glPointParameteriv(GLenum pname, const GLint *v)
{
    GLfloat f[3] = {(GLfloat)v[0], 0, 0};
    if (pname == GL_POINT_DISTANCE_ATTENUATION) {
        f[1] = (GLfloat)v[1];
        f[2] = (GLfloat)v[2];
    }
    glPointParameterfv(pname, f);
}
GLM_EXPORT void glPointParameterfARB(GLenum p, GLfloat v) { glPointParameterf(p, v); }
GLM_EXPORT void glPointParameterfvARB(GLenum p, const GLfloat *v) { glPointParameterfv(p, v); }
GLM_EXPORT void glPointParameterfEXT(GLenum p, GLfloat v) { glPointParameterf(p, v); }
GLM_EXPORT void glPointParameterfvEXT(GLenum p, const GLfloat *v) { glPointParameterfv(p, v); }
GLM_EXPORT void glPointParameteriNV(GLenum p, GLint v) { glPointParameteri(p, v); }
GLM_EXPORT void glPointParameterivNV(GLenum p, const GLint *v) { glPointParameteriv(p, v); }

/* ---- ES2 compatibility and program binaries ------------------------------- */

GLM_EXPORT void glReleaseShaderCompiler(void) {}

GLM_EXPORT void glShaderBinary(GLsizei count, const GLuint *shaders, GLenum format, const GLvoid *binary, GLsizei length)
{
    GLM_CONTEXT(ctx);
    (void)count; (void)shaders; (void)format; (void)binary; (void)length;
    glm_error(ctx, GL_INVALID_ENUM); /* GL_NUM_SHADER_BINARY_FORMATS is 0 */
}

GLM_EXPORT void glGetShaderPrecisionFormat(GLenum shader, GLenum precision, GLint *range, GLint *digits)
{
    GLM_CONTEXT(ctx);
    if (shader != GL_VERTEX_SHADER && shader != GL_FRAGMENT_SHADER) return glm_error(ctx, GL_INVALID_ENUM);
    switch (precision) {
    case GL_LOW_FLOAT: case GL_MEDIUM_FLOAT: case GL_HIGH_FLOAT:
        range[0] = range[1] = 127;
        *digits = 23;
        break;
    case GL_LOW_INT: case GL_MEDIUM_INT: case GL_HIGH_INT:
        range[0] = range[1] = 31;
        *digits = 0;
        break;
    default: glm_error(ctx, GL_INVALID_ENUM);
    }
}

/* No binary formats (GL_NUM_PROGRAM_BINARY_FORMATS is 0): nothing can be
   retrieved or loaded. */
GLM_EXPORT void glGetProgramBinary(GLuint program, GLsizei size, GLsizei *length, GLenum *format, GLvoid *binary)
{
    GLM_CONTEXT(ctx);
    (void)size; (void)format; (void)binary;
    if (!glm_program_get(ctx, program)) return glm_error(ctx, GL_INVALID_VALUE);
    if (length) *length = 0;
    glm_error(ctx, GL_INVALID_OPERATION);
}

GLM_EXPORT void glProgramBinary(GLuint program, GLenum format, const GLvoid *binary, GLsizei length)
{
    GLM_CONTEXT(ctx);
    (void)format; (void)binary; (void)length;
    struct glm_program *p = glm_program_get(ctx, program);
    if (!p) return glm_error(ctx, GL_INVALID_VALUE);
    p->linked = false;
    glm_error(ctx, GL_INVALID_ENUM);
}

/* ---- internal format queries (ARB_internalformat_query) -------------------- */

GLM_EXPORT void glGetInternalformativ(GLenum target, GLenum internal, GLenum pname, GLsizei size, GLint *params)
{
    GLM_CONTEXT(ctx);
    if (target != GL_RENDERBUFFER && target != GL_TEXTURE_2D_MULTISAMPLE && target != GL_TEXTURE_2D_MULTISAMPLE_ARRAY)
        return glm_error(ctx, GL_INVALID_ENUM);
    struct glm_format_info info;
    if (!glm_format_lookup(internal, &info)) return glm_error(ctx, GL_INVALID_ENUM);
    GLint counts[4];
    int n = 0;
    for (unsigned samples = 8; samples >= 2; samples /= 2)
        if (glm_backend_supports_samples(samples)) counts[n++] = (GLint)samples;
    if (pname == GL_NUM_SAMPLE_COUNTS) {
        if (size > 0) params[0] = n;
    } else if (pname == GL_SAMPLES) {
        for (int i = 0; i < n && i < size; ++i) params[i] = counts[i];
    } else {
        glm_error(ctx, GL_INVALID_ENUM);
    }
}
