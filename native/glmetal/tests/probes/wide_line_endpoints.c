#include "glc_gl_legacy.h"
#include "glcompare.h"
#include <stdio.h>
static const char *const variants[]={"width1","width2","width3","width4","width5","width6","reverse1","reverse2","reverse3","reverse4","reverse5","reverse6"};
GLC_CASE_VARIANTS(legacy_wide_line_endpoints,variants,.profile=GLC_LEGACY)
{
    int width=1+glc_variant%6,reverse=glc_variant>=6;
    glDisable(GL_MULTISAMPLE);glDisable(GL_LINE_SMOOTH);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    glLineWidth(width);glColor3f(1,1,1);
    glBegin(GL_LINES);
    float p[2][2]={{6.4f,25.72f},{56,28.92f}};
    for(int i=0;i<2;++i){int v=reverse?1-i:i;glVertex2f(p[v][0]/32-1,p[v][1]/32-1);}
    glEnd();unsigned char rgba[64*64*4];glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
    fprintf(stderr,"Endpoint %s:",variants[glc_variant]);
    for(int x=5;x<=57;++x)if(x<=7||x>=54){fprintf(stderr," x%d=",x);for(int y=16;y<36;++y)if(rgba[(y*64+x)*4])fprintf(stderr,"%d,",y);}
    fprintf(stderr,"\n");
}
