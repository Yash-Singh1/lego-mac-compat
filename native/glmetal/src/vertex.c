/* Vertex specification: immediate mode, client arrays and buffer arrays,
 * attribute fetch, and conversion of GL primitives into ones Metal draws.
 *
 * Every draw is resolved on the CPU into a float4-per-attribute vertex
 * stream plus a 32-bit index list. The backend then draws that stream with a
 * single vertex layout, so client arrays, immediate mode and every GL vertex
 * format share one path. */
#include "glm_internal.h"
#include "programs.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <limits.h>

/* Metal primitive types (MTLPrimitiveType). */
enum { MTL_POINT = 0, MTL_LINE = 1, MTL_LINE_STRIP = 2, MTL_TRIANGLE = 3, MTL_TRIANGLE_STRIP = 4 };

struct glm_begin_end {
    GLenum mode;
    float *vertices; /* GLM_MAX_ATTRIBS float4 per vertex */
    uint32_t count, capacity;
};

/* ---- current attributes -------------------------------------------------- */

static void emit_vertex(struct glm_context *ctx, float x, float y, float z, float w);

GLM_HIDDEN void glm_attrib(int slot, float x, float y, float z, float w)
{
    GLM_CONTEXT(ctx);
    if (slot == GLM_ATTR_POSITION) {
        if (ctx->immediate) emit_vertex(ctx, x, y, z, w);
        return;
    }
    float *c = ctx->state.current[slot];
    c[0] = x; c[1] = y; c[2] = z; c[3] = w;
}

GLM_HIDDEN void glm_generic_attrib(GLuint index, float x, float y, float z, float w)
{
    GLM_CONTEXT(ctx);
    if (index >= GLM_MAX_ATTRIBS) return glm_error(ctx, GL_INVALID_VALUE);
    glm_attrib((int)index, x, y, z, w);
}

/* ---- immediate mode ------------------------------------------------------ */

static bool valid_mode(GLenum mode) { return mode <= GL_TRIANGLE_STRIP_ADJACENCY || mode == GL_PATCHES; }

GLM_EXPORT void glBegin(GLenum mode)
{
    GLM_CONTEXT(ctx);
    if (ctx->immediate) return glm_error(ctx, GL_INVALID_OPERATION);
    if (!valid_mode(mode) || mode == GL_PATCHES) return glm_error(ctx, GL_INVALID_ENUM);
    static __thread struct glm_begin_end storage;
    storage.mode = mode;
    storage.count = 0;
    ctx->immediate = &storage;
}

static void emit_vertex(struct glm_context *ctx, float x, float y, float z, float w)
{
    struct glm_begin_end *im = ctx->immediate;
    if (im->count == im->capacity) {
        im->capacity = im->capacity ? im->capacity * 2 : 256;
        im->vertices = realloc(im->vertices, (size_t)im->capacity * GLM_MAX_ATTRIBS * 4 * sizeof(float));
    }
    float *v = im->vertices + (size_t)im->count * GLM_MAX_ATTRIBS * 4;
    memcpy(v, ctx->state.current, sizeof ctx->state.current);
    v[0] = x; v[1] = y; v[2] = z; v[3] = w;
    /* Fog coordinates live in the fog slot's x like other attributes. */
    ++im->count;
}

static void submit(struct glm_context *ctx, GLenum mode, float *vertices, uint32_t vertex_count,
                   const uint32_t *indices, uint32_t index_count, uint32_t instance, uint32_t vertex_id_base);

GLM_EXPORT void glEnd(void)
{
    GLM_CONTEXT(ctx);
    struct glm_begin_end *im = ctx->immediate;
    if (!im) return glm_error(ctx, GL_INVALID_OPERATION);
    ctx->immediate = NULL;
    if (im->count && !ctx->conditional_discard) submit(ctx, im->mode, im->vertices, im->count, NULL, 0, 0, 0);
}

GLM_EXPORT void glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2)
{
    glBegin(GL_POLYGON);
    glVertex2f(x1, y1);
    glVertex2f(x2, y1);
    glVertex2f(x2, y2);
    glVertex2f(x1, y2);
    glEnd();
}
GLM_EXPORT void glRectd(GLdouble x1, GLdouble y1, GLdouble x2, GLdouble y2) { glRectf((float)x1, (float)y1, (float)x2, (float)y2); }
GLM_EXPORT void glRecti(GLint x1, GLint y1, GLint x2, GLint y2) { glRectf((float)x1, (float)y1, (float)x2, (float)y2); }
GLM_EXPORT void glRects(GLshort x1, GLshort y1, GLshort x2, GLshort y2) { glRectf(x1, y1, x2, y2); }
GLM_EXPORT void glRectfv(const GLfloat *a, const GLfloat *b) { glRectf(a[0], a[1], b[0], b[1]); }
GLM_EXPORT void glRectdv(const GLdouble *a, const GLdouble *b) { glRectd(a[0], a[1], b[0], b[1]); }
GLM_EXPORT void glRectiv(const GLint *a, const GLint *b) { glRecti(a[0], a[1], b[0], b[1]); }
GLM_EXPORT void glRectsv(const GLshort *a, const GLshort *b) { glRects(a[0], a[1], b[0], b[1]); }

/* ---- array state --------------------------------------------------------- */

static void set_array(struct glm_context *ctx, int slot, GLint size, GLenum type, GLboolean normalized,
                      GLsizei stride, const void *pointer, bool integer)
{
    if (stride < 0) return glm_error(ctx, GL_INVALID_VALUE);
    struct glm_array *a = &ctx->vao->arrays[slot];
    glm_vao_changed(ctx->vao);
    a->size = size;
    a->type = type;
    a->normalized = normalized;
    a->integer = integer;
    a->stride = stride;
    a->pointer = pointer;
    a->buffer = ctx->array_buffer;
}

GLM_EXPORT void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{ GLM_CONTEXT(ctx); set_array(ctx, GLM_ATTR_POSITION, size, type, GL_FALSE, stride, pointer, false); }
GLM_EXPORT void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *pointer)
{ GLM_CONTEXT(ctx); set_array(ctx, GLM_ATTR_NORMAL, 3, type, GL_TRUE, stride, pointer, false); }
GLM_EXPORT void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{ GLM_CONTEXT(ctx); set_array(ctx, GLM_ATTR_COLOR, size, type, GL_TRUE, stride, pointer, false); }
GLM_EXPORT void glSecondaryColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{ GLM_CONTEXT(ctx); set_array(ctx, GLM_ATTR_SECONDARY_COLOR, size, type, GL_TRUE, stride, pointer, false); }
GLM_EXPORT void glSecondaryColorPointerEXT(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{ glSecondaryColorPointer(size, type, stride, pointer); }
GLM_EXPORT void glFogCoordPointer(GLenum type, GLsizei stride, const GLvoid *pointer)
{ GLM_CONTEXT(ctx); set_array(ctx, GLM_ATTR_FOG, 1, type, GL_FALSE, stride, pointer, false); }
GLM_EXPORT void glFogCoordPointerEXT(GLenum type, GLsizei stride, const GLvoid *pointer) { glFogCoordPointer(type, stride, pointer); }
GLM_EXPORT void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
    GLM_CONTEXT(ctx);
    set_array(ctx, GLM_ATTR_TEX0 + (int)((ctx->vao->client_active_texture - GL_TEXTURE0) & 7), size, type, GL_FALSE,
              stride, pointer, false);
}

GLM_EXPORT void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
                                      const GLvoid *pointer)
{
    GLM_CONTEXT(ctx);
    if (index >= GLM_MAX_ATTRIBS) return glm_error(ctx, GL_INVALID_VALUE);
    set_array(ctx, (int)index, size, type, normalized, stride, pointer, false);
}
GLM_EXPORT void glVertexAttribPointerARB(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
                                         const GLvoid *pointer)
{ glVertexAttribPointer(index, size, type, normalized, stride, pointer); }

GLM_EXPORT void glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
    GLM_CONTEXT(ctx);
    if (index >= GLM_MAX_ATTRIBS) return glm_error(ctx, GL_INVALID_VALUE);
    set_array(ctx, (int)index, size, type, GL_FALSE, stride, pointer, true);
}

GLM_EXPORT void glVertexAttribDivisor(GLuint index, GLuint divisor)
{
    GLM_CONTEXT(ctx);
    if (index >= GLM_MAX_ATTRIBS) return glm_error(ctx, GL_INVALID_VALUE);
    ctx->vao->arrays[index].divisor = divisor;
    glm_vao_changed(ctx->vao);
}
GLM_EXPORT void glVertexAttribDivisorARB(GLuint index, GLuint divisor) { glVertexAttribDivisor(index, divisor); }

GLM_HIDDEN int glm_client_array_slot(struct glm_context *ctx, GLenum array)
{
    switch (array) {
    case GL_VERTEX_ARRAY: return GLM_ATTR_POSITION;
    case GL_NORMAL_ARRAY: return GLM_ATTR_NORMAL;
    case GL_COLOR_ARRAY: return GLM_ATTR_COLOR;
    case GL_SECONDARY_COLOR_ARRAY: return GLM_ATTR_SECONDARY_COLOR;
    case GL_FOG_COORD_ARRAY: return GLM_ATTR_FOG;
    case GL_TEXTURE_COORD_ARRAY: return GLM_ATTR_TEX0 + (int)((ctx->vao->client_active_texture - GL_TEXTURE0) & 7);
    default: return -1;
    }
}

GLM_EXPORT void glEnableClientState(GLenum array)
{
    GLM_CONTEXT(ctx);
    int slot = glm_client_array_slot(ctx, array);
    if (slot < 0) {
        if (array == GL_EDGE_FLAG_ARRAY || array == GL_INDEX_ARRAY || array == GL_ELEMENT_ARRAY_APPLE) return;
        return glm_error(ctx, GL_INVALID_ENUM);
    }
    ctx->vao->arrays[slot].enabled = true;
    glm_vao_changed(ctx->vao);
}

GLM_EXPORT void glDisableClientState(GLenum array)
{
    GLM_CONTEXT(ctx);
    int slot = glm_client_array_slot(ctx, array);
    if (slot < 0) {
        if (array == GL_EDGE_FLAG_ARRAY || array == GL_INDEX_ARRAY || array == GL_ELEMENT_ARRAY_APPLE) return;
        return glm_error(ctx, GL_INVALID_ENUM);
    }
    ctx->vao->arrays[slot].enabled = false;
    glm_vao_changed(ctx->vao);
}

GLM_EXPORT void glEnableVertexAttribArray(GLuint index)
{
    GLM_CONTEXT(ctx);
    if (index >= GLM_MAX_ATTRIBS) return glm_error(ctx, GL_INVALID_VALUE);
    ctx->vao->arrays[index].enabled = true;
    glm_vao_changed(ctx->vao);
}
GLM_EXPORT void glEnableVertexAttribArrayARB(GLuint index) { glEnableVertexAttribArray(index); }

GLM_EXPORT void glDisableVertexAttribArray(GLuint index)
{
    GLM_CONTEXT(ctx);
    if (index >= GLM_MAX_ATTRIBS) return glm_error(ctx, GL_INVALID_VALUE);
    ctx->vao->arrays[index].enabled = false;
    glm_vao_changed(ctx->vao);
}
GLM_EXPORT void glDisableVertexAttribArrayARB(GLuint index) { glDisableVertexAttribArray(index); }

GLM_EXPORT void glClientActiveTexture(GLenum texture)
{
    GLM_CONTEXT(ctx);
    if (texture < GL_TEXTURE0 || texture >= GL_TEXTURE0 + 8) return glm_error(ctx, GL_INVALID_ENUM);
    ctx->vao->client_active_texture = texture;
}
GLM_EXPORT void glClientActiveTextureARB(GLenum texture) { glClientActiveTexture(texture); }

/* Edge flags only matter for GL_LINE/GL_POINT polygon modes, whose outlines
   are drawn whole. */
GLM_EXPORT void glEdgeFlag(GLboolean flag) { (void)flag; }
GLM_EXPORT void glEdgeFlagv(const GLboolean *flag) { (void)flag; }
GLM_EXPORT void glEdgeFlagPointer(GLsizei stride, const GLvoid *pointer) { (void)stride, (void)pointer; }

