#include <assert.h>
#include <stdio.h>
int main(void)
{
    struct glm_context ctx={.state.viewport={0,0,64,64}};
    for(int mode=0;mode<2;++mode){
        float m[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        if(!mode){m[10]=-11.f/9;m[11]=-1;m[14]=-20.f/9;m[15]=0;}
        else{ctx.state.clip_plane_enabled[0]=1;ctx.state.clip_planes[0][0]=1;ctx.state.clip_planes[0][1]=.3;ctx.state.clip_planes[0][3]=.1;}
        for(int r=0;r<4;++r){
            float y=-.7f+.43f*r;
            float p[2][4]={{-.8f,y,mode?0:-1.1f,1},{.75f*(mode?1:3),(y+.1f)*(mode?1:3),mode?0:-3,1}};
            const float *ends[2]={p[0],p[1]};double w[2][2];
            assert(wide_projected_interval(&ctx,ends,m,true,w));
            assert(ceil(floor(w[0][0]*256+.5)/256-.5)==(mode?(r==0?35:r==1?31:r==2?27:23):9));
            assert(!wide_projected_interval(&ctx,ends,m,false,w));
            p[0][3]=NAN;assert(!wide_projected_interval(&ctx,ends,m,true,w));p[0][3]=1;
            m[4]=.1f;assert(!wide_projected_interval(&ctx,ends,m,true,w));m[4]=0;
            if(!mode){float saved=p[0][2];p[0][2]=-.5;assert(!wide_projected_interval(&ctx,ends,m,true,w));p[0][2]=0;assert(!wide_projected_interval(&ctx,ends,m,true,w));p[0][2]=saved;}
            else{ctx.state.clip_plane_enabled[1]=1;assert(!wide_projected_interval(&ctx,ends,m,true,w));ctx.state.clip_plane_enabled[1]=0;}
        }
    }
    /* A finite cancelled dot result does not make its float intermediates
     * safe. These structurally supported matrices must retain native draws. */
    ctx.state.clip_plane_enabled[0]=0;
    float m[16]={1,0,0,0,0,1,0,0,0,0,0x1p19f,-1,0,0,0x1p19f,0};
    float p[2][4]={{-.8f*0x1p120f,-.7f*0x1p120f,-0x1p120f,0x1p120f},
                  {.75f*0x1p120f,.6f*0x1p120f,-0x1p120f,0x1p120f}};
    const float *ends[2]={p[0],p[1]};double w[2][2];
    assert(!wide_projected_interval(&ctx,ends,m,true,w));
    m[10]=FLT_MIN;m[14]=1;
    float tiny_product[2][4]={{-.1f,-.1f,-.5f,.125f},{.2f,.1f,-.5f,.125f}};
    ends[0]=tiny_product[0];ends[1]=tiny_product[1];
    assert(!wide_projected_interval(&ctx,ends,m,true,w));
    puts("Projected interval accepts eight bounded lines and rejects unsupported axes, transforms, clipping and eye crossings");
}
