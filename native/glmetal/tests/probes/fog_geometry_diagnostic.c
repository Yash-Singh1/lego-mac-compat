#include "glc_gl_legacy.h"
#include "glcompare.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Standalone diagnostic only. Build with the normal glcompare -O2 flags. */
struct fog_vertex { double p[4], color[3], fog; };
static const char *const fog_geometry_names[] = {
    "quad", "current", "alternate", "current_rotate1", "current_rotate2", "preclip",
    "quad_inputs", "current_inputs", "alternate_inputs", "current_rotate1_inputs", "current_rotate2_inputs", "preclip_inputs",
    "preclip_float_inputs", "preclip_reverse_inputs", "preclip_float_reverse_inputs", "preclip_x_inputs", "preclip_y_inputs", "preclip_depth_inputs",
    "snap256_round_inputs", "snap128_round_inputs", "snap512_round_inputs", "snap256_floor_inputs", "snap128_floor_inputs", "snap512_floor_inputs", "snap256_round_color"
};
static double fog_plane(const struct fog_vertex *v, int plane)
{ return v->p[3] + ((plane&1)?-1:1)*v->p[plane/2]; }
static struct fog_vertex fog_lerp(struct fog_vertex a, struct fog_vertex b, double t)
{
    struct fog_vertex r;
    for(int i=0;i<4;++i)r.p[i]=a.p[i]+t*(b.p[i]-a.p[i]);
    for(int i=0;i<3;++i)r.color[i]=a.color[i]+t*(b.color[i]-a.color[i]);
    r.fog=a.fog+t*(b.fog-a.fog);return r;
}
static int fog_preclip(struct fog_vertex *vertices, int profile)
{
    struct fog_vertex a[12],b[12];memcpy(a,vertices,3*sizeof *a);int n=3;
    for(int step=0;step<6 && n; ++step){int plane=(profile==2 || profile==3)?5-step:step;
        if(profile==4 && plane/2!=0)continue;
        if(profile==5 && plane/2!=1)continue;
        if(profile==6 && plane/2!=2)continue;
        int m=0;
        for(int i=0;i<n;++i){struct fog_vertex x=a[i],y=a[(i+1)%n];double dx=fog_plane(&x,plane),dy=fog_plane(&y,plane);
            if(dx>=0)b[m++]=x;
            if((dx>=0)!=(dy>=0)){
                if(profile==1 || profile==3){
                    float fx=(float)x.p[3]+((plane&1)?-1.0f:1.0f)*(float)x.p[plane/2];
                    float fy=(float)y.p[3]+((plane&1)?-1.0f:1.0f)*(float)y.p[plane/2];
                    float t=fx/(fx-fy);struct fog_vertex r;
                    for(int k=0;k<4;++k){volatile float delta=(float)y.p[k]-(float)x.p[k];volatile float product=t*delta;r.p[k]=(float)x.p[k]+product;}
                    for(int k=0;k<3;++k){volatile float delta=(float)y.color[k]-(float)x.color[k];volatile float product=t*delta;r.color[k]=(float)x.color[k]+product;}
                    volatile float delta=(float)y.fog-(float)x.fog;volatile float product=t*delta;r.fog=(float)x.fog+product;b[m++]=r;
                }else b[m++]=fog_lerp(x,y,dx/(dx-dy));
            }
        }memcpy(a,b,m*sizeof *a);n=m;
    }memcpy(vertices,a,n*sizeof *a);return n;
}
static void fog_emit(struct fog_vertex v)
{
    glColor3f((float)v.color[0],(float)v.color[1],(float)v.color[2]);glFogCoordf((float)v.fog);
    glVertex4f((float)v.p[0],(float)v.p[1],(float)v.p[2],(float)v.p[3]);
}
static uint32_t fog_word(float f){uint32_t u;memcpy(&u,&f,4);return u;}
GLC_CASE_VARIANTS(legacy_fog_geometry_diagnostic, fog_geometry_names, .profile=GLC_LEGACY)
{
    int snap=glc_variant==24?0:glc_variant>=18?glc_variant-18:-1;
    int profile=glc_variant>=12 && glc_variant<18?glc_variant-11:0;
    int mode=snap>=0?0:glc_variant>=12?5:glc_variant%6,inputs=glc_variant>=6 && glc_variant!=24;GLint original;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&original);
    GLuint tex,fbo,program=0;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F_ARB,64,64,0,GL_RGBA,GL_FLOAT,NULL);
    glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)glc_fail("Fog geometry float target incomplete");
    glViewport(0,0,64,64);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);glEnable(GL_FOG);
    const float color[]={.6f,.6f,.7f,1};glFogfv(GL_FOG_COLOR,color);glFogi(GL_FOG_MODE,GL_EXP);glFogf(GL_FOG_DENSITY,.25f);
    glMatrixMode(GL_PROJECTION);glLoadIdentity();glFrustum(-.5,.5,-.5,.5,1,20);float matrix[16];glGetFloatv(GL_PROJECTION_MATRIX,matrix);
    glMatrixMode(GL_MODELVIEW);glLoadIdentity();
    struct fog_vertex v[4]={{{-1.5,-1,-1.2f,1},{.9f,.3f,.1f},1.2f},{{1.5,-1,-1.2f,1},{.9f,.3f,.1f},1.2f},
        {{1.5,1,-15,1},{.1f,.8f,.2f},15},{{-1.5,1,-15,1},{.1f,.8f,.2f},15}};
    if(mode==5){for(int i=0;i<4;++i){double p[4];for(int r=0;r<4;++r){p[r]=0;for(int c=0;c<4;++c)p[r]+=(double)matrix[4*c+r]*v[i].p[c];}memcpy(v[i].p,p,sizeof p);}
        glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glFogi(GL_FOG_COORDINATE_SOURCE,GL_FOG_COORDINATE);}
    char snap_vertex[768];
    if(snap>=0){const int scales[]={256,128,512};int scale=scales[snap%3];
        snprintf(snap_vertex,sizeof snap_vertex,
            "#version 120\nvoid main(){vec4 p=ftransform();vec2 win=(p.xy/p.w*0.5+0.5)*64.0;win=floor(win*%d.0%s)/%d.0;p.xy=(win/64.0*2.0-1.0)*p.w;gl_Position=p;gl_FrontColor=gl_Color;gl_BackColor=gl_Color;gl_FogFragCoord=-(gl_ModelViewMatrix*gl_Vertex).z;}",scale,snap<3?"+0.5":"",scale);
    }
    if(inputs || snap>=0)program=glc_program(snap>=0?snap_vertex:mode==5?
        "#version 120\nvoid main(){gl_Position=ftransform();gl_FrontColor=gl_Color;gl_BackColor=gl_Color;gl_FogFragCoord=gl_FogCoord;}" :
        "#version 120\nvoid main(){gl_Position=ftransform();gl_FrontColor=gl_Color;gl_BackColor=gl_Color;gl_FogFragCoord=-(gl_ModelViewMatrix*gl_Vertex).z;}",
        inputs?"#version 120\nvoid main(){float z=abs(gl_FogFragCoord);gl_FragColor=vec4(gl_Color.g,z,exp(-gl_Fog.density*z),1);}" :
        "#version 120\nvoid main(){float f=exp(-gl_Fog.density*abs(gl_FogFragCoord));gl_FragColor=vec4(mix(gl_Fog.color.rgb,gl_Color.rgb,f),gl_Color.a);}",NULL);
    else glUseProgram(0);
    if(mode==0){glBegin(GL_QUADS);for(int i=0;i<4;++i)fog_emit(v[i]);glEnd();}
    else {const int current[6]={0,1,3,1,2,3},alternate[6]={0,1,2,0,2,3};const int *ix=mode==2?alternate:current;
        glBegin(GL_TRIANGLES);for(int t=0;t<2;++t){if(mode==5){struct fog_vertex clipped[12]={v[ix[t*3]],v[ix[t*3+1]],v[ix[t*3+2]]};int n=fog_preclip(clipped,profile);
                for(int k=1;k+1<n;++k){fog_emit(clipped[0]);fog_emit(clipped[k]);fog_emit(clipped[k+1]);}}
            else for(int k=0;k<3;++k)fog_emit(v[ix[t*3+(k+(mode==3?1:mode==4?2:0))%3]]);}glEnd();}
    float pixels[64*64*4];glReadPixels(0,0,64,64,GL_RGBA,GL_FLOAT,pixels);
    for(int x=0;x<64;++x){float *p=pixels+(17*64+x)*4;if(p[3])fprintf(stderr,"FogGeometry %s (%d,17) %.9g %.9g %.9g bits=%08x,%08x,%08x G255=%.12g\n",
        fog_geometry_names[glc_variant],x,p[0],p[1],p[2],fog_word(p[0]),fog_word(p[1]),fog_word(p[2]),(double)p[1]*255);}
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)original);glBlitFramebuffer(0,0,64,64,0,0,64,64,GL_COLOR_BUFFER_BIT,GL_NEAREST);glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)original);
    GLenum error=glGetError();if(error)glc_fail("Fog geometry diagnostic GL error %x",error);
    if(program)glDeleteProgram(program);glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&tex);
}