GLM_EXPORT void glGetPointerv(GLenum pname, GLvoid **params)
{
    GLM_CONTEXT(ctx);
    const struct glm_array *arrays = ctx->vao->arrays;
    int texture = GLM_ATTR_TEX0 + (int)((ctx->vao->client_active_texture - GL_TEXTURE0) & 7);
    switch (pname) {
    case GL_VERTEX_ARRAY_POINTER: *params = (GLvoid *)arrays[GLM_ATTR_POSITION].pointer; break;
    case GL_NORMAL_ARRAY_POINTER: *params = (GLvoid *)arrays[GLM_ATTR_NORMAL].pointer; break;
    case GL_COLOR_ARRAY_POINTER: *params = (GLvoid *)arrays[GLM_ATTR_COLOR].pointer; break;
    case GL_SECONDARY_COLOR_ARRAY_POINTER: *params = (GLvoid *)arrays[GLM_ATTR_SECONDARY_COLOR].pointer; break;
    case GL_FOG_COORD_ARRAY_POINTER: *params = (GLvoid *)arrays[GLM_ATTR_FOG].pointer; break;
    case GL_TEXTURE_COORD_ARRAY_POINTER: *params = (GLvoid *)arrays[texture].pointer; break;
    case GL_EDGE_FLAG_ARRAY_POINTER: case GL_INDEX_ARRAY_POINTER: case GL_FEEDBACK_BUFFER_POINTER:
    case GL_SELECTION_BUFFER_POINTER: *params = NULL; break;
    case GL_ELEMENT_ARRAY_POINTER_APPLE: *params = (GLvoid *)ctx->apple_element_pointer; break;
    default: glm_error(ctx, GL_INVALID_ENUM);
    }
}

/* GL 2.1 2.8: the interleaved formats as individual array calls. */
GLM_EXPORT void glInterleavedArrays(GLenum format, GLsizei stride, const GLvoid *pointer)
{
    GLM_CONTEXT(ctx);
    enum { F = sizeof(GLfloat), C = 4 * sizeof(GLubyte) };
    /* texcoord size, colour size, colour type, vertex size, normal, colour offset, normal offset, vertex offset, stride */
    int tc = 0, cc = 0, vc = 0, pc, pn, pv, s;
    GLenum ctype = GL_FLOAT;
    bool normal = false;
    switch (format) {
    case GL_V2F: vc = 2; pc = pn = 0; pv = 0; s = 2 * F; break;
    case GL_V3F: vc = 3; pc = pn = 0; pv = 0; s = 3 * F; break;
    case GL_C4UB_V2F: cc = 4; ctype = GL_UNSIGNED_BYTE; vc = 2; pc = 0; pn = 0; pv = C; s = C + 2 * F; break;
    case GL_C4UB_V3F: cc = 4; ctype = GL_UNSIGNED_BYTE; vc = 3; pc = 0; pn = 0; pv = C; s = C + 3 * F; break;
    case GL_C3F_V3F: cc = 3; vc = 3; pc = 0; pn = 0; pv = 3 * F; s = 6 * F; break;
    case GL_N3F_V3F: normal = true; vc = 3; pc = 0; pn = 0; pv = 3 * F; s = 6 * F; break;
    case GL_C4F_N3F_V3F: cc = 4; normal = true; vc = 3; pc = 0; pn = 4 * F; pv = 7 * F; s = 10 * F; break;
    case GL_T2F_V3F: tc = 2; vc = 3; pc = pn = 0; pv = 2 * F; s = 5 * F; break;
    case GL_T4F_V4F: tc = 4; vc = 4; pc = pn = 0; pv = 4 * F; s = 8 * F; break;
    case GL_T2F_C4UB_V3F: tc = 2; cc = 4; ctype = GL_UNSIGNED_BYTE; vc = 3; pc = 2 * F; pn = 0; pv = C + 2 * F; s = C + 5 * F; break;
    case GL_T2F_C3F_V3F: tc = 2; cc = 3; vc = 3; pc = 2 * F; pn = 0; pv = 5 * F; s = 8 * F; break;
    case GL_T2F_N3F_V3F: tc = 2; normal = true; vc = 3; pc = 0; pn = 2 * F; pv = 5 * F; s = 8 * F; break;
    case GL_T2F_C4F_N3F_V3F: tc = 2; cc = 4; normal = true; vc = 3; pc = 2 * F; pn = 6 * F; pv = 9 * F; s = 12 * F; break;
    case GL_T4F_C4F_N3F_V4F: tc = 4; cc = 4; normal = true; vc = 4; pc = 4 * F; pn = 8 * F; pv = 11 * F; s = 15 * F; break;
    default: return glm_error(ctx, GL_INVALID_ENUM);
    }
    if (stride < 0) return glm_error(ctx, GL_INVALID_VALUE);
    if (!stride) stride = s;
    const char *p = pointer;
    glDisableClientState(GL_EDGE_FLAG_ARRAY);
    glDisableClientState(GL_INDEX_ARRAY);
    if (tc) {
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(tc, GL_FLOAT, stride, p);
    } else {
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    if (cc) {
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(cc, ctype, stride, p + pc);
    } else {
        glDisableClientState(GL_COLOR_ARRAY);
    }
    if (normal) {
        glEnableClientState(GL_NORMAL_ARRAY);
        glNormalPointer(GL_FLOAT, stride, p + pn);
    } else {
        glDisableClientState(GL_NORMAL_ARRAY);
    }
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(vc, GL_FLOAT, stride, p + pv);
}

/* ---- attribute fetch ----------------------------------------------------- */

static size_t type_size(GLenum type)
{
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: return 2;
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: case GL_FIXED: case GL_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_2_10_10_10_REV: return 4;
    case GL_DOUBLE: return 8;
    default: return 4;
    }
}

static float half_to_float(uint16_t h)
{
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, exponent = (h >> 10) & 31, mantissa = h & 1023, bits;
    if (exponent == 0) {
        if (!mantissa) bits = sign;
        else {
            exponent = 127 - 15 + 1;
            while (!(mantissa & 1024)) { mantissa <<= 1; --exponent; }
            bits = sign | exponent << 23 | (mantissa & 1023) << 13;
        }
    } else if (exponent == 31) bits = sign | 0x7f800000 | mantissa << 13;
    else bits = sign | (exponent + 127 - 15) << 23 | mantissa << 13;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

/* Reads one attribute element as float4 following GL's conversion rules. */
static void fetch_element(const struct glm_array *a, const uint8_t *p, float *out)
{
    out[0] = out[1] = out[2] = 0;
    out[3] = 1;
    int size = a->size == GL_BGRA ? 4 : a->size;
    bool normalize = a->normalized && !a->integer;
    for (int i = 0; i < size; ++i) {
        float v = 0;
        switch (a->type) {
        case GL_FLOAT: { float f; memcpy(&f, p + i * 4, 4); v = f; break; }
        case GL_DOUBLE: { double d; memcpy(&d, p + i * 8, 8); v = (float)d; break; }
        case GL_HALF_FLOAT: { uint16_t h; memcpy(&h, p + i * 2, 2); v = half_to_float(h); break; }
        case GL_UNSIGNED_BYTE: v = p[i]; if (normalize) v /= 255.0f; break;
        case GL_BYTE: v = (int8_t)p[i]; if (normalize) v = (2 * v + 1) / 255.0f; break;
        case GL_UNSIGNED_SHORT: { uint16_t s; memcpy(&s, p + i * 2, 2); v = s; if (normalize) v /= 65535.0f; break; }
        case GL_SHORT: { int16_t s; memcpy(&s, p + i * 2, 2); v = s; if (normalize) v = (2 * v + 1) / 65535.0f; break; }
        case GL_UNSIGNED_INT: { uint32_t u; memcpy(&u, p + i * 4, 4); v = (float)u; if (normalize) v = (float)(u / 4294967295.0); break; }
        case GL_INT: { int32_t s; memcpy(&s, p + i * 4, 4); v = (float)s; if (normalize) v = (float)((2.0 * s + 1) / 4294967295.0); break; }
        case GL_FIXED: { int32_t s; memcpy(&s, p + i * 4, 4); v = (float)s / 65536.0f; break; }
        default: break;
        }
        if (a->integer) {
            /* Integer attributes travel as their bit pattern. */
            int32_t bits = 0;
            switch (a->type) {
            case GL_UNSIGNED_BYTE: bits = p[i]; break;
            case GL_BYTE: bits = (int8_t)p[i]; break;
            case GL_UNSIGNED_SHORT: { uint16_t s; memcpy(&s, p + i * 2, 2); bits = s; break; }
            case GL_SHORT: { int16_t s; memcpy(&s, p + i * 2, 2); bits = s; break; }
            default: memcpy(&bits, p + i * 4, 4); break;
            }
            memcpy(&v, &bits, 4);
        }
        out[i] = v;
    }
    if (a->size == GL_BGRA) {
        float t = out[0];
        out[0] = out[2];
        out[2] = t;
    }
    if (a->integer) {
        /* Missing integer components default to 0,0,0,1 as integers. */
        for (int i = size; i < 4; ++i) {
            int32_t bits = i == 3 ? 1 : 0;
            memcpy(&out[i], &bits, 4);
        }
    }
}

/* The array's first element; `limit` gets the bytes readable from it
   (SIZE_MAX for client memory). */
static const uint8_t *array_base(struct glm_context *ctx, const struct glm_array *a, size_t *limit)
{
    *limit = SIZE_MAX;
    if (!a->buffer) return a->pointer;
    struct glm_buffer *buffer = glm_buffer_get(ctx, a->buffer);
    if (!buffer) return NULL;
    const uint8_t *contents = glm_backend_buffer_contents(ctx, buffer, false);
    if (!contents || (uintptr_t)a->pointer > (uintptr_t)buffer->size) return NULL;
    *limit = (size_t)buffer->size - (uintptr_t)a->pointer;
    return contents + (uintptr_t)a->pointer;
}

/* Builds the vertex stream for vertices [first, first + count) with every
   attribute slot filled (arrays where enabled, current values otherwise). */
static float *fetch_vertices(struct glm_context *ctx, uint32_t first, uint32_t count, uint32_t instance)
{
    float *out = malloc((size_t)count * GLM_MAX_ATTRIBS * 4 * sizeof(float));
    if (!out) return NULL;
    /* Integer shader inputs read the stream's bits as integers: their
       current values are converted. */
    uint32_t integer_inputs = 0;
    const struct glm_program *program = glm_current_program(ctx);
    if (program)
        for (int i = 0; i < program->result.attribute_count; ++i)
            if (program->result.attributes[i].integer && program->result.attributes[i].index != GLM_INPUT_BUILTIN &&
                program->result.attributes[i].location >= 0 &&
                program->result.attributes[i].location < GLM_MAX_ATTRIBS)
                integer_inputs |= 1u << program->result.attributes[i].location;
    for (int slot = 0; slot < GLM_MAX_ATTRIBS; ++slot) {
        const struct glm_array *a = &ctx->vao->arrays[slot];
        size_t limit = 0;
        const uint8_t *base = a->enabled ? array_base(ctx, a, &limit) : NULL;
        if (!base) {
            float value[4];
            memcpy(value, ctx->state.current[slot], sizeof value);
            if ((integer_inputs >> slot) & 1)
                for (int c = 0; c < 4; ++c) {
                    int32_t bits = (int32_t)ctx->state.current[slot][c];
                    memcpy(&value[c], &bits, 4);
                }
            for (uint32_t v = 0; v < count; ++v)
                memcpy(out + ((size_t)v * GLM_MAX_ATTRIBS + slot) * 4, value, sizeof value);
            continue;
        }
        size_t element = type_size(a->type) * (size_t)(a->size == GL_BGRA ? 4 : a->size);
        size_t stride = a->stride ? (size_t)a->stride : element;
        for (uint32_t v = 0; v < count; ++v) {
            uint32_t index = a->divisor ? instance / a->divisor : first + v;
            float *dst = out + ((size_t)v * GLM_MAX_ATTRIBS + slot) * 4;
            /* Past the end of a buffer: zeros (robust access, like Metal's
               vertex fetch) rather than a read of whatever follows. */
            if (limit != SIZE_MAX && (stride * index > limit || limit - stride * index < element)) {
                dst[0] = dst[1] = dst[2] = 0;
                dst[3] = 1;
                continue;
            }
            fetch_element(a, base + stride * index, dst);
        }
    }
    return out;
}

/* ---- primitive conversion ------------------------------------------------ */

#define index_list glm_index_list

static void push(struct index_list *list, uint32_t index)
{
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 64;
        list->data = realloc(list->data, list->capacity * sizeof(uint32_t));
    }
    list->data[list->count++] = index;
}

