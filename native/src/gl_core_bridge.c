/* Typed OpenGL ABI adapters. Signatures follow the macOS OpenGL headers.
 * Only used after the existing stateful GL bridge declines an entry point.
 * No native pointer or host-sized integer is returned to guest memory. */
#include "gl_core_bridge.h"
#include "objc_bridge.h"
#include <dlfcn.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
static void *symbols[149];
static float f32(uint32_t w){float f;memcpy(&f,&w,4);return f;}
static double f64(const uint32_t *w){double f;memcpy(&f,w,8);return f;}
static uint64_t call_glAttachShader(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[0])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glBeginTransformFeedback(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[1])((uint32_t)a[0]);return 0;
}
static uint64_t call_glBindAttribLocation(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, const char *))symbols[2])((uint32_t)a[0], (uint32_t)a[1], (const char *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glBindBufferBase(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t))symbols[3])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]);return 0;
}
static uint64_t call_glBindBufferRange(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t, intptr_t, intptr_t))symbols[4])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], (intptr_t)(int32_t)a[3], (intptr_t)(int32_t)a[4]);return 0;
}
static uint64_t call_glBindFragDataLocation(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, const char *))symbols[5])((uint32_t)a[0], (uint32_t)a[1], (const char *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glBindProgramPipeline(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[6])((uint32_t)a[0]);return 0;
}
static uint64_t call_glBindSampler(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[7])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glBindVertexArray(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[8])((uint32_t)a[0]);return 0;
}
static uint64_t call_glBlendEquationSeparatei(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t))symbols[9])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]);return 0;
}
static uint64_t call_glBlendFuncSeparateiARB(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))symbols[10])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], (uint32_t)a[3], (uint32_t)a[4]);return 0;
}
static uint64_t call_glClampColorARB(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[11])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glClearBufferfi(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, float, int32_t))symbols[12])((uint32_t)a[0], (int32_t)a[1], f32(a[2]), (int32_t)a[3]);return 0;
}
static uint64_t call_glClearBufferfv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, const float *))symbols[13])((uint32_t)a[0], (int32_t)a[1], (const float *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glClearBufferiv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, const int32_t *))symbols[14])((uint32_t)a[0], (int32_t)a[1], (const int32_t *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glClearBufferuiv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, const uint32_t *))symbols[15])((uint32_t)a[0], (int32_t)a[1], (const uint32_t *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glClipPlane(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, const double *))symbols[16])((uint32_t)a[0], (const double *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glColor4fv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(const float *))symbols[17])((const float *)(uintptr_t)a[0]);return 0;
}
static uint64_t call_glColorMaski(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint8_t, uint8_t, uint8_t, uint8_t))symbols[18])((uint32_t)a[0], (uint8_t)a[1], (uint8_t)a[2], (uint8_t)a[3], (uint8_t)a[4]);return 0;
}
static uint64_t call_glColorMaterial(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[19])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glColorPointer(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, uint32_t, int32_t, const void *))symbols[20])((int32_t)a[0], (uint32_t)a[1], (int32_t)a[2], (const void *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glCompileShader(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[21])((uint32_t)a[0]);return 0;
}
static uint64_t call_glCompressedTexImage1D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, int32_t, int32_t, int32_t, const void *))symbols[22])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (int32_t)a[3], (int32_t)a[4], (int32_t)a[5], (const void *)(uintptr_t)a[6]);return 0;
}
static uint64_t call_glCompressedTexImage3D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, const void *))symbols[23])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (int32_t)a[3], (int32_t)a[4], (int32_t)a[5], (int32_t)a[6], (int32_t)a[7], (const void *)(uintptr_t)a[8]);return 0;
}
static uint64_t call_glCompressedTexSubImage1D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, int32_t, uint32_t, int32_t, const void *))symbols[24])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (uint32_t)a[4], (int32_t)a[5], (const void *)(uintptr_t)a[6]);return 0;
}
static uint64_t call_glCompressedTexSubImage3D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, int32_t, const void *))symbols[25])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (int32_t)a[4], (int32_t)a[5], (int32_t)a[6], (int32_t)a[7], (uint32_t)a[8], (int32_t)a[9], (const void *)(uintptr_t)a[10]);return 0;
}
static uint64_t call_glCopyBufferSubData(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, intptr_t, intptr_t, intptr_t))symbols[26])((uint32_t)a[0], (uint32_t)a[1], (intptr_t)(int32_t)a[2], (intptr_t)(int32_t)a[3], (intptr_t)(int32_t)a[4]);return 0;
}
static uint64_t call_glCreateProgram(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    return (uint64_t)((uint32_t (*)(void))symbols[27])();
}
static uint64_t call_glCreateShader(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    return (uint64_t)((uint32_t (*)(uint32_t))symbols[28])((uint32_t)a[0]);
}
static uint64_t call_glDeleteFencesAPPLE(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, const uint32_t *))symbols[29])((int32_t)a[0], (const uint32_t *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glDeleteFramebuffersEXT(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, const uint32_t *))symbols[30])((int32_t)a[0], (const uint32_t *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glDeleteProgram(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[31])((uint32_t)a[0]);return 0;
}
static uint64_t call_glDeleteProgramPipelines(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, const uint32_t *))symbols[32])((int32_t)a[0], (const uint32_t *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glDeleteSamplers(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, const uint32_t *))symbols[33])((int32_t)a[0], (const uint32_t *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glDeleteShader(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[34])((uint32_t)a[0]);return 0;
}
static uint64_t call_glDeleteVertexArrays(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, const uint32_t *))symbols[35])((int32_t)a[0], (const uint32_t *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glDepthBoundsEXT(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(double, double))symbols[36])(f64(a+0), f64(a+2));return 0;
}
static uint64_t call_glDepthRange(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(double, double))symbols[37])(f64(a+0), f64(a+2));return 0;
}
static uint64_t call_glDepthRangeArrayv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, const double *))symbols[38])((uint32_t)a[0], (int32_t)a[1], (const double *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glDisableClientState(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[39])((uint32_t)a[0]);return 0;
}
static uint64_t call_glDisablei(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[40])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glDrawArraysIndirect(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, const void *))symbols[41])((uint32_t)a[0], (const void *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glDrawArraysInstanced(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, int32_t))symbols[42])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3]);return 0;
}
static uint64_t call_glDrawElements(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, const void *))symbols[43])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (const void *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glDrawElementsBaseVertex(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, const void *, int32_t))symbols[44])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (const void *)(uintptr_t)a[3], (int32_t)a[4]);return 0;
}
static uint64_t call_glDrawElementsIndirect(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, const void *))symbols[45])((uint32_t)a[0], (uint32_t)a[1], (const void *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glDrawElementsInstanced(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, const void *, int32_t))symbols[46])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (const void *)(uintptr_t)a[3], (int32_t)a[4]);return 0;
}
static uint64_t call_glDrawElementsInstancedBaseVertex(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, const void *, int32_t, int32_t))symbols[47])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (const void *)(uintptr_t)a[3], (int32_t)a[4], (int32_t)a[5]);return 0;
}
static uint64_t call_glEnableClientState(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[48])((uint32_t)a[0]);return 0;
}
static uint64_t call_glEnablei(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[49])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glEndTransformFeedback(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(void))symbols[50])();return 0;
}
static uint64_t call_glFlushMappedBufferRange(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, intptr_t, intptr_t))symbols[51])((uint32_t)a[0], (intptr_t)(int32_t)a[1], (intptr_t)(int32_t)a[2]);return 0;
}
static uint64_t call_glFlushMappedBufferRangeAPPLE(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, intptr_t, intptr_t))symbols[52])((uint32_t)a[0], (intptr_t)(int32_t)a[1], (intptr_t)(int32_t)a[2]);return 0;
}
static uint64_t call_glFogCoordPointerEXT(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, const void *))symbols[53])((uint32_t)a[0], (int32_t)a[1], (const void *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glFogf(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, float))symbols[54])((uint32_t)a[0], f32(a[1]));return 0;
}
static uint64_t call_glFogfv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, const float *))symbols[55])((uint32_t)a[0], (const float *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glFogi(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t))symbols[56])((uint32_t)a[0], (int32_t)a[1]);return 0;
}
static uint64_t call_glFramebufferTexture(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t, int32_t))symbols[57])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], (int32_t)a[3]);return 0;
}
static uint64_t call_glFramebufferTexture3D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t, int32_t))symbols[58])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], (uint32_t)a[3], (int32_t)a[4], (int32_t)a[5]);return 0;
}
static uint64_t call_glFramebufferTextureLayer(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t, int32_t, int32_t))symbols[59])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], (int32_t)a[3], (int32_t)a[4]);return 0;
}
static uint64_t call_glGenFencesAPPLE(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, uint32_t *))symbols[60])((int32_t)a[0], (uint32_t *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glGenProgramPipelines(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, uint32_t *))symbols[61])((int32_t)a[0], (uint32_t *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glGenSamplers(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, uint32_t *))symbols[62])((int32_t)a[0], (uint32_t *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glGenVertexArrays(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, uint32_t *))symbols[63])((int32_t)a[0], (uint32_t *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glGetActiveUniformBlockiv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t, int32_t *))symbols[64])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], (int32_t *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glGetInteger64v(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int64_t *))symbols[65])((uint32_t)a[0], (int64_t *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glGetObjectParameterivAPPLE(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t, int32_t *))symbols[66])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], (int32_t *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glGetProgramBinary(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t *, uint32_t *, void *))symbols[67])((uint32_t)a[0], (int32_t)a[1], (int32_t *)(uintptr_t)a[2], (uint32_t *)(uintptr_t)a[3], (void *)(uintptr_t)a[4]);return 0;
}
static uint64_t call_glGetProgramInfoLog(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t *, char *))symbols[68])((uint32_t)a[0], (int32_t)a[1], (int32_t *)(uintptr_t)a[2], (char *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glGetProgramiv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, int32_t *))symbols[69])((uint32_t)a[0], (uint32_t)a[1], (int32_t *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glGetQueryObjectui64v(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint64_t *))symbols[70])((uint32_t)a[0], (uint32_t)a[1], (uint64_t *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glGetRenderbufferParameteriv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, int32_t *))symbols[71])((uint32_t)a[0], (uint32_t)a[1], (int32_t *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glGetShaderInfoLog(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t *, char *))symbols[72])((uint32_t)a[0], (int32_t)a[1], (int32_t *)(uintptr_t)a[2], (char *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glGetShaderSource(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t *, char *))symbols[73])((uint32_t)a[0], (int32_t)a[1], (int32_t *)(uintptr_t)a[2], (char *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glGetShaderiv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, int32_t *))symbols[74])((uint32_t)a[0], (uint32_t)a[1], (int32_t *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glGetTexEnvfv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, float *))symbols[75])((uint32_t)a[0], (uint32_t)a[1], (float *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glGetTransformFeedbackVarying(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, int32_t, int32_t *, int32_t *, uint32_t *, char *))symbols[76])((uint32_t)a[0], (uint32_t)a[1], (int32_t)a[2], (int32_t *)(uintptr_t)a[3], (int32_t *)(uintptr_t)a[4], (uint32_t *)(uintptr_t)a[5], (char *)(uintptr_t)a[6]);return 0;
}
static uint64_t call_glGetUniformBlockIndex(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    return (uint64_t)((uint32_t (*)(uint32_t, const char *))symbols[77])((uint32_t)a[0], (const char *)(uintptr_t)a[1]);
}
static uint64_t call_glGetUniformLocation(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    return (uint64_t)((int32_t (*)(uint32_t, const char *))symbols[78])((uint32_t)a[0], (const char *)(uintptr_t)a[1]);
}
static uint64_t call_glHint(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[79])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glIsTexture(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    return (uint64_t)((uint8_t (*)(uint32_t))symbols[80])((uint32_t)a[0]);
}
static uint64_t call_glLightModelfv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, const float *))symbols[81])((uint32_t)a[0], (const float *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glLightModeli(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t))symbols[82])((uint32_t)a[0], (int32_t)a[1]);return 0;
}
static uint64_t call_glLightf(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, float))symbols[83])((uint32_t)a[0], (uint32_t)a[1], f32(a[2]));return 0;
}
static uint64_t call_glLightfv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, const float *))symbols[84])((uint32_t)a[0], (uint32_t)a[1], (const float *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glLinkProgram(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[85])((uint32_t)a[0]);return 0;
}
static uint64_t call_glLoadMatrixf(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(const float *))symbols[86])((const float *)(uintptr_t)a[0]);return 0;
}
static uint64_t call_glMaterialfv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, const float *))symbols[87])((uint32_t)a[0], (uint32_t)a[1], (const float *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glMateriali(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, int32_t))symbols[88])((uint32_t)a[0], (uint32_t)a[1], (int32_t)a[2]);return 0;
}
static uint64_t call_glMultMatrixf(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(const float *))symbols[89])((const float *)(uintptr_t)a[0]);return 0;
}
static uint64_t call_glNormalPointer(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, const void *))symbols[90])((uint32_t)a[0], (int32_t)a[1], (const void *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glObjectPurgeableAPPLE(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    return (uint64_t)((uint32_t (*)(uint32_t, uint32_t, uint32_t))symbols[91])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]);
}
static uint64_t call_glObjectUnpurgeableAPPLE(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    return (uint64_t)((uint32_t (*)(uint32_t, uint32_t, uint32_t))symbols[92])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]);
}
static uint64_t call_glPatchParameteri(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t))symbols[93])((uint32_t)a[0], (int32_t)a[1]);return 0;
}
static uint64_t call_glPolygonMode(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[94])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glPopGroupMarkerEXT(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(void))symbols[95])();return 0;
}
static uint64_t call_glPopMatrix(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(void))symbols[96])();return 0;
}
static uint64_t call_glPrioritizeTextures(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, const uint32_t *, const float *))symbols[97])((int32_t)a[0], (const uint32_t *)(uintptr_t)a[1], (const float *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glProgramBinary(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, const void *, int32_t))symbols[98])((uint32_t)a[0], (uint32_t)a[1], (const void *)(uintptr_t)a[2], (int32_t)a[3]);return 0;
}
static uint64_t call_glProgramParameteri(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, int32_t))symbols[99])((uint32_t)a[0], (uint32_t)a[1], (int32_t)a[2]);return 0;
}
static uint64_t call_glProgramUniform1i(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t))symbols[100])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2]);return 0;
}
static uint64_t call_glProgramUniform1iv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, const int32_t *))symbols[101])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (const int32_t *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glProgramUniform1ui(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t))symbols[102])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2]);return 0;
}
static uint64_t call_glProgramUniform4fv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, const float *))symbols[103])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (const float *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glProgramUniform4iv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, const int32_t *))symbols[104])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (const int32_t *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glProvokingVertexEXT(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[105])((uint32_t)a[0]);return 0;
}
static uint64_t call_glPushGroupMarkerEXT(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, const char *))symbols[106])((int32_t)a[0], (const char *)(uintptr_t)a[1]);return 0;
}
static uint64_t call_glPushMatrix(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(void))symbols[107])();return 0;
}
static uint64_t call_glQueryCounter(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[108])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glRenderbufferStorageMultisample(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, int32_t, int32_t))symbols[109])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (int32_t)a[3], (int32_t)a[4]);return 0;
}
static uint64_t call_glSampleMaski(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[110])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glSamplerParameterf(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, float))symbols[111])((uint32_t)a[0], (uint32_t)a[1], f32(a[2]));return 0;
}
static uint64_t call_glSamplerParameterfv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, const float *))symbols[112])((uint32_t)a[0], (uint32_t)a[1], (const float *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glSamplerParameteri(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, int32_t))symbols[113])((uint32_t)a[0], (uint32_t)a[1], (int32_t)a[2]);return 0;
}
static uint64_t call_glScalef(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(float, float, float))symbols[114])(f32(a[0]), f32(a[1]), f32(a[2]));return 0;
}
static uint64_t call_glSecondaryColor3fvEXT(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(const float *))symbols[115])((const float *)(uintptr_t)a[0]);return 0;
}
static uint64_t call_glSecondaryColorPointerEXT(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, uint32_t, int32_t, const void *))symbols[116])((int32_t)a[0], (uint32_t)a[1], (int32_t)a[2], (const void *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glSetFenceAPPLE(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[117])((uint32_t)a[0]);return 0;
}
static uint64_t call_glShadeModel(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[118])((uint32_t)a[0]);return 0;
}
static uint64_t call_glTestFenceAPPLE(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    return (uint64_t)((uint8_t (*)(uint32_t))symbols[119])((uint32_t)a[0]);
}
static uint64_t call_glTexBuffer(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t))symbols[120])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]);return 0;
}
static uint64_t call_glTexCoordPointer(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, uint32_t, int32_t, const void *))symbols[121])((int32_t)a[0], (uint32_t)a[1], (int32_t)a[2], (const void *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glTexEnvfv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, const float *))symbols[122])((uint32_t)a[0], (uint32_t)a[1], (const float *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glTexGenfv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, const float *))symbols[123])((uint32_t)a[0], (uint32_t)a[1], (const float *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glTexGeni(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, int32_t))symbols[124])((uint32_t)a[0], (uint32_t)a[1], (int32_t)a[2]);return 0;
}
static uint64_t call_glTexImage1D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *))symbols[125])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (int32_t)a[4], (uint32_t)a[5], (uint32_t)a[6], (const void *)(uintptr_t)a[7]);return 0;
}
static uint64_t call_glTexImage2DMultisample(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, uint8_t))symbols[126])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (int32_t)a[4], (uint8_t)a[5]);return 0;
}
static uint64_t call_glTexImage3DMultisample(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint8_t))symbols[127])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (int32_t)a[4], (int32_t)a[5], (uint8_t)a[6]);return 0;
}
static uint64_t call_glTexStorage1D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, int32_t))symbols[128])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (int32_t)a[3]);return 0;
}
static uint64_t call_glTexStorage2D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, int32_t, int32_t))symbols[129])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (int32_t)a[3], (int32_t)a[4]);return 0;
}
static uint64_t call_glTexStorage3D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, int32_t, int32_t, int32_t))symbols[130])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (int32_t)a[3], (int32_t)a[4], (int32_t)a[5]);return 0;
}
static uint64_t call_glTexSubImage1D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *))symbols[131])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (uint32_t)a[4], (uint32_t)a[5], (const void *)(uintptr_t)a[6]);return 0;
}
static uint64_t call_glTexSubImage2D(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *))symbols[132])((uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (int32_t)a[4], (int32_t)a[5], (uint32_t)a[6], (uint32_t)a[7], (const void *)(uintptr_t)a[8]);return 0;
}
static uint64_t call_glTextureBarrierNV(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(void))symbols[133])();return 0;
}
static uint64_t call_glTranslatef(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(float, float, float))symbols[134])(f32(a[0]), f32(a[1]), f32(a[2]));return 0;
}
static uint64_t call_glUniform1i(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, int32_t))symbols[135])((int32_t)a[0], (int32_t)a[1]);return 0;
}
static uint64_t call_glUniform1iv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, int32_t, const int32_t *))symbols[136])((int32_t)a[0], (int32_t)a[1], (const int32_t *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glUniform1ui(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, uint32_t))symbols[137])((int32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glUniform4fv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, int32_t, const float *))symbols[138])((int32_t)a[0], (int32_t)a[1], (const float *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glUniform4iv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, int32_t, const int32_t *))symbols[139])((int32_t)a[0], (int32_t)a[1], (const int32_t *)(uintptr_t)a[2]);return 0;
}
static uint64_t call_glUniformBlockBinding(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t))symbols[140])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]);return 0;
}
static uint64_t call_glUnmapBuffer(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    return (uint64_t)((uint8_t (*)(uint32_t))symbols[141])((uint32_t)a[0]);
}
static uint64_t call_glUseProgram(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t))symbols[142])((uint32_t)a[0]);return 0;
}
static uint64_t call_glUseProgramStages(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t, uint32_t))symbols[143])((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]);return 0;
}
static uint64_t call_glVertexAttribDivisorARB(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, uint32_t))symbols[144])((uint32_t)a[0], (uint32_t)a[1]);return 0;
}
static uint64_t call_glVertexAttribIPointer(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, uint32_t, int32_t, const void *))symbols[145])((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (int32_t)a[3], (const void *)(uintptr_t)a[4]);return 0;
}
static uint64_t call_glVertexBlendARB(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t))symbols[146])((int32_t)a[0]);return 0;
}
static uint64_t call_glVertexPointer(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(int32_t, uint32_t, int32_t, const void *))symbols[147])((int32_t)a[0], (uint32_t)a[1], (int32_t)a[2], (const void *)(uintptr_t)a[3]);return 0;
}
static uint64_t call_glViewportArrayv(const uint32_t *a,uint32_t site) {
    (void)site; (void)a;
    ((void (*)(uint32_t, int32_t, const float *))symbols[148])((uint32_t)a[0], (int32_t)a[1], (const float *)(uintptr_t)a[2]);return 0;
}
static const struct {const char *name; lp32_fast_import_fn call;} table[] = {
    {"glAttachShader",call_glAttachShader},
    {"glBeginTransformFeedback",call_glBeginTransformFeedback},
    {"glBindAttribLocation",call_glBindAttribLocation},
    {"glBindBufferBase",call_glBindBufferBase},
    {"glBindBufferRange",call_glBindBufferRange},
    {"glBindFragDataLocation",call_glBindFragDataLocation},
    {"glBindProgramPipeline",call_glBindProgramPipeline},
    {"glBindSampler",call_glBindSampler},
    {"glBindVertexArray",call_glBindVertexArray},
    {"glBlendEquationSeparatei",call_glBlendEquationSeparatei},
    {"glBlendFuncSeparateiARB",call_glBlendFuncSeparateiARB},
    {"glClampColorARB",call_glClampColorARB},
    {"glClearBufferfi",call_glClearBufferfi},
    {"glClearBufferfv",call_glClearBufferfv},
    {"glClearBufferiv",call_glClearBufferiv},
    {"glClearBufferuiv",call_glClearBufferuiv},
    {"glClipPlane",call_glClipPlane},
    {"glColor4fv",call_glColor4fv},
    {"glColorMaski",call_glColorMaski},
    {"glColorMaterial",call_glColorMaterial},
    {"glColorPointer",call_glColorPointer},
    {"glCompileShader",call_glCompileShader},
    {"glCompressedTexImage1D",call_glCompressedTexImage1D},
    {"glCompressedTexImage3D",call_glCompressedTexImage3D},
    {"glCompressedTexSubImage1D",call_glCompressedTexSubImage1D},
    {"glCompressedTexSubImage3D",call_glCompressedTexSubImage3D},
    {"glCopyBufferSubData",call_glCopyBufferSubData},
    {"glCreateProgram",call_glCreateProgram},
    {"glCreateShader",call_glCreateShader},
    {"glDeleteFencesAPPLE",call_glDeleteFencesAPPLE},
    {"glDeleteFramebuffersEXT",call_glDeleteFramebuffersEXT},
    {"glDeleteProgram",call_glDeleteProgram},
    {"glDeleteProgramPipelines",call_glDeleteProgramPipelines},
    {"glDeleteSamplers",call_glDeleteSamplers},
    {"glDeleteShader",call_glDeleteShader},
    {"glDeleteVertexArrays",call_glDeleteVertexArrays},
    {"glDepthBoundsEXT",call_glDepthBoundsEXT},
    {"glDepthRange",call_glDepthRange},
    {"glDepthRangeArrayv",call_glDepthRangeArrayv},
    {"glDisableClientState",call_glDisableClientState},
    {"glDisablei",call_glDisablei},
    {"glDrawArraysIndirect",call_glDrawArraysIndirect},
    {"glDrawArraysInstanced",call_glDrawArraysInstanced},
    {"glDrawElements",call_glDrawElements},
    {"glDrawElementsBaseVertex",call_glDrawElementsBaseVertex},
    {"glDrawElementsIndirect",call_glDrawElementsIndirect},
    {"glDrawElementsInstanced",call_glDrawElementsInstanced},
    {"glDrawElementsInstancedBaseVertex",call_glDrawElementsInstancedBaseVertex},
    {"glEnableClientState",call_glEnableClientState},
    {"glEnablei",call_glEnablei},
    {"glEndTransformFeedback",call_glEndTransformFeedback},
    {"glFlushMappedBufferRange",call_glFlushMappedBufferRange},
    {"glFlushMappedBufferRangeAPPLE",call_glFlushMappedBufferRangeAPPLE},
    {"glFogCoordPointerEXT",call_glFogCoordPointerEXT},
    {"glFogf",call_glFogf},
    {"glFogfv",call_glFogfv},
    {"glFogi",call_glFogi},
    {"glFramebufferTexture",call_glFramebufferTexture},
    {"glFramebufferTexture3D",call_glFramebufferTexture3D},
    {"glFramebufferTextureLayer",call_glFramebufferTextureLayer},
    {"glGenFencesAPPLE",call_glGenFencesAPPLE},
    {"glGenProgramPipelines",call_glGenProgramPipelines},
    {"glGenSamplers",call_glGenSamplers},
    {"glGenVertexArrays",call_glGenVertexArrays},
    {"glGetActiveUniformBlockiv",call_glGetActiveUniformBlockiv},
    {"glGetInteger64v",call_glGetInteger64v},
    {"glGetObjectParameterivAPPLE",call_glGetObjectParameterivAPPLE},
    {"glGetProgramBinary",call_glGetProgramBinary},
    {"glGetProgramInfoLog",call_glGetProgramInfoLog},
    {"glGetProgramiv",call_glGetProgramiv},
    {"glGetQueryObjectui64v",call_glGetQueryObjectui64v},
    {"glGetRenderbufferParameteriv",call_glGetRenderbufferParameteriv},
    {"glGetShaderInfoLog",call_glGetShaderInfoLog},
    {"glGetShaderSource",call_glGetShaderSource},
    {"glGetShaderiv",call_glGetShaderiv},
    {"glGetTexEnvfv",call_glGetTexEnvfv},
    {"glGetTransformFeedbackVarying",call_glGetTransformFeedbackVarying},
    {"glGetUniformBlockIndex",call_glGetUniformBlockIndex},
    {"glGetUniformLocation",call_glGetUniformLocation},
    {"glHint",call_glHint},
    {"glIsTexture",call_glIsTexture},
    {"glLightModelfv",call_glLightModelfv},
    {"glLightModeli",call_glLightModeli},
    {"glLightf",call_glLightf},
    {"glLightfv",call_glLightfv},
    {"glLinkProgram",call_glLinkProgram},
    {"glLoadMatrixf",call_glLoadMatrixf},
    {"glMaterialfv",call_glMaterialfv},
    {"glMateriali",call_glMateriali},
    {"glMultMatrixf",call_glMultMatrixf},
    {"glNormalPointer",call_glNormalPointer},
    {"glObjectPurgeableAPPLE",call_glObjectPurgeableAPPLE},
    {"glObjectUnpurgeableAPPLE",call_glObjectUnpurgeableAPPLE},
    {"glPatchParameteri",call_glPatchParameteri},
    {"glPolygonMode",call_glPolygonMode},
    {"glPopGroupMarkerEXT",call_glPopGroupMarkerEXT},
    {"glPopMatrix",call_glPopMatrix},
    {"glPrioritizeTextures",call_glPrioritizeTextures},
    {"glProgramBinary",call_glProgramBinary},
    {"glProgramParameteri",call_glProgramParameteri},
    {"glProgramUniform1i",call_glProgramUniform1i},
    {"glProgramUniform1iv",call_glProgramUniform1iv},
    {"glProgramUniform1ui",call_glProgramUniform1ui},
    {"glProgramUniform4fv",call_glProgramUniform4fv},
    {"glProgramUniform4iv",call_glProgramUniform4iv},
    {"glProvokingVertexEXT",call_glProvokingVertexEXT},
    {"glPushGroupMarkerEXT",call_glPushGroupMarkerEXT},
    {"glPushMatrix",call_glPushMatrix},
    {"glQueryCounter",call_glQueryCounter},
    {"glRenderbufferStorageMultisample",call_glRenderbufferStorageMultisample},
    {"glSampleMaski",call_glSampleMaski},
    {"glSamplerParameterf",call_glSamplerParameterf},
    {"glSamplerParameterfv",call_glSamplerParameterfv},
    {"glSamplerParameteri",call_glSamplerParameteri},
    {"glScalef",call_glScalef},
    {"glSecondaryColor3fvEXT",call_glSecondaryColor3fvEXT},
    {"glSecondaryColorPointerEXT",call_glSecondaryColorPointerEXT},
    {"glSetFenceAPPLE",call_glSetFenceAPPLE},
    {"glShadeModel",call_glShadeModel},
    {"glTestFenceAPPLE",call_glTestFenceAPPLE},
    {"glTexBuffer",call_glTexBuffer},
    {"glTexCoordPointer",call_glTexCoordPointer},
    {"glTexEnvfv",call_glTexEnvfv},
    {"glTexGenfv",call_glTexGenfv},
    {"glTexGeni",call_glTexGeni},
    {"glTexImage1D",call_glTexImage1D},
    {"glTexImage2DMultisample",call_glTexImage2DMultisample},
    {"glTexImage3DMultisample",call_glTexImage3DMultisample},
    {"glTexStorage1D",call_glTexStorage1D},
    {"glTexStorage2D",call_glTexStorage2D},
    {"glTexStorage3D",call_glTexStorage3D},
    {"glTexSubImage1D",call_glTexSubImage1D},
    {"glTexSubImage2D",call_glTexSubImage2D},
    {"glTextureBarrierNV",call_glTextureBarrierNV},
    {"glTranslatef",call_glTranslatef},
    {"glUniform1i",call_glUniform1i},
    {"glUniform1iv",call_glUniform1iv},
    {"glUniform1ui",call_glUniform1ui},
    {"glUniform4fv",call_glUniform4fv},
    {"glUniform4iv",call_glUniform4iv},
    {"glUniformBlockBinding",call_glUniformBlockBinding},
    {"glUnmapBuffer",call_glUnmapBuffer},
    {"glUseProgram",call_glUseProgram},
    {"glUseProgramStages",call_glUseProgramStages},
    {"glVertexAttribDivisorARB",call_glVertexAttribDivisorARB},
    {"glVertexAttribIPointer",call_glVertexAttribIPointer},
    {"glVertexBlendARB",call_glVertexBlendARB},
    {"glVertexPointer",call_glVertexPointer},
    {"glViewportArrayv",call_glViewportArrayv},
};
static uint64_t extra_glPixelStorei(const uint32_t *a,uint32_t site){
    (void)site;glPixelStorei(a[0],(int32_t)a[1]);return 0;
}
static uint64_t extra_glGetBooleanv(const uint32_t *a,uint32_t site){
    (void)site;glGetBooleanv(a[0],(void *)(uintptr_t)a[1]);return 0;
}
static uint64_t extra_glGetBufferSubData(const uint32_t *a,uint32_t site){
    (void)site;glGetBufferSubData(a[0],(int32_t)a[1],(int32_t)a[2],(void *)(uintptr_t)a[3]);return 0;
}
static uint64_t extra_glGetCompressedTexImage(const uint32_t *a,uint32_t site){
    (void)site;glGetCompressedTexImage(a[0],(int32_t)a[1],(void *)(uintptr_t)a[2]);return 0;
}
static uint64_t extra_glGetFramebufferAttachmentParameteriv(const uint32_t *a,uint32_t site){
    (void)site;glGetFramebufferAttachmentParameteriv(a[0],a[1],a[2],(void *)(uintptr_t)a[3]);return 0;
}
static uint64_t extra_glGetTexImage(const uint32_t *a,uint32_t site){
    (void)site;glGetTexImage(a[0],(int32_t)a[1],a[2],a[3],(void *)(uintptr_t)a[4]);return 0;
}
static uint64_t extra_glGetTexLevelParameteriv(const uint32_t *a,uint32_t site){
    (void)site;glGetTexLevelParameteriv(a[0],(int32_t)a[1],a[2],(void *)(uintptr_t)a[3]);return 0;
}
static uint64_t extra_glIsEnabled(const uint32_t *a,uint32_t site){
    (void)site;return glIsEnabled(a[0]);
}
static uint64_t extra_glReadPixels(const uint32_t *a,uint32_t site){
    (void)site;glReadPixels((int32_t)a[0],(int32_t)a[1],(int32_t)a[2],(int32_t)a[3],a[4],a[5],(void *)(uintptr_t)a[6]);return 0;
}
static uint64_t extra_glTransformFeedbackVaryings(const uint32_t *a,uint32_t site){
    (void)site;int32_t count=(int32_t)a[1];
    const uint32_t *guest=(void *)(uintptr_t)a[2];
    const char **names=count>0?calloc((size_t)count,sizeof(*names)):NULL;
    if(count>0&&!names)return 0;
    for(int32_t i=0;i<count;++i)names[i]=(void *)(uintptr_t)guest[i];
    glTransformFeedbackVaryings(a[0],count,names,a[3]);free(names);return 0;
}
static pthread_once_t once=PTHREAD_ONCE_INIT;
static void initialize(void){for(unsigned i=0;i<sizeof(table)/sizeof(table[0]);++i)symbols[i]=dlsym(RTLD_DEFAULT,table[i].name);}
lp32_fast_import_fn gl_core_bridge32_fast_import(const char *name){
    if(*name=='_')++name; if(strncmp(name,"gl",2))return NULL;
    if(!strcmp(name,"glPixelStorei"))return extra_glPixelStorei;
    if(!strcmp(name,"glGetBooleanv"))return extra_glGetBooleanv;
    if(!strcmp(name,"glGetBufferSubData"))return extra_glGetBufferSubData;
    if(!strcmp(name,"glGetCompressedTexImage"))return extra_glGetCompressedTexImage;
    if(!strcmp(name,"glGetFramebufferAttachmentParameteriv"))return extra_glGetFramebufferAttachmentParameteriv;
    if(!strcmp(name,"glGetTexImage"))return extra_glGetTexImage;
    if(!strcmp(name,"glGetTexLevelParameteriv"))return extra_glGetTexLevelParameteriv;
    if(!strcmp(name,"glIsEnabled"))return extra_glIsEnabled;
    if(!strcmp(name,"glReadPixels"))return extra_glReadPixels;
    if(!strcmp(name,"glTransformFeedbackVaryings"))return extra_glTransformFeedbackVaryings;
    pthread_once(&once,initialize);
    for(unsigned i=0;i<sizeof(table)/sizeof(table[0]);++i)if(!strcmp(name,table[i].name))return symbols[i]?table[i].call:NULL;
    return NULL;
}
static uint64_t shader_source(const uint32_t *a,uint32_t site) {
    (void)site;
    extern void glShaderSource(uint32_t,int32_t,const char *const *,const int32_t *);
    int32_t count=(int32_t)a[1];
    if(count<=0){glShaderSource(a[0],count,NULL,(void *)(uintptr_t)a[3]);return 0;}
    const char **strings=calloc((size_t)count,sizeof(*strings));if(!strings)return 0;
    const uint32_t *guest=(void *)(uintptr_t)a[2];
    for(int32_t i=0;i<count;++i)strings[i]=(void *)(uintptr_t)guest[i];
    glShaderSource(a[0],count,strings,(void *)(uintptr_t)a[3]);free(strings);return 0;
}
int gl_core_bridge32_dispatch(const char *name,const uint32_t *a,uint64_t *out){
    if(!strcmp(name,"_glShaderSource")||!strcmp(name,"glShaderSource")){*out=shader_source(a,0);return 1;}
    lp32_fast_import_fn fn=gl_core_bridge32_fast_import(name);if(!fn)return 0;*out=fn(a,0);return 1;
}
