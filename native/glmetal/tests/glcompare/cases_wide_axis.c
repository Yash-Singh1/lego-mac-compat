#include "glc_gl_legacy.h"
#include "glcompare.h"
static const char *const wide_axis_cases[]={"width2","width3","width4","width6","half_above","half_below","reverse","one_pixel","empty","stencil"};
static void wide_axis_line(int vertical,int reverse,float minor,float low,float high)
{
 glBegin(GL_LINES);for(int i=0;i<2;++i){int end=reverse?1-i:i;float major=end?high:low;glVertex2f((vertical?minor:major)/32-1,(vertical?major:minor)/32-1);}glEnd();
}
GLC_CASE_VARIANTS(legacy_wide_axis,wide_axis_cases,.profile=GLC_LEGACY)
{
 GLint original;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&original);GLuint color,fbo,stencil;
 glGenTextures(1,&color);glBindTexture(GL_TEXTURE_2D,color);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
 glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,color,0);
 glGenRenderbuffers(1,&stencil);glBindRenderbuffer(GL_RENDERBUFFER,stencil);glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,64,64);glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,stencil);
 if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)glc_fail("Wide axis target incomplete");
 glViewport(0,0,64,64);glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();
 glUseProgram(0);glDisable(GL_DEPTH_TEST);glDisable(GL_MULTISAMPLE);glDisable(GL_LINE_SMOOTH);glDisable(GL_FOG);glDisable(GL_LIGHTING);glDisable(GL_TEXTURE_2D);
 glClearColor(0,0,0,1);glClearStencil(0);glClear(GL_COLOR_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);glColor4f(.25f,.5f,1,1);
 const float widths[]={2,3,4,6};glLineWidth(glc_variant<4?widths[glc_variant]:3);
 float fraction=glc_variant==4?.5009765625f:glc_variant==5?.4990234375f:0;
 if(glc_variant==9){glEnable(GL_STENCIL_TEST);glStencilFunc(GL_ALWAYS,0,255);glStencilOp(GL_KEEP,GL_KEEP,GL_INCR);wide_axis_line(1,0,6.5f,20,32);}
 else if(glc_variant==7){wide_axis_line(0,0,10.5f,20.4f,20.6f);wide_axis_line(1,1,40.5f,20.4f,20.6f);}
 else if(glc_variant==8){wide_axis_line(0,0,10.5f,20.6f,20.9f);wide_axis_line(1,1,40.5f,20.6f,20.9f);}
 else {wide_axis_line(0,glc_variant==6,10,20+fraction,32+fraction);wide_axis_line(0,1,20.5f,20+fraction,32+fraction);wide_axis_line(1,0,40,20+fraction,32+fraction);wide_axis_line(1,1,50.5f,20+fraction,32+fraction);}
 if(glc_variant==9){unsigned char values[64*64];glReadPixels(0,0,64,64,GL_STENCIL_INDEX,GL_UNSIGNED_BYTE,values);unsigned count=0;
  for(int i=0;i<64*64;++i){if(values[i]>1)glc_fail("Wide axis duplicate stencil sample");count+=values[i]!=0;}
  if(count!=36||values[19*64+6]||values[20*64+6]!=1||values[31*64+6]!=1||values[32*64+6])glc_fail("Wide axis half-open coverage wrong");glDisable(GL_STENCIL_TEST);}
 if(glc_variant==8){unsigned char pixels[64*64*4];glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,pixels);for(int i=0;i<64*64;++i)if(pixels[i*4]||pixels[i*4+1]||pixels[i*4+2])glc_fail("Empty wide axis interval produced samples");}
 glBindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)original);glBlitFramebuffer(0,0,64,64,0,0,64,64,GL_COLOR_BUFFER_BIT,GL_NEAREST);glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)original);
 GLenum error=glGetError();if(error)glc_fail("Wide axis GL error %x",error);glDeleteRenderbuffers(1,&stencil);glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&color);
}