static void push3(struct index_list *list, uint32_t a, uint32_t b, uint32_t c) { push(list, a); push(list, b); push(list, c); }

/* GL's flat-shading provoking vertex for primitive `p` of `mode` given its
   vertex indices (indices into `seq`). */
uint32_t glm_triangulate(GLenum mode, const uint32_t *seq, uint32_t n, struct index_list *out, uint32_t *metal_primitive, bool geometry)
{
    switch (mode) {
    case GL_POINTS:
        *metal_primitive = MTL_POINT;
        for (uint32_t i = 0; i < n; ++i) push(out, seq[i]);
        break;
    /* Patch vertices in order; the caller drops an incomplete last patch. */
    case GL_PATCHES:
        *metal_primitive = MTL_POINT;
        for (uint32_t i = 0; i < n; ++i) push(out, seq[i]);
        break;
    case GL_LINES:
        *metal_primitive = MTL_LINE;
        for (uint32_t i = 0; i + 1 < n; i += 2) { push(out, seq[i]); push(out, seq[i + 1]); }
        break;
    case GL_LINE_STRIP:
    case GL_LINE_LOOP:
        *metal_primitive = MTL_LINE;
        for (uint32_t i = 0; i + 1 < n; ++i) { push(out, seq[i]); push(out, seq[i + 1]); }
        if (mode == GL_LINE_LOOP && n > 2) { push(out, seq[n - 1]); push(out, seq[0]); }
        break;
    case GL_TRIANGLES:
        *metal_primitive = MTL_TRIANGLE;
        for (uint32_t i = 0; i + 2 < n; i += 3) push3(out, seq[i], seq[i + 1], seq[i + 2]);
        break;
    case GL_TRIANGLE_STRIP:
        *metal_primitive = MTL_TRIANGLE;
        for (uint32_t i = 0; i + 2 < n; ++i) {
            if (i & 1) push3(out, seq[i + 1], seq[i], seq[i + 2]);
            else push3(out, seq[i], seq[i + 1], seq[i + 2]);
        }
        break;
    case GL_TRIANGLE_FAN:
    case GL_POLYGON:
        *metal_primitive = MTL_TRIANGLE;
        /* Apple orders fan triangles (i+1, i+2, 0): what geometry shaders
           see as their vertex order (same winding as (0, i+1, i+2)). */
        for (uint32_t i = 1; i + 1 < n; ++i) push3(out, seq[i], seq[i + 1], seq[0]);
        break;
    /* Apple splits quads along the v1-v3 diagonal, which decides how
       smooth-shaded colours interpolate across them. */
    case GL_QUADS:
        *metal_primitive = MTL_TRIANGLE;
        for (uint32_t i = 0; i + 3 < n; i += 4) {
            push3(out, seq[i], seq[i + 1], seq[i + 3]);
            push3(out, seq[i + 1], seq[i + 2], seq[i + 3]);
        }
        break;
    /* Quad strips split along v0-v3 of each quad (v0 v1 v3 v2). */
    case GL_QUAD_STRIP:
        *metal_primitive = MTL_TRIANGLE;
        for (uint32_t i = 0; i + 3 < n; i += 2) {
            push3(out, seq[i], seq[i + 1], seq[i + 3]);
            push3(out, seq[i], seq[i + 3], seq[i + 2]);
        }
        break;
    /* Geometry shaders read every adjacency vertex. Without one, only
       the central line or triangle reaches rasterization. */
    case GL_LINES_ADJACENCY:
        *metal_primitive = MTL_LINE;
        for (uint32_t i = 0; i + 3 < n; i += 4) {
            if (geometry) push(out, seq[i]);
            push(out, seq[i + 1]); push(out, seq[i + 2]);
            if (geometry) push(out, seq[i + 3]);
        }
        break;
    case GL_LINE_STRIP_ADJACENCY:
        *metal_primitive = MTL_LINE;
        for (uint32_t i = 1; i + 2 < n; ++i) {
            if (geometry) push(out, seq[i - 1]);
            push(out, seq[i]); push(out, seq[i + 1]);
            if (geometry) push(out, seq[i + 2]);
        }
        break;
    case GL_TRIANGLES_ADJACENCY:
        *metal_primitive = MTL_TRIANGLE;
        for (uint32_t i = 0; i + 5 < n; i += 6)
            for (uint32_t k = 0; k < 6; k += geometry ? 1 : 2) push(out, seq[i + k]);
        break;
    case GL_TRIANGLE_STRIP_ADJACENCY:
        /* GL 3.2 table 2.4: triangle i uses 2i, 2i+2, 2i+4 with adjacent
           vertices chosen by position in the strip. */
        *metal_primitive = MTL_TRIANGLE;
        if (n >= 6) {
            uint32_t triangles = (n - 4) / 2;
            for (uint32_t i = 0; i < triangles; ++i) {
                uint32_t v[6];
                bool last = i == triangles - 1;
                if (i == 0) {
                    v[0] = 0; v[1] = 1; v[2] = 2; v[3] = last ? 5 : 6; v[4] = 4; v[5] = 3;
                } else if (i & 1) {
                    v[0] = 2 * i + 2; v[1] = 2 * i - 2; v[2] = 2 * i; v[3] = 2 * i + 3; v[4] = 2 * i + 4;
                    v[5] = last ? 2 * i + 5 : 2 * i + 6;
                } else {
                    v[0] = 2 * i; v[1] = 2 * i - 2; v[2] = 2 * i + 2; v[3] = last ? 2 * i + 5 : 2 * i + 6; v[4] = 2 * i + 4;
                    v[5] = 2 * i + 3;
                }
                for (int k = 0; k < 6; k += geometry ? 1 : 2) push(out, seq[v[k] < n ? v[k] : n - 1]);
            }
        }
        break;
    default:
        *metal_primitive = MTL_TRIANGLE;
        break;
    }
    return out->count;
}

/* Index (within the primitive's GL vertices) whose attributes a flat-shaded
   primitive uses, per GL 2.1 table 2.12. Returns the GL vertex position in
   `seq` for output triangle/line `t`. */
static uint32_t provoking(GLenum mode, uint32_t t, uint32_t n, bool first)
{
    if (first) {
        /* GL 3.2 table 2.15, first-vertex convention. */
        switch (mode) {
        case GL_LINES_ADJACENCY: return 4 * t + 1;
        case GL_LINE_STRIP_ADJACENCY: return t + 1;
        case GL_TRIANGLES_ADJACENCY: return 6 * t;
        case GL_TRIANGLE_STRIP_ADJACENCY: return 2 * t;
        case GL_LINES: return 2 * t;
        case GL_LINE_STRIP: case GL_LINE_LOOP: return t;
        case GL_TRIANGLES: return 3 * t;
        case GL_TRIANGLE_STRIP: return t;
        case GL_TRIANGLE_FAN: return t + 1;
        case GL_POLYGON: return 0;
        case GL_QUADS: return 4 * (t / 2);
        case GL_QUAD_STRIP: return 2 * (t / 2);
        default: return t;
        }
    }
    switch (mode) {
    case GL_LINES_ADJACENCY: return 4 * t + 2;
    case GL_LINE_STRIP_ADJACENCY: return t + 2;
    case GL_TRIANGLES_ADJACENCY: return 6 * t + 4;
    case GL_TRIANGLE_STRIP_ADJACENCY: return 2 * t + 4;
    case GL_LINES: return 2 * t + 1;
    case GL_LINE_STRIP: return t + 1;
    case GL_LINE_LOOP: return t + 1 < n ? t + 1 : 0;
    case GL_TRIANGLES: return 3 * t + 2;
    case GL_TRIANGLE_STRIP: return t + 2;
    case GL_TRIANGLE_FAN: return t + 2;
    case GL_POLYGON: return 0;
    case GL_QUADS: return 4 * (t / 2) + 3;
    case GL_QUAD_STRIP: return 2 * (t / 2) + 3;
    default: return t;
    }
}

/* Whether a geometry shader taking primitives of `in_vertices` vertices
   accepts draws of `mode` (GL 3.2 section 2.12.1). */
static bool gs_accepts(int in_vertices, GLenum mode)
{
    switch (in_vertices) {
    case 1: return mode == GL_POINTS;
    case 2: return mode == GL_LINES || mode == GL_LINE_STRIP || mode == GL_LINE_LOOP;
    case 4: return mode == GL_LINES_ADJACENCY || mode == GL_LINE_STRIP_ADJACENCY;
    case 3: return mode == GL_TRIANGLES || mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN;
    case 6: return mode == GL_TRIANGLES_ADJACENCY || mode == GL_TRIANGLE_STRIP_ADJACENCY;
    }
    return true;
}

/* Input interpolation commutes with linear fixed-function vertex processing.
   Lighting, generated reflection coordinates and fog need post-VS clipping. */
static bool linear_fixed_clip(struct glm_context *ctx)
{
    const struct glm_state *s = &ctx->state;
    if (ctx->profile != GLM_PROFILE_LEGACY || glm_current_program(ctx) || s->vertex_program ||
        s->lighting || s->fog || ctx->user_clip_preprocessed || glm_feedback_capturing(ctx) ||
        s->polygon_mode[0] != GL_FILL || s->polygon_mode[1] != GL_FILL)
        return false;
    bool enabled = false;
    for (int i = 0; i < GLM_MAX_CLIP_PLANES; ++i) enabled |= s->clip_plane_enabled[i];
    if (!enabled) return false;
    for (int u = 0; u < GLM_MAX_TEXTURE_UNITS; ++u)
        for (int c = 0; c < 4; ++c) {
            const struct glm_texgen *g = &s->units[u].gen[c];
            if (g->enabled && g->mode != GL_OBJECT_LINEAR && g->mode != GL_EYE_LINEAR) return false;
        }
    return true;
}

static bool linear_clip_stream(struct glm_context *ctx, GLenum mode)
{
    bool triangles = mode == GL_TRIANGLES || mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN ||
                     mode == GL_QUADS || mode == GL_QUAD_STRIP || mode == GL_POLYGON;
    return triangles && linear_fixed_clip(ctx);
}

static void streamed_batch(struct glm_context *ctx, const struct glm_draw *draw, float *vertices, uint32_t count)
{
    if (!count) return;
    struct glm_draw batch = *draw;
    batch.vertices = vertices;
    batch.indices = NULL;
    batch.vertex_count = batch.index_count = count;
    glm_backend_draw(ctx, &batch);
}

