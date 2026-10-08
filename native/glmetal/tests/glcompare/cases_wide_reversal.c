#include "glc_gl_legacy.h"
#include "glcompare.h"
static const char *const reversal_names[]={"forward","reverse","blend","stencil","scissor","scissor_restore","ordering","flat"};
static void reversal_vertex(int end,int up,float alpha)
{
 glColor4f(end?.75f:.25f,end?.875f:.125f,end?.25f:.75f,alpha);
 glVertex2f((end?56.6f:6.4f)/32-1,(end?(up?28.92f:22.52f):25.72f)/32-1);
}
GLC_CASE_VARIANTS(legacy_wide_reversal,reversal_names,.profile=GLC_LEGACY)
{
 GLint original;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&original);GLuint color,fbo,ds;
 glGenTextures(1,&color);glBindTexture(GL_TEXTURE_2D,color);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
 glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,color,0);
 glGenRenderbuffers(1,&ds);glBindRenderbuffer(GL_RENDERBUFFER,ds);glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,64,64);glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,ds);
 if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)glc_fail("Wide reversal target incomplete");
 glViewport(0,0,64,64);glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();glUseProgram(0);
 glDisable(GL_DEPTH_TEST);glDisable(GL_MULTISAMPLE);glDisable(GL_LINE_SMOOTH);glDisable(GL_FOG);glDisable(GL_LIGHTING);glDisable(GL_TEXTURE_2D);
 glClearColor(0,0,0,1);glClearStencil(0);glClear(GL_COLOR_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);glLineWidth(3);
 int reverse=glc_variant!=0;float alpha=glc_variant==2?.5f:1;
 if(glc_variant==2){glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);}
 if(glc_variant==3){glEnable(GL_STENCIL_TEST);glStencilFunc(GL_ALWAYS,0,255);glStencilOp(GL_KEEP,GL_KEEP,GL_INCR);}
 if(glc_variant==4){glEnable(GL_SCISSOR_TEST);glScissor(6,25,1,1);}
 if(glc_variant==7)glShadeModel(GL_FLAT);
 glBegin(GL_LINES);reversal_vertex(reverse?1:0,reverse,alpha);reversal_vertex(reverse?0:1,reverse,alpha);
 if(glc_variant==6){glColor4f(0,1,0,1);glVertex2f(6.5f/32-1,20.0f/32-1);glVertex2f(6.5f/32-1,32.0f/32-1);}glEnd();
 if(glc_variant==3){unsigned char values[64*64];glReadPixels(0,0,64,64,GL_STENCIL_INDEX,GL_UNSIGNED_BYTE,values);for(int i=0;i<64*64;++i)if(values[i]>1)glc_fail("Wide reversal duplicate stencil write");if(values[25*64+6]!=1)glc_fail("Wide reversal terminal sample missing");glDisable(GL_STENCIL_TEST);}
 if(glc_variant==4){GLint box[4];glGetIntegerv(GL_SCISSOR_BOX,box);if(!glIsEnabled(GL_SCISSOR_TEST)||box[0]!=6||box[1]!=25||box[2]!=1||box[3]!=1)glc_fail("Wide reversal application scissor changed");glDisable(GL_SCISSOR_TEST);}
 if(glc_variant==5){if(glIsEnabled(GL_SCISSOR_TEST))glc_fail("Wide reversal enabled internal scissor permanently");glColor3f(0,1,0);glBegin(GL_TRIANGLES);glVertex2f(.2f,.2f);glVertex2f(.9f,.2f);glVertex2f(.9f,.9f);glEnd();}
 if(glc_variant==6){unsigned char pixel[4];glReadPixels(6,25,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);if(pixel[0]||pixel[1]!=255||pixel[2])glc_fail("Wide reversal primitive ordering wrong");}
 glDisable(GL_BLEND);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)original);glBlitFramebuffer(0,0,64,64,0,0,64,64,GL_COLOR_BUFFER_BIT,GL_NEAREST);glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)original);
 GLenum error=glGetError();if(error)glc_fail("Wide reversal GL error %x",error);glDeleteRenderbuffers(1,&ds);glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&color);
}
