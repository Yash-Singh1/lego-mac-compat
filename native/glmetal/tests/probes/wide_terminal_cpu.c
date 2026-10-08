/* Appended after the production helper by run_wide_terminal_cpu.sh. */
#include <assert.h>
#include <stdio.h>
#include "wide_symbolic_diamond.h"
int main(void)
{
 const int widths[]={2,3,4,6};const double fractions[]={0,.4,.5,.6};
 for(int wi=0;wi<4;++wi){unsigned count=0;int width=widths[wi];
  for(int axis=0;axis<2;++axis)for(int slope=0;slope<2;++slope)for(int reverse=0;reverse<2;++reverse)for(int lo=0;lo<4;++lo)for(int hi=0;hi<4;++hi){
   int previous_x=-1,previous_y=-1;
   for(int copy=0;copy<width;++copy){double offset=copy-(width-1)*.5;double ax=6+fractions[lo],ay=25.72+offset,bx=56+fractions[hi],by=(slope?22.52:28.92)+offset;
    if(reverse){double t=ax;ax=bx;bx=t;t=ay;ay=by;by=t;}
    if(axis){double t=ax;ax=ay;ay=t;t=bx;bx=by;by=t;}
    int px,py;if(!wide_terminal_pixel(ax,ay,bx,by,!axis,&px,&py))continue;
    assert(px!=previous_x || py!=previous_y);previous_x=px;previous_y=py;
    double qx=floor(bx*256+.5)/256,qy=floor(by*256+.5)/256;
    double distance=fabs(qx-px-.5)+fabs(qy-py-.5);
    assert(distance<.5 || (distance==.5 && qy>py+.5 && (qx!=px+.5 || axis)));
    double a=axis?ay:ax,b=axis?by:bx,m=axis?py+.5:px+.5;
    assert(m>=fmin(a,b) && m<fmax(a,b));++count;
   }
  }
  printf("width%d supplements %u\n",width,count);
  assert(count==(unsigned)(40*width));
 }
 int x,y;assert(!wide_terminal_pixel(1,1,1,1,true,&x,&y));
 assert(!wide_terminal_pixel(6.4,6.4,56.6,56.6,true,&x,&y));
 /* A high endpoint exactly on a major center is excluded. */
 assert(!wide_terminal_pixel(6.4,25.72,56.5,25.72,true,&x,&y));
 /* Tips remain excluded. */
 assert(!wide_terminal_pixel(56.6,26,6.5,26,true,&x,&y));
 /* Nonaxis upper slanted edges are inside. Lower edges remain native. */
 for(int direction=0;direction<2;++direction)for(int slope=0;slope<2;++slope){
  double bx=direction?20.25:44.75,ax=direction?44.75:20.25;
  assert(wide_terminal_pixel(ax,slope?30.25:18.25,bx,24.75,true,&x,&y));
  assert(x==(direction?20:44) && y==24);
  assert(!wide_terminal_pixel(ax,slope?30.25:18.25,bx,24.25,true,&x,&y));
 }
 /* X-major tips, axes, and exact major ties stay out. */
 assert(!wide_terminal_pixel(20,10,24.5,25,true,&x,&y));
 assert(!wide_terminal_pixel(40,30,24,24.5,true,&x,&y));
 /* Check the actual helper against all native-probed nonaxis pairs. Every
  * accepted candidate must omit forward coverage and gain reversed coverage.
  * Exact tips never enter the boundary extension. */
 const int edges[8][2]={{0,128},{64,64},{128,0},{64,-64},{0,-128},{-64,-64},{-128,0},{-64,64}};
 const int octants[8][2]={{3072,1024},{1024,3072},{-1024,3072},{-3072,1024},{-3072,-1024},{-1024,-3072},{1024,-3072},{3072,-1024}};
 unsigned checked=0;
 for(int e=0;e<8;++e)for(int o=0;o<8;++o)for(int shift=-1;shift<=1;++shift){
  int bx=edges[e][0],by=edges[e][1];
  if(by)by+=(by>0?1:-1)*shift;else bx+=(bx>0?1:-1)*shift;
  double ax=32.5+octants[o][0]/256.0,ay=32.5+octants[o][1]/256.0;
  double endx=32.5+bx/256.0,endy=32.5+by/256.0;
  bool accepted=wide_terminal_pixel(ax,ay,endx,endy,fabs(endx-ax)>fabs(endy-ay),&x,&y);
  if(accepted){
   int cx=x*256+128,cy=y*256+128;
   assert(!glm_probe_symbolic_diamond(lround(ax*256),lround(ay*256),lround(endx*256),lround(endy*256),cx,cy));
   assert(glm_probe_symbolic_diamond(lround(endx*256),lround(endy*256),lround(ax*256),lround(ay*256),cx,cy));
   if(!shift && (e%2)==0 && fabs(endx-ax)>fabs(endy-ay))assert(x!=32 || y!=32);
  }
  ++checked;
 }
 printf("Actual helper checked %u native-probed nonaxis pairs\n",checked);
 const double slopes[4][2]={{8,8+1.0/256},{4,12},{1.0/256,12},{4,24}};
 const int translations[3][2]={{30,28},{32,32},{36,35}};
 unsigned tips=0;
 for(int slope=0;slope<4;++slope)for(int tr=0;tr<3;++tr)
  for(int sx=-1;sx<=1;sx+=2)for(int sy=-1;sy<=1;sy+=2)for(int shift=-1;shift<=1;++shift){
   double bx=translations[tr][0]+.5,by=translations[tr][1]+1+shift/256.0;
   double ax=bx+sx*slopes[slope][0],ay=by+sy*slopes[slope][1];
   if(wide_terminal_pixel(ax,ay,bx,by,false,&x,&y)){
    int cx=x*256+128,cy=y*256+128;
    assert(!glm_probe_symbolic_diamond(lround(ax*256),lround(ay*256),lround(bx*256),lround(by*256),cx,cy));
    assert(glm_probe_symbolic_diamond(lround(bx*256),lround(by*256),lround(ax*256),lround(ay*256),cx,cy));
   }
   if(!shift && sy<0){assert(wide_terminal_pixel(ax,ay,bx,by,false,&x,&y));assert(x==translations[tr][0]&&y==translations[tr][1]);}
   ++tips;
  }
 printf("Actual helper checked %u validated y-major upper-tip pairs\n",tips);
 /* Bounds are the intersection of nonnegative framebuffer pixels, the
  * snapped major interval, and the unchanged application scissor. */
 const int app_boxes[5][4]={{0,0,8,8},{-4,-3,10,9},{4,3,2,4},{4,3,0,4},{INT_MAX,INT_MAX,INT_MAX,INT_MAX}};
 unsigned intervals=0;
 for(int axis=0;axis<2;++axis)for(int ia=-12;ia<=40;++ia)for(int ib=-12;ib<=40;++ib)
  for(int app=-1;app<5;++app){
   int original[4]={0};if(app>=0)for(int c=0;c<4;++c)original[c]=app_boxes[app][c];
   int saved[4];for(int c=0;c<4;++c)saved[c]=original[c];
   int box[4];double a=ia/4.0,b=ib/4.0;
   assert(wide_major_scissor(a,b,axis,app>=0,original,box));
   for(int c=0;c<4;++c)assert(saved[c]==original[c]);
   for(int px=0;px<12;++px)for(int py=0;py<12;++py){
    double major=(axis?py:px)+.5;
    bool expected=major>=fmin(a,b)&&major<fmax(a,b);
    if(app>=0)expected=expected&&original[2]>0&&original[3]>0&&px>=original[0]&&py>=original[1]&&
     (int64_t)px<(int64_t)original[0]+original[2]&&(int64_t)py<(int64_t)original[1]+original[3];
    bool inside=px>=box[0]&&py>=box[1]&&(int64_t)px<(int64_t)box[0]+box[2]&&(int64_t)py<(int64_t)box[1]+box[3];
    assert(inside==expected);
   }
   ++intervals;
  }
 int no_app[4]={0},box[4];
 assert(!wide_major_scissor(INFINITY,1,0,false,no_app,box));
 assert(wide_major_scissor(-1e300,1e300,0,false,no_app,box));
 assert(box[0]==0&&box[2]==INT_MAX);
 printf("Actual helper checked %u major/scissor intersections without input mutation\n",intervals);
 assert(!wide_terminal_pixel(6.4,25.72,56.6,25.72,false,&x,&y));
 /* Finite extreme viewport positions must never overflow integer casts. */
 assert(!wide_terminal_pixel(2147483700.6,25.72,2147483650.4,25.72,true,&x,&y));
 assert(!wide_terminal_pixel(-2147483650.4,25.72,-2147483700.6,25.72,true,&x,&y));
 assert(wide_terminal_viewport(-.8f,0,64,(-.8f*.5+0.5)*64,0));
 /* Exact half-step and its adjacent input floats need conservative fallback. */
 float boundary=(float)((6.5/256/64-.5)*2);
 double boundary_window=(boundary*.5+.5)*64;
 assert(!wide_terminal_viewport(boundary,0,64,boundary_window,0));
 assert(!wide_terminal_viewport(0,2147483640,64,2147483672.0,0));
 assert(wide_terminal_viewport(-.5f,7,48,19,0));
 assert(wide_terminal_viewport(0,0,1,8,7.5));
 assert(!wide_terminal_viewport(0,0,1,8+0.5/256,7.5));
 assert(!wide_terminal_viewport(0,0,1,-7-0.5/256,-7.5));
 puts("CPU terminal coverage, shifted viewport intervals and boundary guards passed");
}