static bool linear_clip_draw(struct glm_context *ctx, const struct glm_draw *draw)
{
    if (draw->metal_primitive != MTL_TRIANGLE || !linear_fixed_clip(ctx)) return false;
    enum { COMPONENTS = GLM_MAX_ATTRIBS * 4, MAX_VERTICES = 3 + GLM_MAX_CLIP_PLANES };
    const struct glm_state *s = &ctx->state;
    const float *modelview = s->modelview.m[s->modelview.depth];
    double planes[GLM_MAX_CLIP_PLANES][4];
    for (int p = 0; p < GLM_MAX_CLIP_PLANES; ++p)
        for (int c = 0; c < 4; ++c) {
            planes[p][c] = 0;
            for (int r = 0; r < 4; ++r)
                planes[p][c] += s->clip_planes[p][r] * (double)modelview[c * 4 + r];
        }
    unsigned active_planes = 0;
    for (int p = 0; p < GLM_MAX_CLIP_PLANES; ++p) active_planes += s->clip_plane_enabled[p];
    /* Bound temporary memory independently of the input draw's size. */
    uint32_t capacity = (uint32_t)fmin((double)(draw->index_count / 3) * (active_planes + 1) * 3, 16384);
    float *output = malloc((size_t)capacity * COMPONENTS * sizeof(float));
    if (!output) { glm_error(ctx, GL_OUT_OF_MEMORY); return true; }
    uint32_t written = 0;
    ctx->user_clip_preprocessed = true;
    __atomic_add_fetch(&ctx->state_serial, 1, __ATOMIC_RELAXED);
    for (uint32_t primitive = 0; primitive + 2 < draw->index_count; primitive += 3) {
        double polygons[2][MAX_VERTICES][COMPONENTS];
        unsigned count = 3, buffer = 0;
        for (unsigned v = 0; v < 3; ++v) {
            uint32_t index = draw->indices ? draw->indices[primitive + v] : primitive + v;
            const float *input = draw->vertices + (size_t)index * COMPONENTS;
            for (unsigned c = 0; c < COMPONENTS; ++c) polygons[0][v][c] = input[c];
            /* FF saturates vertex colors before interpolation. submit already
               replicated each flat-shaded primitive's provoking colors. */
            for (int slot = GLM_ATTR_COLOR; slot <= GLM_ATTR_SECONDARY_COLOR; ++slot)
                for (int c = 0; c < 4; ++c)
                    polygons[0][v][slot * 4 + c] = fmin(fmax(input[slot * 4 + c], 0), 1);
        }
        for (int p = 0; p < GLM_MAX_CLIP_PLANES && count; ++p) {
            if (!s->clip_plane_enabled[p]) continue;
            double distances[MAX_VERTICES];
            unsigned inside = 0;
            for (unsigned v = 0; v < count; ++v) {
                distances[v] = 0;
                for (int c = 0; c < 4; ++c)
                    distances[v] += planes[p][c] * polygons[buffer][v][GLM_ATTR_POSITION * 4 + c];
                inside += distances[v] >= 0;
            }
            for (unsigned v = 0; v < count; ++v)
                if (!isfinite(distances[v])) { count = 0; break; }
            if (!count) break;
            if (inside == count) continue; /* retain the original fan origin */
            if (!inside) { count = 0; break; }
            unsigned next = buffer ^ 1, produced = 0;
            for (unsigned a = 0; a < count; ++a) {
                unsigned b = (a + 1) % count;
                bool a_inside = distances[a] >= 0, b_inside = distances[b] >= 0;
                if (a_inside != b_inside) {
                    if (produced == MAX_VERTICES) { produced = 0; break; }
                    double t = distances[a] / (distances[a] - distances[b]);
                    for (unsigned c = 0; c < COMPONENTS; ++c) {
                        double av = polygons[buffer][a][c], bv = polygons[buffer][b][c];
                        polygons[next][produced][c] = av + t * (bv - av);
                    }
                    ++produced;
                }
                if (b_inside) {
                    if (produced == MAX_VERTICES) { produced = 0; break; }
                    memcpy(polygons[next][produced], polygons[buffer][b], sizeof polygons[next][produced]);
                    ++produced;
                }
            }
            count = produced;
            buffer = next;
        }
        unsigned primitive_vertices = count >= 3 ? (count - 2) * 3 : 0;
        if (primitive_vertices > capacity) continue; /* invalid/nonconvex numerical input */
        if (written + primitive_vertices > capacity) {
            streamed_batch(ctx, draw, output, written);
            written = 0;
        }
        for (unsigned triangle = 1; triangle + 1 < count; ++triangle) {
            unsigned indices[] = {0, triangle, triangle + 1};
            for (unsigned v = 0; v < 3; ++v) {
                for (unsigned c = 0; c < COMPONENTS; ++c)
                    output[(size_t)written * COMPONENTS + c] = (float)polygons[buffer][indices[v]][c];
                ++written;
            }
        }
    }
    streamed_batch(ctx, draw, output, written);
    ctx->user_clip_preprocessed = false;
    __atomic_add_fetch(&ctx->state_serial, 1, __ATOMIC_RELAXED);
    free(output);
    return true;
}

/* GL 2.1 section 3.4.2: aliased wide lines replicate a one-pixel line
   along the minor window axis. Each copy still uses Metal's line coverage
   and interpolates the original attributes. The fixed-function fog input
   uses only x; streamed copies carry their clip offset in y/z. */
static bool wide_fixed_lines(struct glm_context *ctx)
{
    return ctx->profile == GLM_PROFILE_LEGACY && ctx->state.line_width >= 1.5f &&
           !ctx->state.line_smooth && !glm_current_program(ctx) && !ctx->state.vertex_program &&
           !(ctx->state.multisample && glm_backend_draw_samples(ctx) > 1);
}

static bool wide_line_stream(struct glm_context *ctx, GLenum mode)
{
    bool lines = mode == GL_LINES || mode == GL_LINE_STRIP || mode == GL_LINE_LOOP;
    bool outline = mode != GL_POINTS && (ctx->state.polygon_mode[0] == GL_LINE ||
                                         ctx->state.polygon_mode[1] == GL_LINE);
    return (lines || outline) && wide_fixed_lines(ctx);
}

/* Terminal coverage is distinct from directed narrow-line diamond exit.
 * This helper returns only strictly unambiguous samples on a half-open
 * major-axis interval; native replicas retain all other coverage. */
static bool wide_terminal_pixel(double ax, double ay, double bx, double by,
                                bool x_major, int *px, int *py)
{
    ax = floor(ax * 256 + 0.5) / 256; ay = floor(ay * 256 + 0.5) / 256;
    bx = floor(bx * 256 + 0.5) / 256; by = floor(by * 256 + 0.5) / 256;
    double a = x_major ? ax : ay, b = x_major ? bx : by;
    if (a == b || fabs(bx - ax) == fabs(by - ay) ||
        x_major != (fabs(bx - ax) > fabs(by - ay))) return false;
    double major = floor(b) + 0.5;
    /* An upward y-major endpoint at an integral row sits on the upper tip
     * of the preceding sample diamond. Use that last half-open center only
     * for this separately validated family; other candidates stay unchanged. */
    if (!x_major && a < b && by == floor(by) && bx == floor(bx) + 0.5)
        major = b - 0.5;
    if (!(major >= fmin(a, b) && major < fmax(a, b))) return false;
    double t = (major - a) / (b - a);
    double minor = x_major ? ay + t * (by - ay) : ax + t * (bx - ax);
    if (minor == floor(minor)) return false;
    double cx = x_major ? major : floor(minor) + 0.5;
    double cy = x_major ? floor(minor) + 0.5 : major;
    /* Interior endpoints cannot exit the diamond. Native probes also prove
     * this for upper slanted edges and nonaxis y-major upper tips. Other
     * exact tips have direction-dependent tie rules and remain excluded. */
    double distance = fabs(bx - cx) + fabs(by - cy);
    bool upper_edge = distance == 0.5 && by > cy && bx != cx &&
                      ax != bx && ay != by;
    bool upper_tip = !x_major && ax != bx && bx == cx && by - cy == 0.5;
    if (!(distance < 0.5 || upper_edge || upper_tip)) return false;
    if ((upper_edge || upper_tip) && !(fabs(ax - cx) + fabs(ay - cy) > 0.5)) return false;
    if (cx < -2147483648.0 || cx > 2147483647.0 ||
        cy < -2147483648.0 || cy > 2147483647.0) return false;
    *px = (int)floor(cx); *py = (int)floor(cy);
    return true;
}

/* An identity transform and W=1 leave only the viewport affine map.
 * gamma(12) bounds replica-offset division, clip addition, coefficient
 * conversion and either contracted
 * or separate multiply/add evaluation. Reject an endpoint if this derived
 * interval straddles a 1/256 round-to-nearest quantization boundary. */
static bool wide_terminal_viewport(float ndc, int origin, int size, double window, double offset)
{
    const double u = 0x1p-24;
    const double gamma = (12 * u) / (1 - 12 * u);
    double magnitude = (fabs((double)ndc) + 1) * 0.5 * size + fabs((double)origin);
    double error = gamma * (magnitude + fabs(offset));
    double lo = floor((window - error) * 256 + 0.5);
    double hi = floor((window + error) * 256 + 0.5);
    return lo == hi;
}

struct wide_axis_interval {
    int32_t first_pixel, last_pixel; /* canonical increasing order */
    uint32_t count;
    double draw_start, draw_end;    /* original traversal direction */
};

/* Constant-color/depth-off axis reconstruction. No clipping is
 * performed: moving an endpoint across the viewport would invalidate the
 * diamond-exit proof. False means retain the original implementation. */
static bool wide_axis_bounds(double start, double end,
        int32_t viewport_origin, uint32_t viewport_size,
        struct wide_axis_interval *out)
{
    if (!out) return false;
    *out = (struct wide_axis_interval){0};
    if (!isfinite(start) || !isfinite(end) || viewport_size == 0 || viewport_size > INT32_MAX) return false;
    double edge0 = viewport_origin, edge1 = edge0 + viewport_size;
    if (!(start > edge0 && start < edge1 && end > edge0 && end < edge1)) return false;
    start = floor(start * 256 + 0.5) / 256;
    end = floor(end * 256 + 0.5) / 256;
    double low = fmin(start, end), high = fmax(start, end);
    double first = ceil(low - 0.5), last = ceil(high - 0.5) - 1;
    if (first > last) return true; /* Empty half-open sample-center interval. */
    if (first < INT32_MIN || first > INT32_MAX || last < INT32_MIN || last > INT32_MAX) return false;
    double count = last - first + 1;
    if (!(count > 0 && count <= UINT32_MAX)) return false;
    double draw_start = start < end ? first + 0.5 : last + 0.5;
    double draw_end = start < end ? last + 1.25 : first - 0.25;
    if (!(draw_start > edge0 && draw_start < edge1 && draw_end > edge0 && draw_end < edge1)) return false;
    out->first_pixel = (int32_t)first; out->last_pixel = (int32_t)last;
    out->count = (uint32_t)count; out->draw_start = draw_start; out->draw_end = draw_end;
    return true;
}

/* Clip only sample centers outside the snapped half-open major interval.
 * Double bounds are clamped before casts; app-scissor sums use int64_t. */
static bool wide_major_scissor(double a, double b, int axis, bool enabled,
                               const int original[4], int box[4])
{
    if (!isfinite(a) || !isfinite(b)) return false;
    a = floor(a * 256 + 0.5) / 256;
    b = floor(b * 256 + 0.5) / 256;
    double first = ceil(fmin(a, b) - 0.5), end = ceil(fmax(a, b) - 0.5);
    int64_t lo[2] = {0, 0}, hi[2] = {INT_MAX, INT_MAX};
    lo[axis] = (int64_t)fmax(0, fmin(INT_MAX, first));
    hi[axis] = (int64_t)fmax(0, fmin(INT_MAX, end));
    if (enabled) for (int c = 0; c < 2; ++c) {
        int64_t app_end = (int64_t)original[c] + original[c + 2];
        if (lo[c] < original[c]) lo[c] = original[c];
        if (hi[c] > app_end) hi[c] = app_end;
        if (original[c + 2] <= 0) hi[c] = lo[c];
    }
    for (int c = 0; c < 2; ++c) {
        box[c] = (int)lo[c];
        box[c + 2] = (int)(hi[c] > lo[c] ? hi[c] - lo[c] : 0);
    }
    return true;
}

static bool wide_interval_state(struct glm_context *ctx, const struct glm_draw *draw, bool transformed)
{
    const struct glm_state *s = &ctx->state;
    if (ctx->polygon_split || draw->capture || draw->instance_count != 1 || ctx->xfb.active ||
        ctx->bound_pipeline || s->fragment_program || s->lighting || s->fog ||
        s->depth_test || s->point_sprite || s->rasterizer_discard || glm_backend_draw_samples(ctx) != 1 ||
        (draw->mode != GL_LINES && draw->mode != GL_LINE_STRIP && draw->mode != GL_LINE_LOOP)) return false;
    /* Identity avoids CPU/GPU dot-product contraction differences near a
     * native projected-coordinate quantization boundary. */
    const float *modelview = s->modelview.m[s->modelview.depth];
    const float *projection = s->projection.m[s->projection.depth];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
        float identity = r == c ? 1.0f : 0.0f;
        if (modelview[c * 4 + r] != identity || (!transformed && projection[c * 4 + r] != identity)) return false;
    }
    for (int q = 2; q < 4; ++q) {
        if (ctx->active_queries[q]) return false;
        for (int stream = 0; stream < 3; ++stream) if (ctx->indexed_queries[q][stream]) return false;
    }
    for (int c = 0; c < ctx->extra_cap_count; ++c)
        if (ctx->extra_caps[c].enabled && (ctx->extra_caps[c].cap == GL_LINE_STIPPLE ||
                                          ctx->extra_caps[c].cap == GL_POINT_SMOOTH)) return false;
    for (int p = 0; p < GLM_MAX_CLIP_PLANES; ++p) if (!transformed && s->clip_plane_enabled[p]) return false;
    for (int u = 0; u < GLM_MAX_TEXTURE_UNITS; ++u) {
        const struct glm_texture_unit *unit = &s->units[u];
        if (unit->enabled_1d || unit->enabled_2d || unit->enabled_3d || unit->enabled_cube || unit->enabled_rect) return false;
        for (int c = 0; c < 4; ++c) if (unit->gen[c].enabled) return false;
    }
    return true;
}

