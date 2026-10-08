/* Extension-suffixed names of core entry points Apple's OpenGL.framework
   still exports, and Apple extensions that need no work on Metal. */
#include "glm_internal.h"

#include <string.h>

/* Core entry points gl.h (legacy) does not declare. */
void glBlendEquationi(GLuint buf, GLenum mode);
void glBlendEquationSeparatei(GLuint buf, GLenum rgb, GLenum alpha);
void glBlendFunci(GLuint buf, GLenum src, GLenum dst);
void glBlendFuncSeparatei(GLuint buf, GLenum sr, GLenum dr, GLenum sa, GLenum da);
void glColorMaski(GLuint buf, GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void glEnablei(GLenum cap, GLuint index);
void glDisablei(GLenum cap, GLuint index);
GLboolean glIsEnabledi(GLenum cap, GLuint index);
void glGetBooleani_v(GLenum pname, GLuint index, GLboolean *data);
void glGetIntegeri_v(GLenum pname, GLuint index, GLint *data);
void glTexParameterIiv(GLenum target, GLenum pname, const GLint *params);
void glTexParameterIuiv(GLenum target, GLenum pname, const GLuint *params);
void glGetTexParameterIiv(GLenum target, GLenum pname, GLint *params);
void glGetTexParameterIuiv(GLenum target, GLenum pname, GLuint *params);
void glFramebufferTexture(GLenum target, GLenum attachment, GLuint texture, GLint level);
void glFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer);
void glProgramParameteri(GLuint program, GLenum pname, GLint value);
void glBeginConditionalRender(GLuint id, GLenum mode);
void glEndConditionalRender(void);
void glUniform1ui(GLint l, GLuint x);
void glUniform2ui(GLint l, GLuint x, GLuint y);
void glUniform3ui(GLint l, GLuint x, GLuint y, GLuint z);
void glUniform4ui(GLint l, GLuint x, GLuint y, GLuint z, GLuint w);
void glUniform1uiv(GLint l, GLsizei n, const GLuint *v);
void glUniform2uiv(GLint l, GLsizei n, const GLuint *v);
void glUniform3uiv(GLint l, GLsizei n, const GLuint *v);
void glUniform4uiv(GLint l, GLsizei n, const GLuint *v);
void glGetUniformuiv(GLuint program, GLint location, GLuint *params);
void glVertexAttribI1i(GLuint i, GLint x);
void glVertexAttribI2i(GLuint i, GLint x, GLint y);
void glVertexAttribI3i(GLuint i, GLint x, GLint y, GLint z);
void glVertexAttribI4i(GLuint i, GLint x, GLint y, GLint z, GLint w);
void glVertexAttribI1ui(GLuint i, GLuint x);
void glVertexAttribI2ui(GLuint i, GLuint x, GLuint y);
void glVertexAttribI3ui(GLuint i, GLuint x, GLuint y, GLuint z);
void glVertexAttribI4ui(GLuint i, GLuint x, GLuint y, GLuint z, GLuint w);
void glVertexAttribI1iv(GLuint i, const GLint *v);
void glVertexAttribI2iv(GLuint i, const GLint *v);
void glVertexAttribI3iv(GLuint i, const GLint *v);
void glVertexAttribI4iv(GLuint i, const GLint *v);
void glVertexAttribI1uiv(GLuint i, const GLuint *v);
void glVertexAttribI2uiv(GLuint i, const GLuint *v);
void glVertexAttribI3uiv(GLuint i, const GLuint *v);
void glVertexAttribI4uiv(GLuint i, const GLuint *v);
void glVertexAttribI4bv(GLuint i, const GLbyte *v);
void glVertexAttribI4sv(GLuint i, const GLshort *v);
void glVertexAttribI4ubv(GLuint i, const GLubyte *v);
void glVertexAttribI4usv(GLuint i, const GLushort *v);
void glVertexAttribIPointer(GLuint i, GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void glGetVertexAttribIiv(GLuint i, GLenum pname, GLint *params);
void glGetVertexAttribIuiv(GLuint i, GLenum pname, GLuint *params);

/* ---- blending and per-buffer state (EXT_blend_*, ARB_draw_buffers_blend,
        EXT_draw_buffers2) ---------------------------------------------------- */

GLM_EXPORT void glBlendColorEXT(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { glBlendColor(r, g, b, a); }
GLM_EXPORT void glBlendEquationEXT(GLenum mode) { glBlendEquation(mode); }
GLM_EXPORT void glBlendEquationSeparateEXT(GLenum rgb, GLenum alpha) { glBlendEquationSeparate(rgb, alpha); }
GLM_EXPORT void glBlendEquationSeparateATI(GLenum rgb, GLenum alpha) { glBlendEquationSeparate(rgb, alpha); }
GLM_EXPORT void glBlendFuncSeparateEXT(GLenum sr, GLenum dr, GLenum sa, GLenum da) { glBlendFuncSeparate(sr, dr, sa, da); }
GLM_EXPORT void glBlendEquationiARB(GLuint buf, GLenum mode) { glBlendEquationi(buf, mode); }
GLM_EXPORT void glBlendEquationSeparateiARB(GLuint buf, GLenum rgb, GLenum alpha) { glBlendEquationSeparatei(buf, rgb, alpha); }
GLM_EXPORT void glBlendFunciARB(GLuint buf, GLenum src, GLenum dst) { glBlendFunci(buf, src, dst); }
GLM_EXPORT void glBlendFuncSeparateiARB(GLuint buf, GLenum sr, GLenum dr, GLenum sa, GLenum da)
{
    glBlendFuncSeparatei(buf, sr, dr, sa, da);
}
GLM_EXPORT void glColorMaskIndexedEXT(GLuint buf, GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    glColorMaski(buf, r, g, b, a);
}
GLM_EXPORT void glEnableIndexedEXT(GLenum cap, GLuint index) { glEnablei(cap, index); }
GLM_EXPORT void glDisableIndexedEXT(GLenum cap, GLuint index) { glDisablei(cap, index); }
GLM_EXPORT GLboolean glIsEnabledIndexedEXT(GLenum cap, GLuint index) { return glIsEnabledi(cap, index); }
GLM_EXPORT void glGetBooleanIndexedvEXT(GLenum pname, GLuint index, GLboolean *data) { glGetBooleani_v(pname, index, data); }
GLM_EXPORT void glGetIntegerIndexedvEXT(GLenum pname, GLuint index, GLint *data) { glGetIntegeri_v(pname, index, data); }

/* ---- textures ------------------------------------------------------------- */

GLM_EXPORT void glCompressedTexImage1DARB(GLenum target, GLint level, GLenum internal, GLsizei w, GLint border,
                                          GLsizei size, const GLvoid *data)
{
    glCompressedTexImage1D(target, level, internal, w, border, size, data);
}
GLM_EXPORT void glCompressedTexImage3DARB(GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLsizei d,
                                          GLint border, GLsizei size, const GLvoid *data)
{
    glCompressedTexImage3D(target, level, internal, w, h, d, border, size, data);
}
GLM_EXPORT void glCompressedTexSubImage1DARB(GLenum target, GLint level, GLint x, GLsizei w, GLenum format, GLsizei size,
                                             const GLvoid *data)
{
    glCompressedTexSubImage1D(target, level, x, w, format, size, data);
}
GLM_EXPORT void glCompressedTexSubImage3DARB(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h,
                                             GLsizei d, GLenum format, GLsizei size, const GLvoid *data)
{
    glCompressedTexSubImage3D(target, level, x, y, z, w, h, d, format, size, data);
}
GLM_EXPORT void glTexParameterIivEXT(GLenum target, GLenum pname, GLint *params) { glTexParameterIiv(target, pname, params); }
GLM_EXPORT void glTexParameterIuivEXT(GLenum target, GLenum pname, GLuint *params) { glTexParameterIuiv(target, pname, params); }
GLM_EXPORT void glGetTexParameterIivEXT(GLenum target, GLenum pname, GLint *params) { glGetTexParameterIiv(target, pname, params); }
GLM_EXPORT void glGetTexParameterIuivEXT(GLenum target, GLenum pname, GLuint *params)
{
    glGetTexParameterIuiv(target, pname, params);
}
/* APPLE_texture_range / client storage hints: storage is always Metal's. */
GLM_EXPORT void glTextureRangeAPPLE(GLenum target, GLsizei length, const GLvoid *pointer)
{
    (void)target, (void)length, (void)pointer;
}
GLM_EXPORT void glGetTexParameterPointervAPPLE(GLenum target, GLenum pname, GLvoid **params)
{
    (void)target, (void)pname;
    *params = NULL;
}
/* NV_texture_barrier: a new render pass makes earlier writes visible to
   later texture reads. */
GLM_EXPORT void glTextureBarrierNV(void)
{
    GLM_CONTEXT(ctx);
    glm_backend_framebuffer_changed(ctx);
}

/* ---- framebuffers and programs ------------------------------------------- */

GLM_EXPORT void glFramebufferTextureEXT(GLenum target, GLenum attachment, GLuint texture, GLint level)
{
    glFramebufferTexture(target, attachment, texture, level);
}
GLM_EXPORT void glFramebufferTextureFaceEXT(GLenum target, GLenum attachment, GLuint texture, GLint level, GLenum face)
{
    glFramebufferTextureLayer(target, attachment, texture, level, (GLint)(face - GL_TEXTURE_CUBE_MAP_POSITIVE_X));
}
GLM_EXPORT void glProgramParameteriEXT(GLuint program, GLenum pname, GLint value) { glProgramParameteri(program, pname, value); }
GLM_EXPORT void glBeginConditionalRenderNV(GLuint id, GLenum mode) { glBeginConditionalRender(id, mode); }
GLM_EXPORT void glEndConditionalRenderNV(void) { glEndConditionalRender(); }

/* ---- uniforms and vertex attributes (EXT_gpu_shader4) ---------------------- */

GLM_EXPORT void glUniform1uiEXT(GLint l, GLuint x) { glUniform1ui(l, x); }
GLM_EXPORT void glUniform2uiEXT(GLint l, GLuint x, GLuint y) { glUniform2ui(l, x, y); }
GLM_EXPORT void glUniform3uiEXT(GLint l, GLuint x, GLuint y, GLuint z) { glUniform3ui(l, x, y, z); }
GLM_EXPORT void glUniform4uiEXT(GLint l, GLuint x, GLuint y, GLuint z, GLuint w) { glUniform4ui(l, x, y, z, w); }
GLM_EXPORT void glUniform1uivEXT(GLint l, GLsizei n, const GLuint *v) { glUniform1uiv(l, n, v); }
GLM_EXPORT void glUniform2uivEXT(GLint l, GLsizei n, const GLuint *v) { glUniform2uiv(l, n, v); }
GLM_EXPORT void glUniform3uivEXT(GLint l, GLsizei n, const GLuint *v) { glUniform3uiv(l, n, v); }
GLM_EXPORT void glUniform4uivEXT(GLint l, GLsizei n, const GLuint *v) { glUniform4uiv(l, n, v); }
GLM_EXPORT void glGetUniformuivEXT(GLuint program, GLint location, GLuint *params) { glGetUniformuiv(program, location, params); }

GLM_EXPORT void glVertexAttribI1iEXT(GLuint i, GLint x) { glVertexAttribI1i(i, x); }
GLM_EXPORT void glVertexAttribI2iEXT(GLuint i, GLint x, GLint y) { glVertexAttribI2i(i, x, y); }
GLM_EXPORT void glVertexAttribI3iEXT(GLuint i, GLint x, GLint y, GLint z) { glVertexAttribI3i(i, x, y, z); }
GLM_EXPORT void glVertexAttribI4iEXT(GLuint i, GLint x, GLint y, GLint z, GLint w) { glVertexAttribI4i(i, x, y, z, w); }
GLM_EXPORT void glVertexAttribI1uiEXT(GLuint i, GLuint x) { glVertexAttribI1ui(i, x); }
GLM_EXPORT void glVertexAttribI2uiEXT(GLuint i, GLuint x, GLuint y) { glVertexAttribI2ui(i, x, y); }
GLM_EXPORT void glVertexAttribI3uiEXT(GLuint i, GLuint x, GLuint y, GLuint z) { glVertexAttribI3ui(i, x, y, z); }
GLM_EXPORT void glVertexAttribI4uiEXT(GLuint i, GLuint x, GLuint y, GLuint z, GLuint w) { glVertexAttribI4ui(i, x, y, z, w); }
GLM_EXPORT void glVertexAttribI1ivEXT(GLuint i, const GLint *v) { glVertexAttribI1iv(i, v); }
GLM_EXPORT void glVertexAttribI2ivEXT(GLuint i, const GLint *v) { glVertexAttribI2iv(i, v); }
GLM_EXPORT void glVertexAttribI3ivEXT(GLuint i, const GLint *v) { glVertexAttribI3iv(i, v); }
GLM_EXPORT void glVertexAttribI4ivEXT(GLuint i, const GLint *v) { glVertexAttribI4iv(i, v); }
GLM_EXPORT void glVertexAttribI1uivEXT(GLuint i, const GLuint *v) { glVertexAttribI1uiv(i, v); }
GLM_EXPORT void glVertexAttribI2uivEXT(GLuint i, const GLuint *v) { glVertexAttribI2uiv(i, v); }
GLM_EXPORT void glVertexAttribI3uivEXT(GLuint i, const GLuint *v) { glVertexAttribI3uiv(i, v); }
GLM_EXPORT void glVertexAttribI4uivEXT(GLuint i, const GLuint *v) { glVertexAttribI4uiv(i, v); }
GLM_EXPORT void glVertexAttribI4bvEXT(GLuint i, const GLbyte *v) { glVertexAttribI4bv(i, v); }
GLM_EXPORT void glVertexAttribI4svEXT(GLuint i, const GLshort *v) { glVertexAttribI4sv(i, v); }
GLM_EXPORT void glVertexAttribI4ubvEXT(GLuint i, const GLubyte *v) { glVertexAttribI4ubv(i, v); }
GLM_EXPORT void glVertexAttribI4usvEXT(GLuint i, const GLushort *v) { glVertexAttribI4usv(i, v); }
GLM_EXPORT void glVertexAttribIPointerEXT(GLuint i, GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
    glVertexAttribIPointer(i, size, type, stride, pointer);
}
GLM_EXPORT void glGetVertexAttribIivEXT(GLuint i, GLenum pname, GLint *params) { glGetVertexAttribIiv(i, pname, params); }
GLM_EXPORT void glGetVertexAttribIuivEXT(GLuint i, GLenum pname, GLuint *params) { glGetVertexAttribIuiv(i, pname, params); }

/* ---- matrices, buffers, draws --------------------------------------------- */

GLM_EXPORT void glLoadTransposeMatrixfARB(const GLfloat *m) { glLoadTransposeMatrixf(m); }
GLM_EXPORT void glLoadTransposeMatrixdARB(const GLdouble *m) { glLoadTransposeMatrixd(m); }
GLM_EXPORT void glMultTransposeMatrixfARB(const GLfloat *m) { glMultTransposeMatrixf(m); }
GLM_EXPORT void glMultTransposeMatrixdARB(const GLdouble *m) { glMultTransposeMatrixd(m); }
GLM_EXPORT void glGetBufferSubDataARB(GLenum target, GLintptrARB offset, GLsizeiptrARB size, GLvoid *data)
{
    glGetBufferSubData(target, offset, size, data);
}
GLM_EXPORT void glGetBufferPointervARB(GLenum target, GLenum pname, GLvoid **params) { glGetBufferPointerv(target, pname, params); }
GLM_EXPORT void glMultiDrawArraysEXT(GLenum mode, const GLint *first, const GLsizei *count, GLsizei n)
{
    glMultiDrawArrays(mode, first, count, n);
}
GLM_EXPORT void glMultiDrawElementsEXT(GLenum mode, const GLsizei *count, GLenum type, const GLvoid *const *indices, GLsizei n)
{
    glMultiDrawElements(mode, count, type, indices, n);
}

/* ---- Apple extensions with nothing to do on Metal -------------------------- */

/* APPLE_vertex_array_range: client arrays are read at draw time anyway. */
GLM_EXPORT void glVertexArrayRangeAPPLE(GLsizei length, const GLvoid *pointer) { (void)length, (void)pointer; }
GLM_EXPORT void glFlushVertexArrayRangeAPPLE(GLsizei length, const GLvoid *pointer) { (void)length, (void)pointer; }
GLM_EXPORT void glVertexArrayParameteriAPPLE(GLenum pname, GLint param) { (void)pname, (void)param; }
/* EXT_compiled_vertex_array: a hint. */
GLM_EXPORT void glLockArraysEXT(GLint first, GLsizei count) { (void)first, (void)count; }
GLM_EXPORT void glUnlockArraysEXT(void) {}
/* APPLE_object_purgeable: objects are never purged. */
GLM_EXPORT GLenum glObjectPurgeableAPPLE(GLenum type, GLuint name, GLenum option)
{
    (void)type, (void)name;
    return option == GL_RELEASED_APPLE ? GL_RELEASED_APPLE : GL_VOLATILE_APPLE;
}
GLM_EXPORT GLenum glObjectUnpurgeableAPPLE(GLenum type, GLuint name, GLenum option)
{
    (void)type, (void)name, (void)option;
    return GL_RETAINED_APPLE;
}
GLM_EXPORT void glGetObjectParameterivAPPLE(GLenum type, GLuint name, GLenum pname, GLint *params)
{
    (void)type, (void)name, (void)pname;
    *params = GL_FALSE;
}
GLM_EXPORT void glFlushRenderAPPLE(void) { glFlush(); }
GLM_EXPORT void glFinishRenderAPPLE(void) { glFinish(); }

/* ---- EXT_debug_marker / EXT_debug_label ---------------------------------- */

GLM_EXPORT void glInsertEventMarkerEXT(GLsizei length, const char *marker) { (void)length, (void)marker; }
GLM_EXPORT void glPushGroupMarkerEXT(GLsizei length, const GLchar *marker) { (void)length, (void)marker; }
GLM_EXPORT void glPopGroupMarkerEXT(void) {}
GLM_EXPORT void glLabelObjectEXT(GLenum type, GLuint object, GLsizei length, const GLchar *label)
{
    (void)type, (void)object, (void)length, (void)label;
}
GLM_EXPORT void glGetObjectLabelEXT(GLenum type, GLuint object, GLsizei size, GLsizei *length, GLchar *label)
{
    (void)type, (void)object;
    if (length) *length = 0;
    if (label && size > 0) label[0] = 0;
}
