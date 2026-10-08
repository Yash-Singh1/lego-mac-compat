#include "glc_gl_legacy.h"
#include "glcompare.h"
#include <stdio.h>
/* Fractional high endpoints distinguish center-interval coverage from sorting
 * directed narrow lines. Constant color keeps this a coverage-only probe. */
static const char *const names[]={
 "x_up_forward_integral","x_up_reverse_integral","y_up_forward_integral","y_up_reverse_integral",
 "x_down_forward_integral","x_down_reverse_integral","y_down_forward_integral","y_down_reverse_integral",
 "x_up_forward_fractional","x_up_reverse_fractional","y_up_forward_fractional","y_up_reverse_fractional",
 "x_down_forward_fractional","x_down_reverse_fractional","y_down_forward_fractional","y_down_reverse_fractional"};
GLC_CASE_VARIANTS(legacy_wide_line_interval_diagnostic,names,.profile=GLC_LEGACY)
{
 int reverse=glc_variant&1,ymajor=glc_variant&2,down=glc_variant&4,fractional=glc_variant&8;
 float p[2][2]={{6.4f,25.72f},{fractional?56.6f:56.0f,down?22.52f:28.92f}};
 glDisable(GL_MULTISAMPLE);glDisable(GL_LINE_SMOOTH);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);glLineWidth(3);glColor3f(1,1,1);
 glBegin(GL_LINES);for(int i=0;i<2;++i){int v=reverse?1-i:i;glVertex2f(p[v][ymajor?1:0]/32-1,p[v][ymajor?0:1]/32-1);}glEnd();
 unsigned char rgba[64*64*4];glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
 fprintf(stderr,"WideInterval %s:",names[glc_variant]);for(int major=5;major<=57;++major)if(major<=7||major>=54){fprintf(stderr," %d=",major);
  for(int minor=16;minor<36;++minor){int x=ymajor?minor:major,y=ymajor?major:minor;if(rgba[(y*64+x)*4])fprintf(stderr,"%d,",minor);}}
 fprintf(stderr,"\n");
}