static bool wide_terminal_state(struct glm_context *ctx, const struct glm_draw *draw)
{
    return wide_interval_state(ctx, draw, false);
}

/* Trim only, using identity eye coordinates and either identity projection or
 * an axis-aligned symmetric perspective projection. This keeps shader input
 * evaluation bounded without claiming a hardware raster-division error proof.
 * gamma(12) is a conservative viewport envelope for the measured native/Metal setup on
 * these structures; ambiguous quantization, clipping and major axes fall back.
 * It never moves a vertex or reconstructs an attribute. */
static bool wide_projected_interval(struct glm_context *ctx, const float *ends[2],
                                    const float mvp[16], bool x_major, double window[2][2])
{
    const struct glm_state *s = &ctx->state;
    const double u = 0x1p-24, gamma = (12 * u) / (1 - 12 * u);
    const double dot_gamma = (7 * u) / (1 - 7 * u);
    const double lerp_gamma = (5 * u) / (1 - 5 * u);
    bool identity = true;
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) {
        if (!isfinite(mvp[c * 4 + r])) return false;
        if (mvp[c * 4 + r] != (c == r ? 1.0f : 0.0f)) identity = false;
    }
    if (!identity) {
        /* No cross-axis terms, asymmetric frustum offsets or eye-plane shifts. */
        for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
            if (!(c == r && c < 3) && !(c == 3 && r == 2) && !(c == 2 && r == 3) && mvp[c * 4 + r] != 0) return false;
        if (!(mvp[0] > 0 && mvp[5] > 0 && mvp[11] == -1 && mvp[15] == 0)) return false;
        /* Exact power-of-two XY scaling and W=-Z avoid a shader dot error
         * in the coordinates actually divided by the rasterizer. */
        int exponent;
        if (frexpf(mvp[0], &exponent) != .5f || frexpf(mvp[5], &exponent) != .5f) return false;
    }
    int plane = -1;
    for (int p = 0; p < GLM_MAX_CLIP_PLANES; ++p) if (s->clip_plane_enabled[p]) {
        if (plane >= 0) return false; /* Multiple intersections are not certified. */
        plane = p;
    }
    double clip[2][4], error[2][4], distance[2] = {1, 1}, distance_error[2] = {0, 0};
    for (int e = 0; e < 2; ++e) {
        const float *p = ends[e] + GLM_ATTR_POSITION * 4;
        for (int c = 0; c < 4; ++c) if (!isfinite(p[c])) return false;
        for (int r = 0; r < 4; ++r) {
            double value = 0, magnitude = 0;
            for (int c = 0; c < 4; ++c) {
                double product = (double)mvp[c * 4 + r] * p[c];
                if (product != 0 && fabs(product) < FLT_MIN) return false;
                value += product; magnitude += fabs(product);
            }
            if (magnitude > FLT_MAX) return false;
            clip[e][r] = value;
            error[e][r] = identity || r != 2 ? 0 : dot_gamma * magnitude + 0x1p-140;
            if (!isfinite(value) || fabs(value) > FLT_MAX ||
                (value != 0 && fabs(value) < FLT_MIN)) return false;
        }
        /* Do not infer hardware clipping near the eye, near/far or side planes. */
        if (!(clip[e][3] > error[e][3])) return false;
        for (int c = 0; c < 3; ++c)
            if (!(fabs(clip[e][c]) + error[e][c] < clip[e][3] - error[e][3])) return false;
        if (plane >= 0) {
            double value = 0, magnitude = 0;
            for (int c = 0; c < 4; ++c) {
                double coefficient = (float)s->clip_planes[plane][c];
                if (!isfinite(coefficient)) return false;
                double product = coefficient * p[c];
                if (product != 0 && fabs(product) < FLT_MIN) return false;
                value += product; magnitude += fabs(product);
            }
            if (magnitude > FLT_MAX || (value != 0 && fabs(value) < FLT_MIN)) return false;
            distance[e] = value; distance_error[e] = dot_gamma * magnitude + 0x1p-140;
            if (!(fabs(value) > distance_error[e])) return false;
        }
    }
    if (plane >= 0) {
        if (distance[0] < 0 && distance[1] < 0) return false;
        if ((distance[0] < 0) != (distance[1] < 0)) {
            double denominator = distance[0] - distance[1];
            double denominator_error = distance_error[0] + distance_error[1];
            if (!(fabs(denominator) > denominator_error)) return false;
            double t = distance[0] / denominator;
            double t_error = (distance_error[0] + fabs(t) * denominator_error) /
                (fabs(denominator) - denominator_error) + (3 * u) / (1 - 3 * u) * fabs(t);
            if (!(t - t_error > 0 && t + t_error < 1)) return false;
            int outside = distance[0] < 0 ? 0 : 1;
            for (int c = 0; c < 4; ++c) {
                double delta = clip[1][c] - clip[0][c];
                double value = clip[0][c] + t * delta;
                double bound = error[0][c] + fabs(t) * (error[0][c] + error[1][c]) +
                    t_error * (fabs(delta) + error[0][c] + error[1][c]) +
                    lerp_gamma * (fabs(clip[0][c]) + fabs(t * delta));
                clip[outside][c] = value; error[outside][c] = bound;
            }
        }
    }
    double window_error[2][2];
    for (int e = 0; e < 2; ++e) for (int c = 0; c < 2; ++c) {
        double w = clip[e][3], ew = error[e][3];
        if (!(w > ew)) return false;
        double ndc = clip[e][c] / w;
        double bound = (error[e][c] + fabs(ndc) * ew) / (w - ew);
        window[e][c] = s->viewport[c] + (ndc * .5 + .5) * s->viewport[c + 2];
        window_error[e][c] = bound * .5 * s->viewport[c + 2] +
            gamma * ((fabs(ndc) + 1) * .5 * s->viewport[c + 2] +
                     fabs((double)s->viewport[c]) + 1);
        double lo = floor((window[e][c] - window_error[e][c]) * 256 + .5);
        double hi = floor((window[e][c] + window_error[e][c]) * 256 + .5);
        if (!isfinite(lo) || lo != hi) return false;
    }
    double dx = window[1][0] - window[0][0], dy = window[1][1] - window[0][1];
    double ex = window_error[0][0] + window_error[1][0] + 1.0 / 256;
    double ey = window_error[0][1] + window_error[1][1] + 1.0 / 256;
    if (!(fabs(dx) > ex && fabs(dy) > ey && fabs(fabs(dx) - fabs(dy)) > ex + ey)) return false;
    return x_major == (fabs(dx) > fabs(dy));
}

