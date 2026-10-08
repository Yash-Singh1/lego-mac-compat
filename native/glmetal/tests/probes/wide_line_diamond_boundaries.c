#include "glc_gl_legacy.h"
#include "glcompare.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
static const char *const boundary_names[]={"diagonal","strip"};
static uint32_t boundary_word(float f){uint32_t u;memcpy(&u,&f,4);return u;}
static void boundary_draw(int fixture,int copy,int reverse,int axis,int shift,int wide)
{
 float p[2][2];
 if(!fixture){p[0][0]=-.75f;p[0][1]=-.8f;p[1][0]=-.5f;p[1][1]=.75f;}
 else{float y=-.7f;p[0][0]=.75f;p[0][1]=y+.1f;p[1][0]=.35f;p[1][1]=y+.3f;}
 if(!wide){float offset=(copy? .5f:-.5f)/32;for(int e=0;e<2;++e)p[e][fixture?1:0]+=offset;}
 /* Only the original terminal endpoint is moved in the shifted controls. */
 p[1][axis]+=shift/(256.0f*32);
 glClear(GL_COLOR_BUFFER_BIT);glLineWidth(wide?2:1);glBegin(GL_LINES);
 for(int i=0;i<2;++i){int end=reverse?1-i:i;glColor4f(end?.75f:.25f,end?.125f:.875f,end?.875f:.125f,1);glVertex2fv(p[end]);}glEnd();
}
GLC_CASE_VARIANTS(legacy_wide_line_diamond_boundaries,boundary_names,.profile=GLC_LEGACY)
{
 int fixture=glc_variant;GLint original;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&original);GLuint texture,fbo;
 glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F_ARB,64,64,0,GL_RGBA,GL_FLOAT,NULL);
 glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
 if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)glc_fail("Diamond boundary float target incomplete");
 glViewport(0,0,64,64);glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();glUseProgram(0);
 glDisable(GL_MULTISAMPLE);glDisable(GL_LINE_SMOOTH);glDisable(GL_DEPTH_TEST);glDisable(GL_FOG);glDisable(GL_LIGHTING);glDisable(GL_TEXTURE_2D);glClearColor(0,0,0,0);
 float pixels[64*64*4];int coverage[2][2]={{0}};
 boundary_draw(fixture,0,0,0,0,1);glReadPixels(0,0,64,64,GL_RGBA,GL_FLOAT,pixels);
 for(int copy=0;copy<2;++copy){int x=fixture?43:15+copy,y=fixture?18+copy:55;float *p=pixels+(y*64+x)*4;
  fprintf(stderr,"BoundaryWide %s copy%d (%d,%d) covered%d RGB%.9g,%.9g,%.9g bits=%08x,%08x,%08x\n",boundary_names[fixture],copy,x,y,p[3]!=0,p[0],p[1],p[2],boundary_word(p[0]),boundary_word(p[1]),boundary_word(p[2]));}
 const struct timespec pause={0,50000000};nanosleep(&pause,NULL);
 for(int copy=0;copy<2;++copy)for(int reverse=0;reverse<2;++reverse)for(int axis=0;axis<2;++axis)for(int shift=-1;shift<=1;++shift){
  boundary_draw(fixture,copy,reverse,axis,shift,0);glReadPixels(0,0,64,64,GL_RGBA,GL_FLOAT,pixels);
  int tx=fixture?43:15+copy,ty=fixture?18+copy:55;
  if(!shift)coverage[copy][reverse]=pixels[(ty*64+tx)*4+3]!=0;
  for(int y=ty-1;y<=ty+1;++y)for(int x=tx-1;x<=tx+1;++x){float *p=pixels+(y*64+x)*4;
   fprintf(stderr,"BoundaryNative %s copy%d reverse%d axis%d shift%d (%d,%d) covered%d RGB%.9g,%.9g,%.9g bits=%08x,%08x,%08x\n",boundary_names[fixture],copy,reverse,axis,shift,x,y,p[3]!=0,p[0],p[1],p[2],boundary_word(p[0]),boundary_word(p[1]),boundary_word(p[2]));}
  nanosleep(&pause,NULL);
 }
 for(int copy=0;copy<2;++copy)fprintf(stderr,"BoundaryProof %s copy%d original%d reversed%d eligible%d duplicate%d\n",boundary_names[fixture],copy,coverage[copy][0],coverage[copy][1],!coverage[copy][0]&&coverage[copy][1],coverage[copy][0]&&coverage[copy][1]);
 glBindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)original);glBlitFramebuffer(0,0,64,64,0,0,64,64,GL_COLOR_BUFFER_BIT,GL_NEAREST);glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)original);
 GLenum error=glGetError();if(error)glc_fail("Diamond boundary GL error %x",error);glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&texture);
}
