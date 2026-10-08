#include "glc_gl_legacy.h"
#include "glcompare.h"
#include <stdio.h>
static const char *const terminal_cases[]={"width2","width3","width4","width6","blend","stencil","ordering","point_size_restore"};
static void terminal_vertex(float x,float y){glVertex2f(x/32-1,y/32-1);}
GLC_CASE_VARIANTS(legacy_wide_terminal,terminal_cases,.profile=GLC_LEGACY)
{
    GLint original;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&original);GLuint texture,fbo,ds;
    glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    glGenRenderbuffers(1,&ds);glBindRenderbuffer(GL_RENDERBUFFER,ds);glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,64,64);glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,ds);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)glc_fail("Wide terminal target incomplete");
    glViewport(0,0,64,64);glClearColor(0,0,0,1);glClearStencil(0);glClear(GL_COLOR_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    glUseProgram(0);glDisable(GL_DEPTH_TEST);glDisable(GL_MULTISAMPLE);glDisable(GL_LINE_SMOOTH);glDisable(GL_FOG);glDisable(GL_LIGHTING);glDisable(GL_TEXTURE_2D);
    const float widths[]={2,3,4,6};glLineWidth(glc_variant<4?widths[glc_variant]:3);
    if(glc_variant==4){glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);}
    if(glc_variant==5){glEnable(GL_STENCIL_TEST);glStencilFunc(GL_ALWAYS,0,255);glStencilOp(GL_KEEP,GL_KEEP,GL_INCR);}
    if(glc_variant==7)glPointSize(7);
    glColor4f(1,.25f,.5f,glc_variant==4?.5f:1);
    glBegin(GL_LINES);
    if(glc_variant<4 && !(glc_variant&1)){terminal_vertex(6.4f,25.72f);terminal_vertex(56.6f,22.52f);}
    else {terminal_vertex(56.6f,28.92f);terminal_vertex(6.4f,25.72f);}
    if(glc_variant==6){glColor4f(0,0,1,1);terminal_vertex(6.5f,20);terminal_vertex(6.5f,32);}
    glEnd();
    if(glc_variant==5){unsigned char stencil[64*64];glReadPixels(0,0,64,64,GL_STENCIL_INDEX,GL_UNSIGNED_BYTE,stencil);
        for(int i=0;i<64*64;++i)if(stencil[i]>1)glc_fail("Wide terminal duplicate stencil write at %d,%d",i%64,i/64);
        if(stencil[25*64+6]!=1)glc_fail("Wide terminal stencil sample missing");glDisable(GL_STENCIL_TEST);}
    if(glc_variant==6){unsigned char pixel[4];glReadPixels(6,25,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);if(pixel[0]!=0||pixel[2]!=255)glc_fail("Wide terminal primitive order wrong");}
    if(glc_variant==7){glColor3f(0,1,0);glBegin(GL_POINTS);terminal_vertex(32.5f,40.5f);glEnd();}
    glDisable(GL_BLEND);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)original);glBlitFramebuffer(0,0,64,64,0,0,64,64,GL_COLOR_BUFFER_BIT,GL_NEAREST);glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)original);
    GLenum error=glGetError();if(error)glc_fail("Wide terminal GL error %x",error);glDeleteRenderbuffers(1,&ds);glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&texture);
}
