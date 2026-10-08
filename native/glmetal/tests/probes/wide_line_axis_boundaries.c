#include "glc_gl_legacy.h"
#include "glcompare.h"
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <math.h>
static const char *const axis_widths[]={"width2","width3","width4","width6"};
GLC_CASE_VARIANTS(legacy_wide_line_axis_boundaries,axis_widths,.profile=GLC_LEGACY)
{
 const int widths[]={2,3,4,6};const float fractions[]={0,.4f,.5f,.6f,.5f-1.f/1024,.5f+1.f/1024,.5f-3.f/1024,.5f+3.f/1024};
 const int fraction_count=sizeof fractions/sizeof fractions[0];
 int reconstruct=getenv("GLM_WIDE_AXIS_RECONSTRUCT")!=NULL;unsigned char pixels[64*64*4];
 glDisable(GL_MULTISAMPLE);glDisable(GL_LINE_SMOOTH);glDisable(GL_DEPTH_TEST);glClearColor(0,0,0,1);glColor4f(1,1,1,1);
 for(int axis=0;axis<2;++axis)for(int reverse=0;reverse<2;++reverse)for(int minorhalf=0;minorhalf<2;++minorhalf)for(int fraction=0;fraction<fraction_count+2;++fraction){
  float low=fraction<fraction_count?20+fractions[fraction]:fraction==fraction_count?20.4f:20.6f;
  float high=fraction<fraction_count?32+fractions[fraction]:fraction==fraction_count?20.6f:20.9f;
  float minor=6+(minorhalf?.5f:0);
  for(int control=0;control<2;++control){int width=control?widths[glc_variant]:1;glLineWidth(width);glClear(GL_COLOR_BUFFER_BIT);
   double snapped_low=floor((double)low*256+.5)/256,snapped_high=floor((double)high*256+.5)/256;
   float first=(float)(ceil(snapped_low-.5)+.5),last=(float)(ceil(snapped_high-.5)-.5);
   int empty=control && reconstruct && first>last;
   if(!empty){glBegin(GL_LINES);for(int i=0;i<2;++i){int end=reverse?1-i:i;float major=end?high:low;
       if(control && reconstruct)major=i==0?(reverse?last:first):(reverse?first-.75f:last+.75f);
       glVertex2f((axis?minor:major)/32-1,(axis?major:minor)/32-1);}glEnd();}
   glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
   fprintf(stderr,"AxisBoundary width%d axis%d reverse%d minor%.1f fraction%.8f short%d reconstruct%d:",width,axis,reverse,minor,fraction<fraction_count?fractions[fraction]:-1.0f,fraction>=fraction_count?fraction-fraction_count+1:0,control&&reconstruct);
   for(int major=18;major<35;++major){fprintf(stderr," %d=",major);for(int across=2;across<11;++across){int x=axis?across:major,y=axis?major:across;if(pixels[(y*64+x)*4])fprintf(stderr,"%d,",across);}}
   fprintf(stderr,"\n");
  }
  const struct timespec pause={0,50000000};nanosleep(&pause,NULL);
 }
 GLenum error=glGetError();if(error)glc_fail("Wide axis diagnostic GL error %x",error);
}
