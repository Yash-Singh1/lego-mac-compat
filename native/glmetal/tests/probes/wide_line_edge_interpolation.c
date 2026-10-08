#include "glc_gl_legacy.h"
#include "glcompare.h"
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

struct edge_case { float x0,y0,x1,y1,w0,w1,z0,z1; int reverse,user_clip; };
static const char *const edge_names[]={"horizontal","vertical","positive45","negative45","near45_x","near45_y","zero_length","perspective","reverse_perspective","viewport_clip","near_plane_clip","user_plane_clip"};
static const struct edge_case edges[]={
 {6.4f,25.72f,56.6f,25.72f,1,1,0,0,0,0},{25.72f,6.4f,25.72f,56.6f,1,1,0,0,1,0},
 {6.4f,6.4f,56.6f,56.6f,1,1,0,0,1,0},{6.4f,56.6f,56.6f,6.4f,1,1,0,0,0,0},
 {6.4f,6.4f,56.6f,56.4f,1,1,0,0,1,0},{6.4f,6.4f,56.4f,56.6f,1,1,0,0,0,0},
 {25.4f,25.72f,25.4f,25.72f,1,1,0,0,0,0},
 {6.4f,25.72f,56.6f,28.92f,1,4,0,0,0,0},{6.4f,25.72f,56.6f,28.92f,1,4,0,0,1,0},
 {-6.4f,25.72f,56.6f,28.92f,1,1,0,0,1,0},
 {6.4f,25.72f,56.6f,28.92f,1,1,-2,0,1,0},
 {6.4f,25.72f,56.6f,28.92f,1,1,0,0,1,1}};
static uint32_t edge_bits(float f){uint32_t u;memcpy(&u,&f,4);return u;}
GLC_CASE_VARIANTS(legacy_wide_line_edge_interpolation,edge_names,.profile=GLC_LEGACY)
{
    const char *reverse_option = getenv("GLM_LINE_EDGE_REVERSE_WIDTH3");
    int reverse_width3 = reverse_option && strcmp(reverse_option, "1") == 0;
    fprintf(stderr, "WideEdgeMode width3_reverse=%d\n", reverse_width3);
    const struct edge_case *c=&edges[glc_variant];GLint original;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&original);
    GLuint output,fbo,ramp;glGenTextures(1,&output);glBindTexture(GL_TEXTURE_2D,output);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F_ARB,64,64,0,GL_RGBA,GL_FLOAT,NULL);
    glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,output,0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)glc_fail("Wide edge float target incomplete");
    glGenTextures(1,&ramp);glBindTexture(GL_TEXTURE_2D,ramp);float texels[16*4];for(int i=0;i<16;++i){texels[i*4]=texels[i*4+1]=texels[i*4+2]=(float)i/15;texels[i*4+3]=1;}
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F_ARB,16,1,0,GL_RGBA,GL_FLOAT,texels);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_REPLACE);glViewport(0,0,64,64);glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();
    glUseProgram(0);glDisable(GL_MULTISAMPLE);glDisable(GL_LINE_SMOOTH);glDisable(GL_BLEND);glClearColor(0,0,0,0);
    if(c->user_clip){const GLdouble plane[]={1,0,0,0};glClipPlane(GL_CLIP_PLANE0,plane);glEnable(GL_CLIP_PLANE0);}
    float samples[3][64*64*4];
    for(int width=1;width<=3;width+=2)for(int field=0;field<3;++field){
        glClear(GL_COLOR_BUFFER_BIT);glLineWidth(width);glDisable(GL_FOG);glDisable(GL_TEXTURE_2D);
        if(field==1){const float black[]={0,0,0,1};glEnable(GL_FOG);glFogfv(GL_FOG_COLOR,black);glFogi(GL_FOG_MODE,GL_LINEAR);glFogi(GL_FOG_COORDINATE_SOURCE,GL_FOG_COORDINATE);glFogf(GL_FOG_START,0);glFogf(GL_FOG_END,10);}
        if(field==2)glEnable(GL_TEXTURE_2D);
        /* Reversing submission must not swap attributes between positions. */
        int reverse = c->reverse ^ (reverse_width3 && width == 3);
        glBegin(GL_LINES);for(int ordinal=0;ordinal<2;++ordinal){int e=reverse?1-ordinal:ordinal;float x=e?c->x1:c->x0,y=e?c->y1:c->y0,w=e?c->w1:c->w0,z=e?c->z1:c->z0;
            if(field==0)glColor4f(e?.9f:.1f,e?.2f:.8f,e?.7f:.3f,1);else glColor4f(1,1,1,1);
            glFogCoordf(e?9:1);glTexCoord2f(e?.85f:.15f,.5f);glVertex4f((x/32-1)*w,(y/32-1)*w,z*w,w);}
        glEnd();glReadPixels(0,0,64,64,GL_RGBA,GL_FLOAT,samples[field]);
        if(field==2){for(int y=0;y<64;++y)for(int x=0;x<64;++x){int at=(y*64+x)*4;if(!samples[0][at+3])continue;
                int major_x=fabsf(c->x1-c->x0)>=fabsf(c->y1-c->y0);float major=major_x?x+.5f:y+.5f,low=major_x?c->x0:c->y0,high=major_x?c->x1:c->y1;
                if(fabsf(major-low)>2 && fabsf(major-high)>2 && !c->user_clip && glc_variant!=10)continue;
                double dx=c->x1-c->x0,dy=c->y1-c->y0,den=dx*dx+dy*dy;
                double t_major=(high!=low)?(major-low)/(high-low):0,t_dot=den?((x+.5-c->x0)*dx+(y+.5-c->y0)*dy)/den:0;
                double t_perspective=(t_major/c->w1)/((1-t_major)/c->w0+t_major/c->w1);
                fprintf(stderr,"WideEdge %s width%d (%d,%d) color=%.9g,%.9g,%.9g fog=%.9g tex=%.9g bits=%08x,%08x,%08x tmajor=%.12g tdot=%.12g tpersp=%.12g\n",edge_names[glc_variant],width,x,y,samples[0][at],samples[0][at+1],samples[0][at+2],samples[1][at],samples[2][at],edge_bits(samples[0][at]),edge_bits(samples[1][at]),edge_bits(samples[2][at]),t_major,t_dot,t_perspective);
            }}
        const struct timespec pause={0,50000000};nanosleep(&pause,NULL);
    }
    glDisable(GL_CLIP_PLANE0);glDisable(GL_FOG);glDisable(GL_TEXTURE_2D);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)original);glBlitFramebuffer(0,0,64,64,0,0,64,64,GL_COLOR_BUFFER_BIT,GL_NEAREST);glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)original);
    GLenum error=glGetError();if(error)glc_fail("Wide edge interpolation GL error %x",error);glDeleteTextures(1,&ramp);glDeleteTextures(1,&output);glDeleteFramebuffers(1,&fbo);
}
