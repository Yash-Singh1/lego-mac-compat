#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <stdio.h>
int main(void){ CGLRendererInfoObj ri; GLint nr; CGLQueryRendererInfo(0xffffffff,&ri,&nr); printf("nrend(all)=%d\n",nr);
 for(int r=0;r<nr;r++) for(int p=0;p<200;p++){GLint v=0; CGLError e=CGLDescribeRenderer(ri,r,(CGLRendererProperty)p,&v); if(!e) printf("r%d prop %d = %d (0x%x)\n",r,p,v,v);}
 CGLRendererInfoObj r1; CGLQueryRendererInfo(1,&r1,&nr); printf("nrend(mask1)=%d\n",nr);
 CGLPixelFormatAttribute a[]={kCGLPFAAccelerated,kCGLPFADoubleBuffer,kCGLPFADepthSize,24,kCGLPFAStencilSize,8,0}; CGLPixelFormatObj pf; GLint n; CGLChoosePixelFormat(a,&pf,&n); printf("npix=%d\n",n);
 for(int s=0;s<n;s++) for(int p=0;p<130;p++){GLint v=0; if(!CGLDescribePixelFormat(pf,s,(CGLPixelFormatAttribute)p,&v)) printf("pf s%d attr %d = %d\n",s,p,v);}
 return 0;}
