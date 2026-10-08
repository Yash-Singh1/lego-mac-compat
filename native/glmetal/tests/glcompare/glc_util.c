#include "glc_gl_legacy.h"
#include "glc_gl_count.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glcompare.h"

/* Provided by the runner: looks a symbol up in the implementation it loaded. */
void *glc_lookup(const char *name);

void *glc_resolve(int id, const char *name)
{
    static void *cache[GLC_GL_FUNCTION_COUNT];
    void *function = cache[id];
    if (!function) {
        function = glc_lookup(name);
        if (!function) glc_fail("implementation lacks %s", name);
        cache[id] = function;
    }
    return function;
}

static unsigned compile(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        glGetShaderInfoLog(shader, sizeof log, NULL, log);
        glc_fail("%s shader: %s", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
    }
    return shader;
}

unsigned glc_program(const char *vertex, const char *fragment, const char *const *attributes)
{
    GLuint program = glCreateProgram();
    glAttachShader(program, compile(GL_VERTEX_SHADER, vertex));
    glAttachShader(program, compile(GL_FRAGMENT_SHADER, fragment));
    for (unsigned i = 0; attributes && attributes[i]; ++i) glBindAttribLocation(program, i, attributes[i]);
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        glGetProgramInfoLog(program, sizeof log, NULL, log);
        glc_fail("link: %s", log);
    }
    glUseProgram(program);
    return program;
}

unsigned glc_arb_program(unsigned target, const char *source)
{
    GLuint program = 0;
    glGenProgramsARB(1, &program);
    glBindProgramARB(target, program);
    glProgramStringARB(target, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)strlen(source), source);
    GLint position = -1;
    glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &position);
    if (position != -1)
        glc_fail("ARB program error at %d: %s", position,
                 (const char *)glGetString(GL_PROGRAM_ERROR_STRING_ARB));
    glEnable(target);
    return program;
}

void glc_pattern_rgba8(unsigned char *out, int width, int height, unsigned seed)
{
    unsigned state = seed * 2654435761u + 12345u;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            unsigned char *p = out + ((size_t)y * width + x) * 4;
            state = state * 1103515245u + 12345u;
            p[0] = (unsigned char)(x * 255 / (width > 1 ? width - 1 : 1));
            p[1] = (unsigned char)(y * 255 / (height > 1 ? height - 1 : 1));
            p[2] = (unsigned char)(state >> 24);
            p[3] = (unsigned char)(((x + y) & 1) ? 255 : 160);
        }
}
