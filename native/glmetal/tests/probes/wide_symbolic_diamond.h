#ifndef GLM_PROBE_SYMBOLIC_DIAMOND_H
#define GLM_PROBE_SYMBOLIC_DIAMOND_H
#include <stdint.h>
#include <stdbool.h>
/* Rational coefficients ordered by powers of a formal positive epsilon. */
struct glm_probe_poly { int64_t n[3], d; };
static int glm_probe_poly_cmp(struct glm_probe_poly a,struct glm_probe_poly b)
{
 for(int i=0;i<3;++i){int64_t difference=a.n[i]*b.d-b.n[i]*a.d;if(difference)return difference>0?1:-1;}return 0;
}
static struct glm_probe_poly glm_probe_poly_bound(int64_t boundary,int64_t start,int64_t e1,int64_t e2,int64_t delta)
{
 struct glm_probe_poly p={{boundary-start,-e1,-e2},delta};
 if(p.d<0){p.d=-p.d;for(int i=0;i<3;++i)p.n[i]=-p.n[i];}return p;
}
/* Coordinates are exact1/256 integers. Symbolic shift is(-epsilon^2,-epsilon).
 * Probe coordinates stay within64pixels, bounding all cross products. */
static bool glm_probe_symbolic_diamond(int64_t ax,int64_t ay,int64_t bx,int64_t by,int64_t cx,int64_t cy)
{
 struct glm_probe_poly zero={{0,0,0},1},one={{1,0,0},1},enter=zero,exit=one;
 int64_t start[2]={ax+ay-cx-cy,ax-ay-cx+cy},delta[2]={bx+by-ax-ay,bx-by-ax+ay};
 for(int i=0;i<2;++i){int64_t e1=i?1:-1,e2=-1;
  if(!delta[i]){struct glm_probe_poly s={{start[i],e1,e2},1},lo={{-128,0,0},1},hi={{128,0,0},1};
   if(glm_probe_poly_cmp(s,lo)<=0||glm_probe_poly_cmp(s,hi)>=0)return false;continue;}
  struct glm_probe_poly lo=glm_probe_poly_bound(-128,start[i],e1,e2,delta[i]),hi=glm_probe_poly_bound(128,start[i],e1,e2,delta[i]);
  if(glm_probe_poly_cmp(lo,hi)>0){struct glm_probe_poly t=lo;lo=hi;hi=t;}
  if(glm_probe_poly_cmp(lo,enter)>0)enter=lo;if(glm_probe_poly_cmp(hi,exit)<0)exit=hi;
 }
 return glm_probe_poly_cmp(enter,exit)<0&&glm_probe_poly_cmp(exit,one)<0&&glm_probe_poly_cmp(exit,zero)>0;
}
#endif
