/* Standalone interval-scissor diagnostic; root owns GPU execution. */
#include "glc_gl_legacy.h"
#include "glcompare.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
static const char *const modes[]={"loop","viewport"};
GLC_CASE_VARIANTS(legacy_wide_major_interval,modes,.profile=GLC_LEGACY)
{
 int trim=getenv("GLM_WIDE_MAJOR_SCISSOR")!=NULL;
 glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
 glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();
 glDisable(GL_LINE_SMOOTH);glDisable(GL_MULTISAMPLE);glDisable(GL_DEPTH_TEST);
 int vp[4]={0,0,64,64};if(glc_variant){vp[0]=7;vp[1]=9;vp[2]=48;vp[3]=40;}
 glViewport(vp[0],vp[1],vp[2],vp[3]);
 for(int i=0;i<4;++i){
  const float widths[]={2,3,4.4f,5.6f};float y=-.7f+.43f*i;
  float p[3][2]={{-.8f,y},{.75f,y+.1f},{.35f,y+.3f}};
  float colors[3][3]={{.2f*i+.2f,1,.3f},{1,.1f*i+.2f,.6f},{1,.1f*i+.2f,.6f}};
  glLineWidth(widths[i]);
  int lines=glc_variant?1:3;
  for(int line=0;line<lines;++line){int a=line,b=(line+1)%3;
   if(trim){
    double w[2][2];for(int e=0;e<2;++e)for(int c=0;c<2;++c){int v=e?b:a;w[e][c]=vp[c]+(p[v][c]*.5+.5)*vp[c+2];w[e][c]=floor(w[e][c]*256+.5)/256;}
    int major=fabs(w[1][0]-w[0][0])>=fabs(w[1][1]-w[0][1])?0:1;
    int first=(int)ceil(fmin(w[0][major],w[1][major])-.5),last=(int)ceil(fmax(w[0][major],w[1][major])-.5);
    glEnable(GL_SCISSOR_TEST);glScissor(major?0:first,major?first:0,major?64:last-first,major?last-first:64);
    fprintf(stderr,"MajorInterval mode%d line%d/%d major%d [%d,%d)\n",glc_variant,i,line,major,first,last);
   }
   glBegin(GL_LINES);glColor3fv(colors[a]);glVertex2fv(p[a]);glColor3fv(colors[b]);glVertex2fv(p[b]);glEnd();
   if(trim)glDisable(GL_SCISSOR_TEST);
  }
 }
 GLenum error=glGetError();if(error)glc_fail("Major interval diagnostic GL error %x",error);
}
