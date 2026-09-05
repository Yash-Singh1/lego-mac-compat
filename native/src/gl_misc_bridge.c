#include "gl_misc_bridge.h"
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl.h>
#include <string.h>

static GLfloat float_arg(uint32_t word)
{
    GLfloat value; memcpy(&value, &word, sizeof(value)); return value;
}

int gl_misc_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *r)
{
    if (*name == '_') ++name;
#define IS(s) (!strcmp(name, s))
#define PTR(i) ((void *)(uintptr_t)a[i])
    *r = 0;
    if (IS("glPixelStorei")) glPixelStorei(a[0], a[1]);
    else if (IS("glGetBooleanv")) glGetBooleanv(a[0], PTR(1));
    else if (IS("glGetDoublev")) glGetDoublev(a[0], PTR(1));
    else if (IS("glGetTexLevelParameteriv")) glGetTexLevelParameteriv(a[0], a[1], a[2], PTR(3));
    else if (IS("glGetTexImage")) glGetTexImage(a[0], a[1], a[2], a[3], PTR(4));
    else if (IS("glGetCompressedTexImage")) glGetCompressedTexImage(a[0], a[1], PTR(2));
    else if (IS("glTexSubImage2D")) glTexSubImage2D(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], PTR(8));
    else if (IS("glCompressedTexImage3D")) glCompressedTexImage3D(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], PTR(8));
    else if (IS("glIsEnabled")) *r = glIsEnabled(a[0]);
    else if (IS("glIsTexture")) *r = glIsTexture(a[0]);
    else if (IS("glPolygonMode")) glPolygonMode(a[0], a[1]);
    else if (IS("glPushAttrib")) glPushAttrib(a[0]);
    else if (IS("glPopAttrib")) glPopAttrib();
    else if (IS("glEnableClientState")) glEnableClientState(a[0]);
    else if (IS("glDisableClientState")) glDisableClientState(a[0]);
    else if (IS("glVertexPointer")) glVertexPointer(a[0], a[1], a[2], PTR(3));
    else if (IS("glTexCoordPointer")) glTexCoordPointer(a[0], a[1], a[2], PTR(3));
    else if (IS("glTexCoord2f")) glTexCoord2f(float_arg(a[0]), float_arg(a[1]));
    else if (IS("glColor4sv")) glColor4sv(PTR(0));
    else if (IS("glClipPlane")) glClipPlane(a[0], PTR(1));
    else if (IS("glDepthRange")) {
        GLdouble values[2]; memcpy(values, a, sizeof(values)); glDepthRange(values[0], values[1]);
    } else if (IS("glOrtho")) {
        GLdouble values[6]; memcpy(values, a, sizeof(values));
        glOrtho(values[0], values[1], values[2], values[3], values[4], values[5]);
    } else if (IS("glColorMaskIndexedEXT")) glColorMaskIndexedEXT(a[0], a[1], a[2], a[3], a[4]);
    else if (IS("glEnableIndexedEXT")) glEnableIndexedEXT(a[0], a[1]);
    else if (IS("glDisableIndexedEXT")) glDisableIndexedEXT(a[0], a[1]);
    else if (IS("glGetBooleanIndexedvEXT")) glGetBooleanIndexedvEXT(a[0], a[1], PTR(2));
    else if (IS("glFramebufferTexture3D") || IS("glFramebufferTexture3DEXT")) glFramebufferTexture3DEXT(a[0], a[1], a[2], a[3], a[4], a[5]);
    else if (IS("glRenderbufferStorageMultisample") || IS("glRenderbufferStorageMultisampleEXT")) glRenderbufferStorageMultisampleEXT(a[0], a[1], a[2], a[3], a[4]);
    else if (IS("glGenFencesAPPLE")) glGenFencesAPPLE(a[0], PTR(1));
    else if (IS("glDeleteFencesAPPLE")) glDeleteFencesAPPLE(a[0], PTR(1));
    else if (IS("glSetFenceAPPLE")) glSetFenceAPPLE(a[0]);
    else if (IS("glTestFenceAPPLE")) *r = glTestFenceAPPLE(a[0]);
    else if (IS("glFinishFenceAPPLE")) glFinishFenceAPPLE(a[0]);
    else if (IS("glTextureRangeAPPLE")) glTextureRangeAPPLE(a[0], a[1], PTR(2));
    else if (IS("glGetTexParameterPointervAPPLE")) {
        void *value = NULL; glGetTexParameterPointervAPPLE(a[0], a[1], &value);
        if ((uintptr_t)value > UINT32_MAX) return 0;
        *(uint32_t *)PTR(2) = (uint32_t)(uintptr_t)value;
    } else if (IS("glUniformBufferEXT")) glUniformBufferEXT(a[0], a[1], a[2]);
    else return 0;
    return 1;
#undef IS
#undef PTR
}
