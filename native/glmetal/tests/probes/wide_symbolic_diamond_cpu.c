#include "wide_symbolic_diamond.h"
#include <assert.h>
#include <stdio.h>
static const int edge[8][2]={{0,128},{64,64},{128,0},{64,-64},{0,-128},{-64,-64},{-128,0},{-64,64}};
static const int octant[8][2]={{3072,1024},{1024,3072},{-1024,3072},{-3072,1024},{-3072,-1024},{-1024,-3072},{1024,-3072},{3072,-1024}};
int main(void)
{
 unsigned count=0;
 for(int e=0;e<8;++e)for(int o=0;o<8;++o)for(int shift=-1;shift<=1;++shift){
  int ax=octant[o][0],ay=octant[o][1],bx=edge[e][0],by=edge[e][1],axis=by!=0;
  if(axis)by+=(by>0?1:-1)*shift;else bx+=(bx>0?1:-1)*shift;
  bool forward=glm_probe_symbolic_diamond(ax,ay,bx,by,0,0),reverse=glm_probe_symbolic_diamond(bx,by,ax,ay,0,0);
  bool inside=(bx<0?-bx:bx)+(by<0?-by:by)<128 || ((bx<0?-bx:bx)+(by<0?-by:by)==128&&by>0);
  if(inside){assert(!forward);assert(reverse);}else assert(forward==reverse);
  ++count;
 }
 printf("Formal symbolic CPU proof passed: %u edge/octant/shift pairs\n",count);
}