static bool wide_line_draw(struct glm_context *ctx, const struct glm_draw *draw)
{
    if (draw->metal_primitive != MTL_LINE || !wide_fixed_lines(ctx)) return false;
    const struct glm_state *s = &ctx->state;
    if (s->viewport[2] <= 0 || s->viewport[3] <= 0) return true;
    unsigned width = (unsigned)floorf(fminf(s->line_width, 16.0f) + 0.5f);
    const size_t stride = GLM_MAX_ATTRIBS * 4;
    uint32_t capacity = (16384 / (2 * width)) * (2 * width);
    if ((uint64_t)draw->index_count * width < capacity) capacity = draw->index_count * width;
    float *copies = malloc((size_t)capacity * stride * sizeof(float));
    if (!copies) { glm_error(ctx, GL_OUT_OF_MEMORY); return true; }
    uint32_t written = 0;
    bool terminal_state = wide_terminal_state(ctx, draw);
    bool trim_state = wide_interval_state(ctx, draw, true);
    ctx->wide_line_expansion = true;
    __atomic_add_fetch(&ctx->state_serial, 1, __ATOMIC_RELAXED);
    float mvp[16];
    glm_mat4_multiply(mvp, s->projection.m[s->projection.depth], s->modelview.m[s->modelview.depth]);
    for (uint32_t i = 0; i + 1 < draw->index_count; i += 2) {
        if (written + 2 * width > capacity) {
            streamed_batch(ctx, draw, copies, written);
            written = 0;
        }
        const float *ends[2];
        float clip[2][4];
        for (int e = 0; e < 2; ++e) {
            uint32_t at = draw->indices ? draw->indices[i + e] : i + e;
            ends[e] = draw->vertices + (size_t)at * stride;
            const float *p = ends[e] + GLM_ATTR_POSITION * 4;
            for (int r = 0; r < 4; ++r)
                clip[e][r] = mvp[r] * p[0] + mvp[4 + r] * p[1] + mvp[8 + r] * p[2] + mvp[12 + r] * p[3];
        }
        /* The common w0*w1 denominator cancels when comparing magnitudes.
           This also handles an endpoint on the eye plane without 0/0. */
        double dx = ((double)clip[1][0] * clip[0][3] - (double)clip[0][0] * clip[1][3]) * s->viewport[2];
        double dy = ((double)clip[1][1] * clip[0][3] - (double)clip[0][1] * clip[1][3]) * s->viewport[3];
        bool x_major = fabs(dx) >= fabs(dy);
        bool terminal = terminal_state;
        for (int e = 0; e < 2; ++e) {
            if (clip[e][3] != 1.0f) terminal = false;
            for (int c = 0; c < 3; ++c)
                if (!isfinite(clip[e][c]) || fabsf(clip[e][c]) >= clip[e][3]) terminal = false;
        }
        bool constant_colors = true;
        for (int slot = GLM_ATTR_COLOR; slot <= GLM_ATTR_SECONDARY_COLOR; ++slot)
            for (int c = 0; c < 4; ++c) {
                float a = ends[0][slot * 4 + c], b = ends[1][slot * 4 + c];
                if (!isfinite(a) || !isfinite(b)) terminal = false;
                if (a != b) constant_colors = false;
            }
        float points[16 * GLM_MAX_ATTRIBS * 4];
        unsigned point_count = 0;
        double window[2][2] = {{0}};
        if (terminal) {
            for (int e = 0; e < 2; ++e) for (int c = 0; c < 2; ++c) {
                window[e][c] = s->viewport[c] + (clip[e][c] * 0.5 + 0.5) * s->viewport[c + 2];
                if (!wide_terminal_viewport(clip[e][c], s->viewport[c], s->viewport[c + 2], window[e][c], 0)) terminal = false;
            }
        }
        bool axis_reconstructed = false;
        float axis_position[2] = {0};
        int major_axis = x_major ? 0 : 1, minor_axis = 1 - major_axis;
        if (terminal && constant_colors && window[0][minor_axis] == window[1][minor_axis]) {
            struct wide_axis_interval interval;
            if (wide_axis_bounds(window[0][major_axis], window[1][major_axis],
                                 s->viewport[major_axis], (uint32_t)s->viewport[major_axis + 2], &interval)) {
                if (!interval.count) continue;
                double target[2] = {interval.draw_start, interval.draw_end};
                bool stable = true;
                for (int e = 0; e < 2; ++e) {
                    axis_position[e] = (float)((target[e] - s->viewport[major_axis]) * 2 / s->viewport[major_axis + 2] - 1);
                    double actual = s->viewport[major_axis] + (axis_position[e] * 0.5 + 0.5) * s->viewport[major_axis + 2];
                    if (!wide_terminal_viewport(axis_position[e], s->viewport[major_axis],
                            s->viewport[major_axis + 2], actual, 0) ||
                        floor(actual * 256 + 0.5) != floor(target[e] * 256 + 0.5)) stable = false;
                    for (unsigned copy = 0; copy < width; ++copy) {
                        double offset = (double)copy - (double)(width - 1) * 0.5;
                        if (!wide_terminal_viewport(clip[e][minor_axis], s->viewport[minor_axis],
                                s->viewport[minor_axis + 2], window[e][minor_axis] + offset, offset)) stable = false;
                    }
                }
                axis_reconstructed = stable;
            }
        }
        bool transformed_trim = false;
        if (!terminal && trim_state)
            transformed_trim = wide_projected_interval(ctx, ends, mvp, x_major, window);
        bool interval_scissor = false, original_scissor_enabled = false;
        GLint original_scissor[4];
        if ((terminal || transformed_trim) && !axis_reconstructed) {
            double snapped_dx = floor(window[1][0] * 256 + 0.5) - floor(window[0][0] * 256 + 0.5);
            double snapped_dy = floor(window[1][1] * 256 + 0.5) - floor(window[0][1] * 256 + 0.5);
            bool stable_major = snapped_dx != 0 && snapped_dy != 0 &&
                fabs(snapped_dx) != fabs(snapped_dy) &&
                x_major == (fabs(snapped_dx) > fabs(snapped_dy));
            GLint box[4];
            original_scissor_enabled = ctx->state.scissor_test;
            memcpy(original_scissor, ctx->state.scissor, sizeof original_scissor);
            if (stable_major && wide_major_scissor(window[0][major_axis], window[1][major_axis], major_axis,
                                    original_scissor_enabled, original_scissor, box)) {
                if (!box[2] || !box[3]) continue;
                streamed_batch(ctx, draw, copies, written);
                written = 0;
                ctx->state.scissor_test = true;
                memcpy(ctx->state.scissor, box, sizeof box);
                __atomic_add_fetch(&ctx->state_serial, 1, __ATOMIC_RELAXED);
                interval_scissor = true;
            }
        }
        for (unsigned copy = 0; copy < width; ++copy) {
            float offset = (float)copy - (float)(width - 1) * 0.5f;
            for (int e = 0; e < 2; ++e) {
                float *v = copies + ((size_t)written + copy * 2 + e) * stride;
                memcpy(v, ends[e], stride * sizeof(float));
                if (axis_reconstructed) v[GLM_ATTR_POSITION * 4 + major_axis] = axis_position[e];
                v[GLM_ATTR_FOG * 4 + 1] = x_major ? 0 : offset * 2 / s->viewport[2];
                v[GLM_ATTR_FOG * 4 + 2] = x_major ? offset * 2 / s->viewport[3] : 0;
            }
        }
        written += 2 * width;
        if (terminal && !axis_reconstructed) for (unsigned copy = 0; copy < width; ++copy) {
            double offset = (double)copy - (double)(width - 1) * 0.5;
            bool stable = true;
            for (int e = 0; e < 2; ++e) for (int c = 0; c < 2; ++c) {
                double shift = ((c == 0) != x_major) ? offset : 0;
                if (!wide_terminal_viewport(clip[e][c], s->viewport[c], s->viewport[c + 2],
                                            window[e][c] + shift, shift)) stable = false;
            }
            if (!stable) continue;
            int px, py;
            if (!wide_terminal_pixel(window[0][0] + (x_major ? 0 : offset),
                                     window[0][1] + (x_major ? offset : 0),
                                     window[1][0] + (x_major ? 0 : offset),
                                     window[1][1] + (x_major ? offset : 0), x_major, &px, &py)) continue;
            if (!constant_colors) {
                double start_x = floor((window[0][0] + (x_major ? 0 : offset)) * 256 + 0.5) / 256;
                double start_y = floor((window[0][1] + (x_major ? offset : 0)) * 256 + 0.5) / 256;
                /* Reversal must exit this diamond. Very short segments or
                 * boundary equality retain the original rasterization. */
                if (!(fabs(start_x - px - 0.5) + fabs(start_y - py - 0.5) > 0.5)) continue;
                bool scissor_enabled = ctx->state.scissor_test;
                GLint scissor[4]; memcpy(scissor, ctx->state.scissor, sizeof scissor);
                if (scissor_enabled && (scissor[2] <= 0 || scissor[3] <= 0 ||
                    px < scissor[0] || py < scissor[1] ||
                    (int64_t)px >= (int64_t)scissor[0] + scissor[2] ||
                    (int64_t)py >= (int64_t)scissor[1] + scissor[3])) continue;
                /* Preserve primitive order and all hardware interpolation. */
                streamed_batch(ctx, draw, copies, written);
                written = 0;
                float reverse[2 * GLM_MAX_ATTRIBS * 4];
                float replica_offset = (float)copy - (float)(width - 1) * 0.5f;
                for (int e = 0; e < 2; ++e) {
                    float *v = reverse + (size_t)e * stride;
                    memcpy(v, ends[1 - e], stride * sizeof(float));
                    v[GLM_ATTR_FOG * 4 + 1] = x_major ? 0 : replica_offset * 2 / s->viewport[2];
                    v[GLM_ATTR_FOG * 4 + 2] = x_major ? replica_offset * 2 / s->viewport[3] : 0;
                }
                ctx->state.scissor_test = true;
                ctx->state.scissor[0] = px; ctx->state.scissor[1] = py;
                ctx->state.scissor[2] = ctx->state.scissor[3] = 1;
                __atomic_add_fetch(&ctx->state_serial, 1, __ATOMIC_RELAXED);
                struct glm_draw patch = *draw;
                patch.mode = GL_LINES; patch.metal_primitive = MTL_LINE;
                patch.vertices = reverse; patch.indices = NULL; patch.raster_indices = NULL;
                patch.original_vertex_ids = NULL;
                patch.vertex_count = patch.index_count = 2;
                glm_backend_draw(ctx, &patch);
                ctx->state.scissor_test = scissor_enabled;
                memcpy(ctx->state.scissor, scissor, sizeof scissor);
                __atomic_add_fetch(&ctx->state_serial, 1, __ATOMIC_RELAXED);
                continue;
            }
            float *point = points + (size_t)point_count++ * stride;
            memcpy(point, ends[1], stride * sizeof(float));
            point[GLM_ATTR_FOG * 4 + 1] = (float)((px + 0.5 - window[1][0]) * 2 / s->viewport[2]);
            point[GLM_ATTR_FOG * 4 + 2] = (float)((py + 0.5 - window[1][1]) * 2 / s->viewport[3]);
        }
        if (point_count) {
            /* Supplements belong to this line, before any later primitive. */
            streamed_batch(ctx, draw, copies, written);
            written = 0;
            struct glm_draw patch = *draw;
            patch.mode = GL_POINTS; patch.metal_primitive = MTL_POINT;
            patch.vertices = points; patch.indices = NULL; patch.raster_indices = NULL;
            patch.original_vertex_ids = NULL;
            patch.vertex_count = patch.index_count = point_count;
            glm_backend_draw(ctx, &patch);
        }
        if (interval_scissor) {
            streamed_batch(ctx, draw, copies, written);
            written = 0;
            ctx->state.scissor_test = original_scissor_enabled;
            memcpy(ctx->state.scissor, original_scissor, sizeof original_scissor);
            __atomic_add_fetch(&ctx->state_serial, 1, __ATOMIC_RELAXED);
        }
    }
    streamed_batch(ctx, draw, copies, written);
    ctx->wide_line_expansion = false;
    __atomic_add_fetch(&ctx->state_serial, 1, __ATOMIC_RELAXED);
    free(copies);
    return true;
}

/* `instance` is the gl_InstanceID of this draw: instanced draws are fetched
   and submitted one instance at a time. */
