#include "gl_shader_bridge.h"
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static unsigned char *guest;
static uint32_t call(const char *name, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint32_t args[] = {a, b, c, d};
    uint64_t result = 0;
    assert(gl_shader_bridge32_dispatch(name, args, &result));
    return (uint32_t)result;
}
static uint32_t address(size_t offset) { return (uint32_t)(uintptr_t)(guest + offset); }
static GLuint shader(GLenum type, const char *text)
{
    strcpy((char *)guest + 128, "#version 120\n");
    strcpy((char *)guest + 256, text);
    uint32_t *strings = (void *)guest;
    strings[0] = address(128); strings[1] = address(256);
    GLint *lengths = (void *)(guest + 32);
    lengths[0] = (GLint)strlen((char *)guest + 128);
    lengths[1] = (GLint)strlen(text);
    GLuint object = call("_glCreateShaderObjectARB", type, 0, 0, 0);
    assert(object && glIsShader(object));
    call("glShaderSourceARB", object, 2, address(0), address(32));
    call("_glCompileShaderARB", object, 0, 0, 0);
    call("glGetObjectParameterivARB", object, GL_COMPILE_STATUS, address(64), 0);
    assert(*(GLint *)(guest + 64));
    return object;
}
int main(void)
{
    guest = mmap((void *)0x20000000, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(guest != MAP_FAILED && (uintptr_t)guest + 4096 <= UINT32_MAX);
    CGLPixelFormatAttribute attributes[] = {kCGLPFAAccelerated, 0};
    CGLPixelFormatObj format = NULL;
    CGLContextObj context = NULL;
    GLint count;
    assert(CGLChoosePixelFormat(attributes, &format, &count) == kCGLNoError);
    assert(CGLCreateContext(format, NULL, &context) == kCGLNoError);
    CGLDestroyPixelFormat(format);
    assert(CGLSetCurrentContext(context) == kCGLNoError);
    GLuint vertex = shader(GL_VERTEX_SHADER, "void main() { gl_Position = gl_Vertex; }");
    GLuint fragment = shader(GL_FRAGMENT_SHADER, "uniform vec4 tint; void main() { gl_FragColor = tint; }");
    GLuint program = call("glCreateProgramObjectARB", 0, 0, 0, 0);
    call("glAttachObjectARB", program, vertex, 0, 0);
    call("glAttachObjectARB", program, fragment, 0, 0);
    call("glLinkProgramARB", program, 0, 0, 0);
    call("glGetObjectParameterivARB", program, GL_LINK_STATUS, address(64), 0);
    assert(*(GLint *)(guest + 64));
    call("glUseProgram", program, 0, 0, 0);
    strcpy((char *)guest + 128, "tint");
    GLint uniform = (GLint)call("glGetUniformLocationARB", program, address(128), 0, 0);
    assert(uniform >= 0);
    GLfloat color[] = {1, 0, 0, 1};
    memcpy(guest + 256, color, sizeof(color));
    call("glUniform4fv", uniform, 1, address(256), 0);
    GLuint framebuffer, texture;
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffersEXT(1, &framebuffer); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, framebuffer);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, texture, 0);
    assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT);
    glViewport(0, 0, 4, 4);
    glBegin(GL_QUADS);
    glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(1, 1); glVertex2f(-1, 1);
    glEnd();
    unsigned char pixel[4];
    glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    assert(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255);
    assert(glGetError() == GL_NO_ERROR);
    call("glUseProgram", 0, 0, 0, 0);
    call("glDetachObjectARB", program, vertex, 0, 0);
    call("glDeleteShader", vertex, 0, 0, 0);
    call("glDeleteObjectARB", program, 0, 0, 0);
    call("glDeleteObjectARB", fragment, 0, 0, 0);
    assert(!glIsShader(vertex) && !glIsShader(fragment) && !glIsProgram(program));
    glDeleteFramebuffersEXT(1, &framebuffer); glDeleteTextures(1, &texture);
    CGLSetCurrentContext(NULL); CGLDestroyContext(context);
    munmap(guest, 4096);
    puts("GLSL bridge: PASS (ARB/core handles, multi-string source, uniform, rendered pixel)");
    return 0;
}
