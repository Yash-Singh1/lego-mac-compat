#include "glc_gl_legacy.h"
#include "glcompare.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Diagnostic only: compare the observed wide mask against native width-one
 * replicas, then predict the missing terminal samples with a diamond exit. */
static const char *const terminal_variants[]={"width2","width3","width4","width6"};
static int diamond_exit(double ax,double ay,double bx,double by,double cx,double cy)
{
    /* A pixel's open diamond is |x-cx|+|y-cy|<1/2. Transforming to x+y
     * and x-y makes it an axis-aligned square. Strict interior excludes
     * tangencies without an arbitrary numerical epsilon. */
    double start[2]={ax+ay-cx-cy,ax-ay-cx+cy};
    double delta[2]={bx+by-ax-ay,bx-by-ax+ay};
    double enter=0,exit=1;
    for(int k=0;k<2;++k){
        if(delta[k]==0){if(!(start[k]>-.5 && start[k]<.5))return 0;continue;}
        double lo=(-.5-start[k])/delta[k],hi=(.5-start[k])/delta[k];
        if(lo>hi){double temp=lo;lo=hi;hi=temp;}
        if(lo>enter)enter=lo;if(hi<exit)exit=hi;
    }
    return enter<exit && exit<1 && exit>0;
}
static void terminal_draw(double ax,double ay,double bx,double by,int width,int ymajor,int reverse,unsigned char *rgba)
{
    glClear(GL_COLOR_BUFFER_BIT);glLineWidth(width);glBegin(GL_LINES);
    for(int e=0;e<2;++e){int v=reverse?1-e:e;double x=v?bx:ax,y=v?by:ay;
        glVertex2f((float)((ymajor?y:x)/32-1),(float)((ymajor?x:y)/32-1));}
    glEnd();glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
}
GLC_CASE_VARIANTS(legacy_wide_line_terminal_predicate,terminal_variants,.profile=GLC_LEGACY)
{
    static const int widths[]={2,3,4,6};static const double fractions[]={0,.4,.5,.6};int width=widths[glc_variant];
    unsigned char wide[64*64*4],narrow[64*64*4];unsigned observed[64*64],predicted[64*64];
    glDisable(GL_MULTISAMPLE);glDisable(GL_LINE_SMOOTH);glDisable(GL_BLEND);glClearColor(0,0,0,1);glColor3f(1,1,1);
    unsigned missing_total=0,duplicate_total=0,unpredicted_total=0,diamond_errors=0;
    for(int axis=0;axis<2;++axis)for(int slope=0;slope<2;++slope)for(int reverse=0;reverse<2;++reverse)for(int low=0;low<4;++low)for(int high=0;high<4;++high){
        double ax=6+fractions[low],ay=25.72,bx=56+fractions[high],by=slope?22.52:28.92;
        terminal_draw(ax,ay,bx,by,width,axis,reverse,wide);memset(observed,0,sizeof observed);memset(predicted,0,sizeof predicted);
        int diamond_bad=0,terminal_diamond_bad=0,missing=0,duplicate=0,unpredicted=0;
        for(int copy=0;copy<width;++copy){double offset=copy-(width-1)*.5;
            terminal_draw(ax,ay+offset,bx,by+offset,1,axis,reverse,narrow);
            for(int major=5;major<58;++major)for(int minor=16;minor<36;++minor){int pixel=axis?major*64+minor:minor*64+major;
                int actual=narrow[pixel*4]!=0;observed[pixel]|=(unsigned)actual;
                double startx=reverse?bx:ax,starty=(reverse?by:ay)+offset,endx=reverse?ax:bx,endy=(reverse?ay:by)+offset;
                int diamond=diamond_exit(startx,starty,endx,endy,major+.5,minor+.5);
                if(diamond!=actual){++diamond_bad;
                    if(major==(int)floor(endx))++terminal_diamond_bad;
                    if(diamond_bad<=4)fprintf(stderr,"TerminalDiamond width%d axis%d slope%d reverse%d low%.1f high%.1f copy%d major%d minor%d predicted%d actual%d\n",width,axis,slope,reverse,fractions[low],fractions[high],copy,major,minor,diamond,actual);
                }
                /* Only the endpoint's major column can differ. A half-open
                 * interval includes the low endpoint and excludes the high. */
                int terminal=(int)floor(endx);
                if(major==terminal && major+.5>=ax && major+.5<bx){
                    double liney=ay+offset+(major+.5-ax)*(by-ay)/(bx-ax);
                    int expected=minor==(int)floor(liney);
                    if(expected && !diamond)predicted[pixel]=1;
                }
            }
        }
        for(int pixel=0;pixel<64*64;++pixel){int w=wide[pixel*4]!=0;
            if(w&&!observed[pixel])++missing;
            if(predicted[pixel]&&observed[pixel])++duplicate;
            if((w&&!observed[pixel])!=(predicted[pixel]!=0))++unpredicted;
        }
        missing_total+=missing;duplicate_total+=duplicate;unpredicted_total+=unpredicted;diamond_errors+=diamond_bad;
        fprintf(stderr,"TerminalPredicate width%d axis%d slope%d reverse%d low%.1f high%.1f missing%d duplicate%d mismatch%d diamond%d terminalDiamond%d\n",width,axis,slope,reverse,fractions[low],fractions[high],missing,duplicate,unpredicted,diamond_bad,terminal_diamond_bad);
        const struct timespec pause={0,50000000};nanosleep(&pause,NULL);
    }
    fprintf(stderr,"TerminalPredicate summary width%d missing%u duplicate%u mismatch%u diamond%u\n",width,missing_total,duplicate_total,unpredicted_total,diamond_errors);
    glClear(GL_COLOR_BUFFER_BIT);GLenum error=glGetError();if(error)glc_fail("Terminal predicate GL error %x",error);
}
