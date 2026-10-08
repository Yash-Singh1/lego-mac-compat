#include "glc_gl_legacy.h"
#include "glcompare.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t fog_exp_bits(float value)
{ uint32_t bits; memcpy(&bits,&value,4); return bits; }
static const char *const fog_exp_models[] = {"fixed", "exp", "exp2", "weighted", "delta", "inputs"};
GLC_CASE_VARIANTS(legacy_fog_exp_diagnostic, fog_exp_models, .profile = GLC_LEGACY)
{
    GLint original;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&original);
    GLuint texture,framebuffer,program=0;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F_ARB,64,64,0,GL_RGBA,GL_FLOAT,NULL);
    glGenFramebuffers(1,&framebuffer);glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)glc_fail("Fog float target incomplete");
    glViewport(0,0,64,64);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_FOG);const float fog_color[]={.6f,.6f,.7f,1};glFogfv(GL_FOG_COLOR,fog_color);
    glFogi(GL_FOG_MODE,GL_EXP);glFogf(GL_FOG_START,1.5f);glFogf(GL_FOG_END,12);glFogf(GL_FOG_DENSITY,.25f);
    glMatrixMode(GL_PROJECTION);glLoadIdentity();glFrustum(-.5,.5,-.5,.5,1,20);glMatrixMode(GL_MODELVIEW);glLoadIdentity();
    if(glc_variant){
        const char *mixes[]={"", "mix(gl_Fog.color.rgb,gl_Color.rgb,f)", "mix(gl_Fog.color.rgb,gl_Color.rgb,f)",
            "gl_Color.rgb*f+gl_Fog.color.rgb*(1.0-f)","gl_Fog.color.rgb+(gl_Color.rgb-gl_Fog.color.rgb)*f"};
        char fragment[768];
        if(glc_variant==5)snprintf(fragment,sizeof fragment,
            "#version 120\nvoid main(){float z=abs(gl_FogFragCoord);gl_FragColor=vec4(gl_Color.g,z,exp(-gl_Fog.density*z),1);}");
        else snprintf(fragment,sizeof fragment,
            "#version 120\nvoid main(){float z=abs(gl_FogFragCoord);float f=%s;gl_FragColor=vec4(%s,gl_Color.a);}",
            glc_variant==2?"exp2(-gl_Fog.density*z*1.4426950408889634)":"exp(-gl_Fog.density*z)",mixes[glc_variant]);
        program=glc_program(
            "#version 120\nvoid main(){gl_Position=ftransform();gl_FrontColor=gl_Color;gl_BackColor=gl_Color;gl_FogFragCoord=-(gl_ModelViewMatrix*gl_Vertex).z;}",fragment,NULL);
    }else glUseProgram(0);
    glBegin(GL_QUADS);
    glColor3f(.9f,.3f,.1f);glFogCoordf(1);glVertex3f(-1.5f,-1,-1.2f);glFogCoordf(1);glVertex3f(1.5f,-1,-1.2f);
    glColor3f(.1f,.8f,.2f);glFogCoordf(12);glVertex3f(1.5f,1,-15);glFogCoordf(12);glVertex3f(-1.5f,1,-15);
    glEnd();float pixels[64*64*4];glReadPixels(0,0,64,64,GL_RGBA,GL_FLOAT,pixels);
    for(int y=16;y<=18;++y)for(int x=0;x<64;++x){int at=(y*64+x)*4;
        if(pixels[at+3]!=0 && (y==17 || x==32))fprintf(stderr,
            "Fog %s (%d,%d) value=(%.9g,%.9g,%.9g) bits=%08x,%08x,%08x G255=%.12g\n",fog_exp_models[glc_variant],x,y,
            pixels[at],pixels[at+1],pixels[at+2],fog_exp_bits(pixels[at]),fog_exp_bits(pixels[at+1]),fog_exp_bits(pixels[at+2]),(double)pixels[at+1]*255);}
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)original);glBlitFramebuffer(0,0,64,64,0,0,64,64,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)original);GLenum error=glGetError();if(error)glc_fail("Fog diagnostic GL error %x",error);
    if(program)glDeleteProgram(program);glDeleteFramebuffers(1,&framebuffer);glDeleteTextures(1,&texture);
}