static void submit(struct glm_context *ctx, GLenum mode, float *vertices, uint32_t vertex_count,
                   const uint32_t *indices, uint32_t index_count, uint32_t instance, uint32_t vertex_id_base)
{
    uint32_t *seq = NULL;
    if (!indices) {
        seq = malloc((vertex_count ? vertex_count : 1) * sizeof(uint32_t));
        for (uint32_t i = 0; i < vertex_count; ++i) seq[i] = i;
        indices = seq;
        index_count = vertex_count;
    }
    struct glm_program *current = glm_current_program(ctx);
    if ((mode == GL_PATCHES) != (current && current->tess)) {
        free(seq);
        return glm_error(ctx, GL_INVALID_OPERATION);
    }
    if (current && current->result.gs && !gs_accepts(current->result.gs->in_vertices, mode)) {
        free(seq);
        return glm_error(ctx, GL_INVALID_OPERATION);
    }
    if (mode == GL_PATCHES) index_count -= index_count % (uint32_t)ctx->state.patch_vertices;
    struct index_list list = {0};
    struct glm_draw draw = {.mode = mode, .instance_count = 1, .base_instance = instance,
                            .vertex_id_base = vertex_id_base, .attrib_count = GLM_MAX_ATTRIBS,
                            .attrib_mask = (1u << GLM_MAX_ATTRIBS) - 1};
    glm_triangulate(mode, indices, index_count, &list, &draw.metal_primitive, current && current->gs);
    /* Polygons culled on both faces draw nothing, in any polygon mode. */
    if (draw.metal_primitive == MTL_TRIANGLE && ctx->state.cull_face && ctx->state.cull_mode == GL_FRONT_AND_BACK) {
        free(list.data);
        free(seq);
        return;
    }
    /* Polygon modes other than fill (Metal has one fill mode for both faces,
       no point fill, and outlines every triangle where GL outlines the
       polygon): each triangle is drawn as a triangle, its polygon's edges
       or its vertices by its face's mode, in up to three draws. Facing
       (needed for per-face modes and culling) is computed here with
       fixed-function vertex processing; programs without culling and with
       one mode for both faces need none. */
    const GLenum *pm = ctx->state.polygon_mode;
    bool fixed_function = !glm_current_program(ctx) && !ctx->state.vertex_program;
    if (draw.metal_primitive == MTL_TRIANGLE && list.count && !ctx->polygon_split && (pm[0] != GL_FILL || pm[1] != GL_FILL) &&
        (fixed_function || (pm[0] == pm[1] && !ctx->state.cull_face))) {
        const struct glm_state *st = &ctx->state;
        float mvp[16];
        glm_mat4_multiply(mvp, st->projection.m[st->projection.depth], st->modelview.m[st->modelview.depth]);
        struct index_list groups[3] = {{0}}; /* fill triangles, edges, points */
        uint8_t *point_seen = calloc(vertex_count ? vertex_count : 1, 1);
        uint32_t n = index_count;
        for (uint32_t t = 0; t + 2 < list.count; t += 3) {
            bool front = true;
            if (fixed_function) {
                float clip[3][4];
                for (int k = 0; k < 3; ++k) {
                    const float *v = vertices + (size_t)list.data[t + k] * GLM_MAX_ATTRIBS * 4 + GLM_ATTR_POSITION * 4;
                    for (int r = 0; r < 4; ++r)
                        clip[k][r] = mvp[r] * v[0] + mvp[4 + r] * v[1] + mvp[8 + r] * v[2] + mvp[12 + r] * v[3];
                }
                /* Orientation from homogeneous x, y, w (window y up as in GL). */
                double det = clip[0][0] * ((double)clip[1][1] * clip[2][3] - (double)clip[2][1] * clip[1][3]) -
                             clip[1][0] * ((double)clip[0][1] * clip[2][3] - (double)clip[2][1] * clip[0][3]) +
                             clip[2][0] * ((double)clip[0][1] * clip[1][3] - (double)clip[1][1] * clip[0][3]);
                front = (det > 0) == (st->front_face == GL_CCW);
                if (st->cull_face &&
                    (st->cull_mode == GL_FRONT_AND_BACK || (front ? st->cull_mode == GL_FRONT : st->cull_mode == GL_BACK)))
                    continue;
            }
            GLenum face_mode = pm[front ? 0 : 1];
            const uint32_t *v = list.data + t;
            if (face_mode == GL_FILL) {
                push3(&groups[0], v[0], v[1], v[2]);
            } else if (face_mode == GL_LINE) {
                /* The polygon's own edges: not the diagonals that split
                   quads and polygons into triangles. */
                uint32_t k = t / 3, edges = 7;
                if (mode == GL_QUADS) edges = k & 1 ? 3 /* (1,2)(2,3) */ : 5 /* (0,1)(3,0) */;
                else if (mode == GL_QUAD_STRIP) edges = k & 1 ? 6 /* (3,2)(2,0) */ : 3 /* (0,1)(1,3) */;
                else if (mode == GL_POLYGON) edges = 1u | (k == n - 3 ? 2u : 0u) | (k == 0 ? 4u : 0u);
                for (int e = 0; e < 3; ++e)
                    if (edges >> e & 1) { push(&groups[1], v[e]); push(&groups[1], v[(e + 1) % 3]); }
            } else {
                for (int k = 0; k < 3; ++k)
                    if (v[k] >= vertex_count || !point_seen[v[k]]) {
                        if (v[k] < vertex_count) point_seen[v[k]] = 1;
                        push(&groups[2], v[k]);
                    }
            }
        }
        free(point_seen);
        GLenum saved[2] = {pm[0], pm[1]};
        bool cull = st->cull_face;
        ctx->polygon_split = true;
        if (fixed_function) ctx->state.cull_face = false;
        static const GLenum modes[3] = {GL_TRIANGLES, GL_LINES, GL_POINTS};
        for (int g = 0; g < 3; ++g) {
            if (!groups[g].count) continue;
            /* Polygon offset by the mode these came from. */
            ctx->state.polygon_mode[0] = ctx->state.polygon_mode[1] = g == 0 ? GL_FILL : g == 1 ? GL_LINE : GL_POINT;
            __atomic_add_fetch(&ctx->state_serial, 1, __ATOMIC_RELAXED);
            submit(ctx, modes[g], vertices, vertex_count, groups[g].data, groups[g].count, instance, vertex_id_base);
            free(groups[g].data);
        }
        ctx->state.polygon_mode[0] = saved[0];
        ctx->state.polygon_mode[1] = saved[1];
        ctx->state.cull_face = cull;
        ctx->polygon_split = false;
        __atomic_add_fetch(&ctx->state_serial, 1, __ATOMIC_RELAXED);
        free(list.data);
        free(seq);
        return;
    }
    /* GL_POINT polygon mode: each polygon vertex once, as a point (Metal
       has no point fill mode). */
    if (draw.metal_primitive == MTL_TRIANGLE && ctx->state.polygon_mode[0] == GL_POINT &&
        ctx->state.polygon_mode[1] == GL_POINT && list.count) {
        uint32_t kept = 0;
        uint8_t *seen = calloc(vertex_count ? vertex_count : 1, 1);
        for (uint32_t i = 0; i < list.count; ++i) {
            uint32_t v = list.data[i];
            if (v < vertex_count && seen[v]) continue;
            if (v < vertex_count) seen[v] = 1;
            list.data[kept++] = v;
        }
        free(seen);
        list.count = kept;
        draw.metal_primitive = MTL_POINT;
    }
    /* Metal's flat varyings take the first vertex of each primitive; GL's
       the last (or first under GL_FIRST_VERTEX_CONVENTION). Rotating each
       triangle keeps its winding. */
    struct glm_program *program = glm_current_program(ctx);
    bool capture = glm_feedback_capturing(ctx);
    uint32_t *capture_order = NULL, *raster_order = NULL;
    bool flat_colors = program && program->color_inputs && ctx->state.shade_model == GL_FLAT && ctx->profile == GLM_PROFILE_LEGACY;
    if (program && !program->gs && !program->tess && (program->result.flat_inputs || flat_colors) &&
        draw.metal_primitive != MTL_POINT && list.count) {
        uint32_t per = draw.metal_primitive == MTL_LINE ? 2 : 3;
        if (capture) {
            capture_order = malloc((size_t)list.count * sizeof *capture_order);
            raster_order = malloc((size_t)list.count * sizeof *raster_order);
            if (!capture_order || !raster_order) {
                free(capture_order);
                free(raster_order);
                free(list.data);
                free(seq);
                return glm_error(ctx, GL_OUT_OF_MEMORY);
            }
            memcpy(capture_order, list.data, (size_t)list.count * sizeof *capture_order);
            for (uint32_t i = 0; i < list.count; ++i) raster_order[i] = i;
        }
        bool first_convention = ctx->state.provoking_vertex == GL_FIRST_VERTEX_CONVENTION;
        for (uint32_t prim = 0; prim < list.count / per; ++prim) {
            uint32_t *v = list.data + prim * per;
            uint32_t source = indices[provoking(mode, prim, index_count, first_convention)];
            for (uint32_t k = 1; k < per; ++k) {
                if (v[k] != source) continue;
                uint32_t *targets[2] = {v, raster_order ? raster_order + prim * per : NULL};
                for (int target = 0; target < 2; ++target) {
                    uint32_t *order = targets[target];
                    if (!order) continue;
                    if (per == 2) { uint32_t t = order[0]; order[0] = order[1]; order[1] = t; }
                    else if (k == 1) { uint32_t t = order[0]; order[0] = order[1]; order[1] = order[2]; order[2] = t; }
                    else { uint32_t t = order[2]; order[2] = order[1]; order[1] = order[0]; order[0] = t; }
                }
                break;
            }
        }
    }

    float *expanded = NULL;
    struct glm_program *geometry = glm_current_program(ctx);
    bool deindex = capture || (geometry && (geometry->gs || geometry->tess));
    bool flat = ctx->state.shade_model == GL_FLAT && ctx->profile == GLM_PROFILE_LEGACY &&
                draw.metal_primitive != MTL_POINT && !capture;
    if (deindex && list.count) {
        uint32_t original_base = draw.vertex_id_base;
        draw.vertex_id_base = 0; /* user IDs use the original-ID table when expanded */
        /* Transform feedback and geometry stages: de-indexed, in primitive
           order, so the vertex index is the output position. */
        uint32_t first = 0, kept = capture && !(geometry && geometry->gs) ? glm_feedback_reserve(ctx, list.count, &first) : list.count;
        if ((uint64_t)list.count * GLM_MAX_ATTRIBS * 4 * sizeof(float) > SIZE_MAX) {
            free(capture_order); free(raster_order); free(list.data); free(seq);
            return glm_error(ctx, GL_OUT_OF_MEMORY);
        }
        expanded = malloc((size_t)list.count * GLM_MAX_ATTRIBS * 4 * sizeof(float));
        if (!expanded) {
            free(capture_order); free(raster_order); free(list.data); free(seq);
            return glm_error(ctx, GL_OUT_OF_MEMORY);
        }
        uint32_t *original_ids = NULL;
        if (capture && !(geometry && (geometry->gs || geometry->tess))) {
            if ((uint64_t)list.count * sizeof(uint32_t) > SIZE_MAX) {
                free(expanded); free(capture_order); free(raster_order); free(list.data); free(seq);
                return glm_error(ctx, GL_OUT_OF_MEMORY);
            }
            original_ids = malloc((size_t)list.count * sizeof *original_ids);
            if (!original_ids) {
                free(expanded); free(capture_order); free(raster_order); free(list.data); free(seq);
                return glm_error(ctx, GL_OUT_OF_MEMORY);
            }
        }
        const uint32_t *order = capture_order ? capture_order : list.data;
        for (uint32_t i = 0; i < list.count; ++i) {
            if (original_ids) original_ids[i] = order[i] + original_base;
            memcpy(expanded + (size_t)i * GLM_MAX_ATTRIBS * 4, vertices + (size_t)order[i] * GLM_MAX_ATTRIBS * 4,
                   GLM_MAX_ATTRIBS * 4 * sizeof(float));
        }
        draw.original_vertex_ids = original_ids;
        draw.vertices = expanded;
        draw.vertex_count = list.count;
        draw.indices = NULL;
        draw.raster_indices = raster_order;
        draw.index_count = list.count;
        draw.capture = capture;
        draw.xfb_first = first;
        draw.xfb_count = kept;
        if (draw.index_count) glm_backend_draw(ctx, &draw);
        free(expanded);
        free(original_ids);
        free(capture_order);
        free(raster_order);
        free(list.data);
        free(seq);
        return;
    }
    if (flat && list.count) {
        draw.vertex_id_base = 0; /* provoking-colour expansion is not affine */
        /* De-index and copy the provoking vertex's colours into every vertex
           of its primitive. */
        uint32_t per = draw.metal_primitive == MTL_LINE ? 2 : 3;
        expanded = malloc((size_t)list.count * GLM_MAX_ATTRIBS * 4 * sizeof(float));
        for (uint32_t i = 0; i < list.count; ++i) {
            memcpy(expanded + (size_t)i * GLM_MAX_ATTRIBS * 4, vertices + (size_t)list.data[i] * GLM_MAX_ATTRIBS * 4,
                   GLM_MAX_ATTRIBS * 4 * sizeof(float));
        }
        for (uint32_t prim = 0; prim < list.count / per; ++prim) {
            uint32_t source = indices[provoking(mode, prim, index_count,
                                               ctx->state.provoking_vertex == GL_FIRST_VERTEX_CONVENTION)];
            const float *src = vertices + (size_t)source * GLM_MAX_ATTRIBS * 4;
            for (uint32_t k = 0; k < per; ++k) {
                float *dst = expanded + ((size_t)(prim * per + k)) * GLM_MAX_ATTRIBS * 4;
                memcpy(dst + GLM_ATTR_COLOR * 4, src + GLM_ATTR_COLOR * 4, 4 * sizeof(float));
                memcpy(dst + GLM_ATTR_SECONDARY_COLOR * 4, src + GLM_ATTR_SECONDARY_COLOR * 4, 4 * sizeof(float));
                if (ctx->state.lighting) memcpy(dst + GLM_ATTR_NORMAL * 4, src + GLM_ATTR_NORMAL * 4, 4 * sizeof(float));
            }
        }
        draw.vertices = expanded;
        draw.vertex_count = list.count;
        draw.indices = NULL;
        draw.index_count = list.count;
    } else {
        draw.vertices = vertices;
        draw.vertex_count = vertex_count;
        draw.indices = list.data;
        draw.index_count = list.count;
    }
    if (draw.index_count && !linear_clip_draw(ctx, &draw) && !wide_line_draw(ctx, &draw)) glm_backend_draw(ctx, &draw);
    free(expanded);
    free(list.data);
    free(seq);
}

/* ---- draw calls ---------------------------------------------------------- */

void glm_draw_arrays_internal(struct glm_context *ctx, GLenum mode, GLint first, GLsizei count, GLsizei instances)
{
    if (ctx->immediate) return glm_error(ctx, GL_INVALID_OPERATION);
    if (!valid_mode(mode)) return glm_error(ctx, GL_INVALID_ENUM);
    if (count < 0 || first < 0 || instances < 0) return glm_error(ctx, GL_INVALID_VALUE);
    if (!count || !instances || ctx->conditional_discard) return;
    if (!ctx->draw_framebuffer && !ctx->presents) return glm_error(ctx, GL_INVALID_FRAMEBUFFER_OPERATION);
    if (!glm_feedback_draw_allowed(ctx, mode)) return;
    if (!glm_feedback_capturing(ctx) && !wide_line_stream(ctx, mode) && !linear_clip_stream(ctx, mode) &&
        !glm_backend_clip_stream(ctx, mode) &&
        glm_backend_draw_gpu(ctx, mode, first, count, 0, NULL, instances, 0)) return;
    for (GLsizei instance = 0; instance < instances; ++instance) {
        float *vertices = fetch_vertices(ctx, (uint32_t)first, (uint32_t)count, (uint32_t)instance);
        if (!vertices) return glm_error(ctx, GL_OUT_OF_MEMORY);
        submit(ctx, mode, vertices, (uint32_t)count, NULL, 0, (uint32_t)instance, (uint32_t)first);
        free(vertices);
    }
}

static uint32_t read_index(const void *indices, GLenum type, GLsizei i)
{
    switch (type) {
    case GL_UNSIGNED_BYTE: return ((const uint8_t *)indices)[i];
    case GL_UNSIGNED_SHORT: return ((const uint16_t *)indices)[i];
    default: return ((const uint32_t *)indices)[i];
    }
}

/* ---- draws compiled into display lists ------------------------------------ */

GLM_HIDDEN void *glm_list_alloc(struct glm_context *ctx, size_t size, void (*exec)(const void *));

struct list_draw {
    GLenum mode;
    uint32_t vertex_count, index_count, instance, vertex_id_base;
    /* vertex_count * GLM_MAX_ATTRIBS float4s, then index_count indices */
};

static void list_draw_exec(const void *payload)
{
    const struct list_draw *d = payload;
    struct glm_context *ctx = glm_current();
    if (!ctx || ctx->immediate || ctx->conditional_discard) return;
    float *vertices = (float *)(d + 1);
    uint32_t *indices = d->index_count ? (uint32_t *)(vertices + (size_t)d->vertex_count * GLM_MAX_ATTRIBS * 4) : NULL;
    submit(ctx, d->mode, vertices, d->vertex_count, indices, d->index_count, d->instance, d->vertex_id_base);
}

