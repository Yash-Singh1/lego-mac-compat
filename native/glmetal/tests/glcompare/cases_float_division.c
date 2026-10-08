#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdio.h>
#include <string.h>

static const char *const division_modes[] = {"510", "255", "127", "3", "7", "10", "32", "precise_510", "precise_255"};
GLC_CASE_VARIANTS(core_float_constant_division, division_modes, .profile = GLC_CORE)
{
    const float divisors[] = {510,255,127,3,7,10,32,510,255};
    float divisor = divisors[glc_variant];
    char fragment[768];
    snprintf(fragment, sizeof fragment,
        "#version 410 core\nuniform float divisor;out vec4 frag;void main(){"
        "float n=float((int(gl_FragCoord.x)+64*int(gl_FragCoord.y))%%%d);"
        "%s float value=n/%.1f;frag=vec4(value,n*(1.0/%.1f),n/divisor,1);}",
        (int)divisor+1, glc_variant>=7?"precise":"", divisor, divisor);
    GLuint program=glc_program(
        "#version 410 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2-1,0,1);}",
        fragment,NULL);
    glUniform1f(glGetUniformLocation(program,"divisor"),divisor);
    GLint original;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&original);
    GLuint vao,texture,framebuffer;glGenVertexArrays(1,&vao);glBindVertexArray(vao);
    glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,64,64,0,GL_RGBA,GL_FLOAT,NULL);
    glGenFramebuffers(1,&framebuffer);glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)glc_fail("Division float target incomplete");
    glDrawArrays(GL_TRIANGLES,0,3);
    float pixels[64*64*4];glReadPixels(0,0,64,64,GL_RGBA,GL_FLOAT,pixels);
    int exact=0,reciprocal=0,shader_reciprocal=0,dynamic=0;
    volatile float den=divisor;volatile float inv=1.0f/den;
    for(int i=0;i<4096;++i){
        volatile float n=(float)(i%((int)divisor+1));
        float q=n/den,m=n*inv;
        exact+=pixels[i*4]==q;reciprocal+=pixels[i*4]==m;
        shader_reciprocal+=pixels[i*4]==pixels[i*4+1];dynamic+=pixels[i*4]==pixels[i*4+2];
    }
    fprintf(stderr,"Division %s: exact=%d reciprocal=%d shader_reciprocal=%d dynamic=%d of4096\n",
            division_modes[glc_variant],exact,reciprocal,shader_reciprocal,dynamic);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)original);
    glBlitFramebuffer(0,0,64,64,0,0,64,64,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)original);
    GLenum error=glGetError();if(error)glc_fail("Division GL error %x",error);
    glDeleteFramebuffers(1,&framebuffer);glDeleteTextures(1,&texture);
    glDeleteVertexArrays(1,&vao);glDeleteProgram(program);
}
