#include "glc_gl_legacy.h"
#include "glcompare.h"
#include "wide_symbolic_diamond.h"
#include <math.h>
#include <stdio.h>
#include <time.h>
static const char *const symbolic_edges[]={"upper_tip","upper_right","right_tip","lower_right","lower_tip","lower_left","left_tip","upper_left"};
static const float edge[8][2]={{0,.5f},{.25f,.25f},{.5f,0},{.25f,-.25f},{0,-.5f},{-.25f,-.25f},{-.5f,0},{-.25f,.25f}};
static const float octant[8][2]={{12,4},{4,12},{-4,12},{-12,4},{-12,-4},{-4,-12},{4,-12},{12,-4}};
GLC_CASE_VARIANTS(legacy_wide_line_nonaxis_symbolic,symbolic_edges,.profile=GLC_LEGACY)
{
 glViewport(0,0,64,64);glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();glUseProgram(0);
 glDisable(GL_MULTISAMPLE);glDisable(GL_LINE_SMOOTH);glDisable(GL_DEPTH_TEST);glDisable(GL_FOG);glDisable(GL_LIGHTING);glDisable(GL_TEXTURE_2D);glLineWidth(1);glClearColor(0,0,0,1);glColor4f(1,1,1,1);
 unsigned errors=0;const struct timespec pause={0,50000000};
 for(int oct=0;oct<8;++oct)for(int shift=-1;shift<=1;++shift){
  float p[2][2]={{32.5f+octant[oct][0],32.5f+octant[oct][1]},{32.5f+edge[glc_variant][0],32.5f+edge[glc_variant][1]}};
  int axis=edge[glc_variant][1]!=0?1:0;float sign=edge[glc_variant][axis]>0?1:-1;p[1][axis]+=sign*shift/256.0f;
  int observed[2],expected[2];
  for(int reverse=0;reverse<2;++reverse){int a=reverse?1:0,b=1-a;
   expected[reverse]=glm_probe_symbolic_diamond(lroundf(p[a][0]*256),lroundf(p[a][1]*256),lroundf(p[b][0]*256),lroundf(p[b][1]*256),8320,8320);
   glClear(GL_COLOR_BUFFER_BIT);glBegin(GL_LINES);glVertex2f(p[a][0]/32-1,p[a][1]/32-1);glVertex2f(p[b][0]/32-1,p[b][1]/32-1);glEnd();
   unsigned char pixel[4];glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);observed[reverse]=pixel[0]!=0;
   if(observed[reverse]!=expected[reverse])++errors;
  }
  fprintf(stderr,"NonaxisSymbolic %s octant%d shift%d endpoint%.9g,%.9g original%d reversed%d expected%d,%d eligible%d duplicate%d\n",symbolic_edges[glc_variant],oct,shift,p[1][0],p[1][1],observed[0],observed[1],expected[0],expected[1],!observed[0]&&observed[1],observed[0]&&observed[1]);
  nanosleep(&pause,NULL);
 }
 GLenum error=glGetError();if(error)glc_fail("Nonaxis symbolic GL error %x",error);
 if(errors)glc_fail("Nonaxis symbolic coverage disagreed at %u native samples",errors);
}
