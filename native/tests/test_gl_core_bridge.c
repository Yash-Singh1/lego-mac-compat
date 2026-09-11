#define GL_SILENCE_DEPRECATION 1
#include "gl_core_bridge.h"
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
static uint64_t call(const char *name,const uint32_t *args){uint64_t out=0;assert(gl_core_bridge32_dispatch(name,args,&out));return out;}
extern void glGetMaterialfv(uint32_t, uint32_t, float *);
int main(void){
    uint8_t *memory=mmap((void *)0x10000000,4096,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE|MAP_FIXED,-1,0);
    assert(memory==(void *)0x10000000);
    CGLPixelFormatAttribute attrs[]={kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,kCGLPFAAccelerated,0};
    CGLPixelFormatObj format;GLint count;CGLContextObj context;
    assert(CGLChoosePixelFormat(attrs,&format,&count)==kCGLNoError);
    assert(CGLCreateContext(format,NULL,&context)==kCGLNoError);CGLDestroyPixelFormat(format);
    assert(CGLSetCurrentContext(context)==kCGLNoError);
    const char *source[]={"#version 150\nvoid main(){vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));gl_Position=vec4(p[gl_VertexID],0,1);}",
        "#version 150\nuniform vec4 color;out vec4 pixel;void main(){pixel=color;}"};
    uint32_t shaders[2];
    for(unsigned i=0;i<2;++i){
        uint32_t a[]={i?GL_FRAGMENT_SHADER:GL_VERTEX_SHADER};shaders[i]=(uint32_t)call("_glCreateShader",a);
        strcpy((char *)memory+128,source[i]);*(uint32_t *)memory=0x10000080;
        uint32_t text[]={shaders[i],1,0x10000000,0};call("_glShaderSource",text);
        call("_glCompileShader",&shaders[i]);
        uint32_t status[]={shaders[i],GL_COMPILE_STATUS,0x10000300};
        *(uint32_t *)(memory+772)=0xabcdef01;call("_glGetShaderiv",status);
        assert(*(uint32_t *)(memory+768)==GL_TRUE&&*(uint32_t *)(memory+772)==0xabcdef01);
    }
    uint32_t program=(uint32_t)call("_glCreateProgram",NULL);
    for(unsigned i=0;i<2;++i){uint32_t a[]={program,shaders[i]};call("_glAttachShader",a);}
    call("_glLinkProgram",&program);uint32_t status[]={program,GL_LINK_STATUS,0x10000300};
    call("_glGetProgramiv",status);assert(*(uint32_t *)(memory+768)==GL_TRUE);call("_glUseProgram",&program);
    strcpy((char *)memory+128,"color");uint32_t uniform[]={program,0x10000080};
    uint32_t location=(uint32_t)call("_glGetUniformLocation",uniform);assert((int32_t)location>=0);
    float color[]={0.25,0.5,0.75,1};memcpy(memory+128,color,16);uint32_t set[]={location,1,0x10000080};call("_glUniform4fv",set);
    GLuint texture,fbo;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
    uint32_t vaoargs[]={1,0x10000300};call("_glGenVertexArrays",vaoargs);GLuint vao=*(uint32_t *)(memory+768);call("_glBindVertexArray",&vao);
    glViewport(0,0,4,4);glDrawArrays(GL_TRIANGLES,0,3);
    uint32_t pack[]={GL_PACK_ALIGNMENT,1};call("_glPixelStorei",pack);
    memset(memory+1024,0xa5,8);
    uint32_t read[]={1,1,1,1,GL_RGBA,GL_UNSIGNED_BYTE,0x10000400};
    call("_glReadPixels",read);
    uint8_t *pixel=memory+1024;
    assert(*(uint32_t *)(pixel+4)==0xa5a5a5a5);
    assert(pixel[0]>=63&&pixel[0]<=64&&pixel[1]>=127&&pixel[1]<=128&&pixel[2]>=191&&pixel[2]<=192&&pixel[3]==255);
    assert(glGetError()==GL_NO_ERROR);
    call("_glDeleteProgram",&program);for(unsigned i=0;i<2;++i)call("_glDeleteShader",&shaders[i]);
    glDeleteVertexArrays(1,&vao);glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&texture);CGLSetCurrentContext(NULL);CGLDestroyContext(context);
    CGLPixelFormatAttribute legacy[] = {kCGLPFAAccelerated, 0};
    assert(CGLChoosePixelFormat(legacy, &format, &count) == kCGLNoError);
    assert(CGLCreateContext(format, NULL, &context) == kCGLNoError);
    CGLDestroyPixelFormat(format);
    assert(CGLSetCurrentContext(context) == kCGLNoError);
    float shininess = 12.5f, observed = 0;
    uint32_t material[] = {0x0408, 0x1601, 0};
    memcpy(material + 2, &shininess, 4);
    call("_glMaterialf", material);
    glGetMaterialfv(0x0404, 0x1601, &observed);
    assert(observed == shininess);
    uint32_t point_size; shininess = 3.5f; memcpy(&point_size, &shininess, 4);
    call("_glPointSize", &point_size);
    glGetFloatv(GL_POINT_SIZE, &observed); assert(observed == shininess);
    glPixelStorei(GL_PACK_ALIGNMENT, 8);
    uint32_t mask = 1; /* GL_CLIENT_PIXEL_STORE_BIT */
    call("_glPushClientAttrib", &mask);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    call("_glPopClientAttrib", NULL);
    GLint restored; glGetIntegerv(GL_PACK_ALIGNMENT, &restored); assert(restored == 8);
    assert(glGetError() == GL_NO_ERROR);
    CGLSetCurrentContext(NULL); CGLDestroyContext(context);
    puts("Core GL bridge PASS (rendered pixels, shader ABI, legacy material/point floats, client state restoration)");
}