static void list_draw_record(struct glm_context *ctx, GLenum mode, const float *vertices, uint32_t vertex_count,
                             const uint32_t *indices, uint32_t index_count, uint32_t instance, uint32_t vertex_id_base)
{
    size_t vertex_bytes = (size_t)vertex_count * GLM_MAX_ATTRIBS * 16;
    struct list_draw *d = glm_list_alloc(ctx, sizeof *d + vertex_bytes + (size_t)index_count * 4, list_draw_exec);
    d->mode = mode;
    d->vertex_count = vertex_count;
    d->index_count = index_count;
    d->instance = instance;
    d->vertex_id_base = vertex_id_base;
    memcpy(d + 1, vertices, vertex_bytes);
    if (index_count) memcpy((uint8_t *)(d + 1) + vertex_bytes, indices, (size_t)index_count * 4);
}

void glm_list_capture_draw(struct glm_context *ctx, GLenum mode, GLint first, GLsizei count, GLenum type,
                           const void *indices, GLsizei instances, GLint base_vertex)
{
    if (!valid_mode(mode) || count <= 0 || instances <= 0 || first < 0) return;
    if (!type) {
        for (GLsizei instance = 0; instance < instances; ++instance) {
            float *vertices = fetch_vertices(ctx, (uint32_t)first, (uint32_t)count, (uint32_t)instance);
            if (!vertices) return;
            list_draw_record(ctx, mode, vertices, (uint32_t)count, NULL, 0, (uint32_t)instance, (uint32_t)first);
            free(vertices);
        }
        return;
    }
    const void *source = indices;
    if (ctx->vao->element_buffer) {
        struct glm_buffer *buffer = glm_buffer_get(ctx, ctx->vao->element_buffer);
        const uint8_t *contents = buffer ? glm_backend_buffer_contents(ctx, buffer, false) : NULL;
        if (!contents) return;
        source = contents + (uintptr_t)indices;
    }
    if (!source) return;
    uint32_t low = UINT32_MAX, high = 0;
    for (GLsizei i = 0; i < count; ++i) {
        uint32_t index = read_index(source, type, i) + (uint32_t)base_vertex;
        if (index < low) low = index;
        if (index > high) high = index;
    }
    if (low > high) return;
    uint32_t *run = malloc((size_t)count * sizeof(uint32_t));
    for (GLsizei i = 0; i < count; ++i) run[i] = read_index(source, type, i) + (uint32_t)base_vertex - low;
    for (GLsizei instance = 0; instance < instances; ++instance) {
        float *vertices = fetch_vertices(ctx, low, high - low + 1, (uint32_t)instance);
        if (!vertices) break;
        list_draw_record(ctx, mode, vertices, high - low + 1, run, (uint32_t)count, (uint32_t)instance, low);
        free(vertices);
    }
    free(run);
}

void glm_draw_elements_internal(struct glm_context *ctx, GLenum mode, GLsizei count, GLenum type,
                                const void *indices, GLsizei instances, GLint base_vertex)
{
    if (ctx->immediate) return glm_error(ctx, GL_INVALID_OPERATION);
    if (!valid_mode(mode)) return glm_error(ctx, GL_INVALID_ENUM);
    if (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT)
        return glm_error(ctx, GL_INVALID_ENUM);
    if (ctx->conditional_discard) return;
    if (count < 0 || instances < 0) return glm_error(ctx, GL_INVALID_VALUE);
    if (!count || !instances) return;
    if (!ctx->draw_framebuffer && !ctx->presents) return glm_error(ctx, GL_INVALID_FRAMEBUFFER_OPERATION);
    if (!glm_feedback_draw_allowed(ctx, mode)) return;
    if (!glm_feedback_capturing(ctx) && !wide_line_stream(ctx, mode) && !linear_clip_stream(ctx, mode) &&
        !glm_backend_clip_stream(ctx, mode) &&
        glm_backend_draw_gpu(ctx, mode, 0, count, type, indices, instances, base_vertex))
        return;
    const void *source = indices;
    if (ctx->vao->element_buffer) {
        struct glm_buffer *buffer = glm_buffer_get(ctx, ctx->vao->element_buffer);
        const uint8_t *contents = buffer ? glm_backend_buffer_contents(ctx, buffer, false) : NULL;
        if (!contents) return glm_error(ctx, GL_INVALID_OPERATION);
        source = contents + (uintptr_t)indices;
    }
    if (!source) return;
    /* Split at primitive restart and fetch only the referenced range. */
    uint32_t restart = type == GL_UNSIGNED_BYTE ? 0xff : type == GL_UNSIGNED_SHORT ? 0xffff : 0xffffffff;
    bool use_restart = ctx->state.primitive_restart;
    if (use_restart) restart = ctx->state.primitive_restart_index;
    uint32_t low = UINT32_MAX, high = 0;
    for (GLsizei i = 0; i < count; ++i) {
        uint32_t index = read_index(source, type, i);
        if (use_restart && index == restart) continue;
        index += (uint32_t)base_vertex;
        if (index < low) low = index;
        if (index > high) high = index;
    }
    if (low > high) return;
    for (GLsizei instance = 0; instance < instances; ++instance) {
        float *vertices = fetch_vertices(ctx, low, high - low + 1, (uint32_t)instance);
        if (!vertices) return glm_error(ctx, GL_OUT_OF_MEMORY);
        uint32_t *run = malloc((size_t)count * sizeof(uint32_t));
        GLsizei start = 0;
        while (start < count) {
            uint32_t n = 0;
            GLsizei i = start;
            for (; i < count; ++i) {
                uint32_t index = read_index(source, type, i);
                if (use_restart && index == restart) break;
                run[n++] = index + (uint32_t)base_vertex - low;
            }
            if (n) submit(ctx, mode, vertices, high - low + 1, run, n, (uint32_t)instance, low);
            start = i + 1;
        }
        free(run);
        free(vertices);
    }
}

GLM_EXPORT void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{ GLM_CONTEXT(ctx); glm_draw_arrays_internal(ctx, mode, first, count, 1); }
GLM_EXPORT void glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances)
{ GLM_CONTEXT(ctx); glm_draw_arrays_internal(ctx, mode, first, count, instances); }
GLM_EXPORT void glDrawArraysInstancedARB(GLenum mode, GLint first, GLsizei count, GLsizei instances)
{ glDrawArraysInstanced(mode, first, count, instances); }

GLM_EXPORT void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{ GLM_CONTEXT(ctx); glm_draw_elements_internal(ctx, mode, count, type, indices, 1, 0); }
GLM_EXPORT void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const GLvoid *indices)
{ (void)start; (void)end; glDrawElements(mode, count, type, indices); }
GLM_EXPORT void glDrawRangeElementsEXT(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const GLvoid *indices)
{ glDrawRangeElements(mode, start, end, count, type, indices); }
GLM_EXPORT void glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices, GLsizei instances)
{ GLM_CONTEXT(ctx); glm_draw_elements_internal(ctx, mode, count, type, indices, instances, 0); }
GLM_EXPORT void glDrawElementsInstancedARB(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices, GLsizei instances)
{ glDrawElementsInstanced(mode, count, type, indices, instances); }
GLM_EXPORT void glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices, GLint base)
{ GLM_CONTEXT(ctx); glm_draw_elements_internal(ctx, mode, count, type, indices, 1, base); }
GLM_EXPORT void glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                              const GLvoid *indices, GLint base)
{ (void)start; (void)end; glDrawElementsBaseVertex(mode, count, type, indices, base); }
GLM_EXPORT void glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices,
                                                  GLsizei instances, GLint base)
{ GLM_CONTEXT(ctx); glm_draw_elements_internal(ctx, mode, count, type, indices, instances, base); }

GLM_EXPORT void glMultiDrawArrays(GLenum mode, const GLint *first, const GLsizei *count, GLsizei draws)
{ for (GLsizei i = 0; i < draws; ++i) glDrawArrays(mode, first[i], count[i]); }
GLM_EXPORT void glMultiDrawElements(GLenum mode, const GLsizei *count, GLenum type, const GLvoid *const *indices, GLsizei draws)
{ for (GLsizei i = 0; i < draws; ++i) glDrawElements(mode, count[i], type, indices[i]); }

GLM_EXPORT void glMultiDrawElementsBaseVertex(GLenum mode, const GLsizei *count, GLenum type, const GLvoid *const *indices,
                                              GLsizei draws, const GLint *base)
{ for (GLsizei i = 0; i < draws; ++i) glDrawElementsBaseVertex(mode, count[i], type, indices[i], base[i]); }

/* ---- indirect draws (ARB_draw_indirect): the command is read from the
   GL_DRAW_INDIRECT_BUFFER on the CPU, then drawn directly ---------------- */

static const GLuint *indirect_command(struct glm_context *ctx, const void *offset, size_t size)
{
    if (ctx->profile == GLM_PROFILE_CORE && !ctx->vao->name) {
        glm_error(ctx, GL_INVALID_OPERATION);
        return NULL;
    }
    struct glm_buffer *buffer = ctx->draw_indirect_buffer ? glm_buffer_get(ctx, ctx->draw_indirect_buffer) : NULL;
    if (!buffer || (uintptr_t)offset % 4) {
        glm_error(ctx, GL_INVALID_OPERATION);
        return NULL;
    }
    if ((uintptr_t)offset > (size_t)buffer->size || size > (size_t)buffer->size - (uintptr_t)offset) {
        glm_error(ctx, GL_INVALID_OPERATION);
        return NULL;
    }
    const uint8_t *contents = glm_backend_buffer_contents(ctx, buffer, false);
    return contents ? (const GLuint *)(contents + (uintptr_t)offset) : NULL;
}

GLM_EXPORT void glDrawArraysIndirect(GLenum mode, const GLvoid *indirect)
{
    GLM_CONTEXT(ctx);
    /* count, instances, first, base instance (must be 0 in GL 4.1) */
    const GLuint *c = indirect_command(ctx, indirect, 4 * sizeof(GLuint));
    if (!c || !c[1]) return;
    glm_draw_arrays_internal(ctx, mode, (GLint)c[2], (GLsizei)c[0], (GLsizei)c[1]);
}

GLM_EXPORT void glDrawElementsIndirect(GLenum mode, GLenum type, const GLvoid *indirect)
{
    GLM_CONTEXT(ctx);
    /* count, instances, first index, base vertex, base instance */
    const GLuint *c = indirect_command(ctx, indirect, 5 * sizeof(GLuint));
    if (!c || !c[1]) return;
    size_t index_size = type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : 4;
    glm_draw_elements_internal(ctx, mode, (GLsizei)c[0], type, (const void *)(uintptr_t)(c[2] * index_size), (GLsizei)c[1],
                               (GLint)c[3]);
}

GLM_EXPORT void glArrayElement(GLint i)
{
    GLM_CONTEXT(ctx);
    if (!ctx->immediate) return;
    float *v = fetch_vertices(ctx, (uint32_t)i, 1, 0);
    if (!v) return;
    for (int slot = 1; slot < GLM_MAX_ATTRIBS; ++slot)
        if (ctx->vao->arrays[slot].enabled) memcpy(ctx->state.current[slot], v + slot * 4, 4 * sizeof(float));
    if (ctx->vao->arrays[GLM_ATTR_POSITION].enabled) emit_vertex(ctx, v[0], v[1], v[2], v[3]);
    free(v);
}

GLM_EXPORT void glProvokingVertex(GLenum mode)
{
    GLM_CONTEXT(ctx);
    if (mode != GL_FIRST_VERTEX_CONVENTION && mode != GL_LAST_VERTEX_CONVENTION) return glm_error(ctx, GL_INVALID_ENUM);
    ctx->state.provoking_vertex = mode;
}
GLM_EXPORT void glProvokingVertexEXT(GLenum mode) { glProvokingVertex(mode); }

GLM_EXPORT void glPatchParameteri(GLenum pname, GLint value)
{
    GLM_CONTEXT(ctx);
    if (pname != GL_PATCH_VERTICES) return glm_error(ctx, GL_INVALID_ENUM);
    if (value <= 0 || value > GLM_MAX_PATCH_VERTICES) return glm_error(ctx, GL_INVALID_VALUE);
    ctx->state.patch_vertices = value;
}

GLM_EXPORT void glPatchParameterfv(GLenum pname, const GLfloat *values)
{
    GLM_CONTEXT(ctx);
    if (pname == GL_PATCH_DEFAULT_OUTER_LEVEL) memcpy(ctx->state.patch_default_outer, values, 4 * sizeof(float));
    else if (pname == GL_PATCH_DEFAULT_INNER_LEVEL) memcpy(ctx->state.patch_default_inner, values, 2 * sizeof(float));
    else glm_error(ctx, GL_INVALID_ENUM);
}
