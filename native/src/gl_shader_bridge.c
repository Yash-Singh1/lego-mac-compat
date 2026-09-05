#include "gl_shader_bridge.h"
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl.h>
#include <stdlib.h>
#include <string.h>

/* Apple's ARB shader handles are host pointers. Use the equivalent core API
   throughout so both the ARB and core entry points share 32-bit GLuint names,
   as Source expects when it mixes glCreateShaderObjectARB and glDeleteShader. */
int gl_shader_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *r)
{
    if (*name == '_') ++name;
#define IS(s) (!strcmp(name, s))
#define PTR(i) ((void *)(uintptr_t)a[i])
    *r = 0;
    if (IS("glCreateShaderObjectARB") || IS("glCreateShader")) *r = glCreateShader(a[0]);
    else if (IS("glCreateProgramObjectARB") || IS("glCreateProgram")) *r = glCreateProgram();
    else if (IS("glCompileShaderARB") || IS("glCompileShader")) glCompileShader(a[0]);
    else if (IS("glAttachObjectARB") || IS("glAttachShader")) glAttachShader(a[0], a[1]);
    else if (IS("glDetachObjectARB") || IS("glDetachShader")) glDetachShader(a[0], a[1]);
    else if (IS("glLinkProgramARB") || IS("glLinkProgram")) glLinkProgram(a[0]);
    else if (IS("glUseProgramObjectARB") || IS("glUseProgram")) glUseProgram(a[0]);
    else if (IS("glValidateProgramARB") || IS("glValidateProgram")) glValidateProgram(a[0]);
    else if (IS("glDeleteShader")) glDeleteShader(a[0]);
    else if (IS("glDeleteProgram")) glDeleteProgram(a[0]);
    else if (IS("glDeleteObjectARB")) {
        if (glIsShader(a[0])) glDeleteShader(a[0]); else glDeleteProgram(a[0]);
    } else if (IS("glShaderSourceARB") || IS("glShaderSource")) {
        GLsizei count = (GLsizei)a[1];
        if (count <= 0) { glShaderSource(a[0], count, NULL, PTR(3)); return 1; }
        const uint32_t *guest = PTR(2);
        const GLchar **strings = calloc((size_t)count, sizeof(*strings));
        if (!strings) return 0;
        for (GLsizei i = 0; i < count; ++i) strings[i] = (const void *)(uintptr_t)guest[i];
        glShaderSource(a[0], count, strings, PTR(3));
        free(strings);
    } else if (IS("glGetObjectParameterivARB")) {
        GLint *out = PTR(2);
        if (a[1] == GL_OBJECT_TYPE_ARB) *out = glIsShader(a[0]) ? GL_SHADER_OBJECT_ARB : GL_PROGRAM_OBJECT_ARB;
        else if (glIsShader(a[0])) glGetShaderiv(a[0], a[1], out);
        else glGetProgramiv(a[0], a[1], out);
    } else if (IS("glGetShaderiv")) glGetShaderiv(a[0], a[1], PTR(2));
    else if (IS("glGetProgramiv")) glGetProgramiv(a[0], a[1], PTR(2));
    else if (IS("glGetInfoLogARB")) {
        if (glIsShader(a[0])) glGetShaderInfoLog(a[0], a[1], PTR(2), PTR(3));
        else glGetProgramInfoLog(a[0], a[1], PTR(2), PTR(3));
    } else if (IS("glGetShaderInfoLog")) glGetShaderInfoLog(a[0], a[1], PTR(2), PTR(3));
    else if (IS("glGetProgramInfoLog")) glGetProgramInfoLog(a[0], a[1], PTR(2), PTR(3));
    else if (IS("glGetUniformLocationARB") || IS("glGetUniformLocation")) *r = (uint32_t)glGetUniformLocation(a[0], PTR(1));
    else if (IS("glGetAttribLocationARB") || IS("glGetAttribLocation")) *r = (uint32_t)glGetAttribLocation(a[0], PTR(1));
    else if (IS("glBindAttribLocationARB") || IS("glBindAttribLocation")) glBindAttribLocation(a[0], a[1], PTR(2));
    else if (IS("glUniform1iARB") || IS("glUniform1i")) glUniform1i(a[0], a[1]);
    else if (IS("glUniform1fARB") || IS("glUniform1f")) {
        GLfloat value; memcpy(&value, a + 1, sizeof(value)); glUniform1f(a[0], value);
    } else if (IS("glUniform4fvARB") || IS("glUniform4fv")) glUniform4fv(a[0], a[1], PTR(2));
    else return 0;
    return 1;
#undef IS
#undef PTR
}
