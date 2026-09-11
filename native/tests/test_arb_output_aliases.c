#include "arb_program_guard.h"
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
int main(void) {
    size_t normalized_size;char *normalized=arb_program_normalize_line_endings("a\rb\r\nc\n",7,&normalized_size);
    assert(normalized && normalized_size==6 && !strcmp(normalized,"a\nb\nc\n"));free(normalized);

    const char *lexical = "!!ARBvp1.0\r\n# OUTPUT ignored = result.color;\r\n"
        "OUTPUT uv = result.texcoord[0]; OUTPUT uv2=result.texcoord[1];\n"
        "TEMP uv20;\nMOV uv.xy, vertex.attrib[8]; # uv must stay a comment\n"
        "MOV uv2, uv20;\nEND";
    size_t size;
    char *expanded=arb_program_expand_output_aliases(lexical,strlen(lexical),&size);
    assert(expanded && size==strlen(expanded));
    assert(strstr(expanded,"# OUTPUT ignored = result.color;"));
    assert(strstr(expanded,"MOV result.texcoord[0].xy, vertex.attrib[8]; # uv must stay a comment"));
    assert(strstr(expanded,"MOV result.texcoord[1], uv20;"));
    free(expanded);
    const char *bad="!!ARBvp1.0\nOUTPUT x = result.position";
    assert(!arb_program_expand_output_aliases(bad,strlen(bad),&size));
    const char *duplicate="!!ARBvp1.0\nOUTPUT x=result.position;OUTPUT x=result.color;END";
    assert(!arb_program_expand_output_aliases(duplicate,strlen(duplicate),&size));
    const char *plain="!!ARBvp1.0\nMOV result.position,vertex.attrib[0];END";
    assert(!arb_program_expand_output_aliases(plain,strlen(plain),&size));
    const char *vertex="!!ARBvp1.0\nOUTPUT pos=result.position;\n"
        "OUTPUT color=result.color;OUTPUT unused=result.color.secondary;\n"
        "OUTPUT uv=result.texcoord[0];\nMOV pos,vertex.attrib[0];\n"
        "MOV uv,{0,0,0,1};\nMOV uv.xy,vertex.attrib[8];\nEND";
    const char *fragment="!!ARBfp1.0\r\r# options precede declarations\rOPTION ARB_fragment_program_shadow;\rTEMP scratch;\rRSQ scratch.x,{1};\rOUTPUT color=result.color;\r"
        "MOV color.xy,fragment.texcoord[0];MOV color.zw,{0,0,0,1};END";
    CGLPixelFormatAttribute attrs[]={kCGLPFAAccelerated,kCGLPFAColorSize,24,0};
    CGLPixelFormatObj format;GLint count;CGLContextObj context;
    assert(!CGLChoosePixelFormat(attrs,&format,&count));
    assert(!CGLCreateContext(format,NULL,&context));CGLDestroyPixelFormat(format);
    assert(!CGLSetCurrentContext(context));
    GLuint texture,fbo,programs[2];
    glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,32,32,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glGenFramebuffersEXT(1,&fbo);glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_2D,texture,0);
    assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT)==GL_FRAMEBUFFER_COMPLETE_EXT);
    glViewport(0,0,32,32);glGenProgramsARB(2,programs);
    GLenum targets[]={GL_VERTEX_PROGRAM_ARB,GL_FRAGMENT_PROGRAM_ARB};
    const char *sources[]={vertex,fragment};
    for(unsigned i=0;i<2;++i){
        normalized=arb_program_normalize_line_endings(sources[i],strlen(sources[i]),&normalized_size);
        const char *source=normalized?normalized:sources[i];
        expanded=arb_program_expand_output_aliases(source,strlen(source),&size);assert(expanded);free(normalized);
        size_t guarded_size;char *guarded=arb_program_guard_undefined_math(expanded,size,&guarded_size,NULL,NULL);
        if(guarded){free(expanded);expanded=guarded;size=guarded_size;}
        glBindProgramARB(targets[i],programs[i]);
        glProgramStringARB(targets[i],GL_PROGRAM_FORMAT_ASCII_ARB,(GLsizei)size,expanded);
        assert(!glGetError());glEnable(targets[i]);free(expanded);
    }
    const GLfloat vertices[]={-1,-1,0,1,0,0, 1,-1,0,1,1,0,
                                1,1,0,1,1,1, -1,1,0,1,0,1};
    glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,6*sizeof(float),vertices);
    glVertexAttribPointer(8,2,GL_FLOAT,GL_FALSE,6*sizeof(float),vertices+4);
    glEnableVertexAttribArray(0);glEnableVertexAttribArray(8);
    const GLushort indices[]={0,1,2,0,2,3};
    glDrawRangeElements(GL_TRIANGLES,0,3,6,GL_UNSIGNED_SHORT,indices);
    unsigned char pixel[4];
    glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    assert(pixel[0]>=66 && pixel[0]<=70 && pixel[1]>=66 && pixel[1]<=70);
    glReadPixels(24,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    assert(pixel[0]>=193 && pixel[0]<=197 && pixel[1]>=66 && pixel[1]<=70);
    assert(!glGetError());glDeleteProgramsARB(2,programs);
    glDeleteFramebuffersEXT(1,&fbo);glDeleteTextures(1,&texture);
    CGLSetCurrentContext(NULL);CGLDestroyContext(context);
    puts("ARB output aliases PASS (line endings, option ordering, lexical boundaries, native interpolated pixels)");
}
