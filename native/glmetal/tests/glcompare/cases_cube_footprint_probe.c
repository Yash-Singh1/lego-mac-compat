#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdio.h>
#include <math.h>

/* Fit array footprints against native nonseamless cube sampling in one
   provider. Each candidate uses identical texels and sampler state. */
GLC_CASE(core_cube_footprint_probe, .profile=GLC_CORE, .tolerance=2)
{
    const char *vs="#version 410 core\nout vec2 uv;void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);uv=p;gl_Position=vec4(p*2-1,0,1);}";
    const char *fs="#version 410 core\nuniform samplerCube cube;uniform sampler2DArray faces;uniform int model;in vec2 uv;out vec4 frag;"
        "vec2 perm(vec2 v,int k){if((k&1)!=0)v=v.yx;if((k&2)!=0)v.x=-v.x;if((k&4)!=0)v.y=-v.y;return v;}"
        "float sampleFace(vec3 d,vec3 dx,vec3 dy,int channel){vec3 a=abs(d),p,px,py;float f;vec2 agx;vec2 agy;"
        "if(a.x>=a.y&&a.x>=a.z){if(d.x>=0){p=vec3(-d.z,-d.y,d.x);px=vec3(-dx.z,-dx.y,dx.x);py=vec3(-dy.z,-dy.y,dy.x);f=0;}"
        "else{p=vec3(d.z,-d.y,-d.x);px=vec3(dx.z,-dx.y,-dx.x);py=vec3(dy.z,-dy.y,-dy.x);f=1;}agx=dx.xz;agy=dy.xz;}"
        "else if(a.y>=a.z){if(d.y>=0){p=vec3(d.x,d.z,d.y);px=vec3(dx.x,dx.z,dx.y);py=vec3(dy.x,dy.z,dy.y);f=2;}"
        "else{p=vec3(d.x,-d.z,-d.y);px=vec3(dx.x,-dx.z,-dx.y);py=vec3(dy.x,-dy.z,-dy.y);f=3;}agx=dx.xy;agy=dy.xy;}"
        "else{if(d.z>=0){p=vec3(d.x,-d.y,d.z);px=vec3(dx.x,-dx.y,dx.z);py=vec3(dy.x,-dy.y,dy.z);f=4;}"
        "else{p=vec3(-d.x,-d.y,-d.z);px=vec3(-dx.x,-dx.y,-dx.z);py=vec3(-dy.x,-dy.y,-dy.z);f=5;}agx=dx.yz;agy=dy.yz;}"
        "vec2 x,y;int candidate=model%48,base=candidate/16,k=candidate%8;"
        "if(base==0){x=px.xy/p.z*.5;y=py.xy/p.z*.5;}"
        "else if(base==1){x=(px.xy-p.xy*px.z/p.z)/p.z*.5;y=(py.xy-p.xy*py.z/p.z)/p.z*.5;}"
        "else{x=agx/p.z*.5;y=agy/p.z*.5;}"
        "x=perm(x,k);y=perm(y,k);if((candidate%16)>=8){vec2 z=x;x=y;y=z;}"
        "vec2 center=p.xy/p.z*.5+.5;if(model>=96){vec2 edge=vec2(.5)/vec2(textureSize(faces,0).xy);center=clamp(center,edge,1-edge);}"
        "return textureGrad(faces,vec3(center,f),x,y)[channel];}"
        "void main(){vec3 d=normalize(vec3(uv.x*4-2,(uv.y-.5)*.3,.6));vec3 dx=dFdx(d),dy=dFdy(d);"
        "if(model<0)frag=vec4(texture(cube,d).r,texture(cube,d.zxy,1.25).g,textureGrad(cube,-d.yzx,-dx.yzx*2,-dy.yzx*.5).b,1);"
        "else frag=vec4(sampleFace(d,dx,dy,0),sampleFace(d.zxy,dx.zxy*exp2(1.25),dy.zxy*exp2(1.25),1),"
        "sampleFace(-d.yzx,-dx.yzx*2,-dy.yzx*.5,2),1);}";
    GLuint p=glc_program(vs,fs,NULL);glUseProgram(p);
    GLuint vao,cube,array,colour,fbo;glGenVertexArrays(1,&vao);glBindVertexArray(vao);
    glGenTextures(1,&cube);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_CUBE_MAP,cube);
    glGenTextures(1,&array);glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D_ARRAY,array);
    /* 128px retains positive anisotropic mip levels. */
    float values[128*128*6*4];
    for(int level=0,size=128;level<8;++level,size>>=1){
        for(int face=0;face<6;++face)for(int y=0;y<size;++y)for(int x=0;x<size;++x){
            int i=((face*size+y)*size+x)*4;float u=size>1?(float)x/(size-1):.5f,v=size>1?(float)y/(size-1):.5f;
            values[i]=u*.65f+face*.025f+level*.012f;
            values[i+1]=v*.65f+face*.025f+level*.012f;
            values[i+2]=(float)((x+3*y+face)&3)/4+level*.018f;values[i+3]=1;
        }
        glActiveTexture(GL_TEXTURE0);
        for(int face=0;face<6;++face)glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,level,GL_RGBA32F,size,size,0,GL_RGBA,GL_FLOAT,values+face*size*size*4);
        glActiveTexture(GL_TEXTURE1);glTexImage3D(GL_TEXTURE_2D_ARRAY,level,GL_RGBA32F,size,size,6,0,GL_RGBA,GL_FLOAT,values);
    }
    GLfloat limit=1;glGetFloatv(0x84FF,&limit);
    for(int unit=0;unit<2;++unit){glActiveTexture(GL_TEXTURE0+unit);GLenum target=unit?GL_TEXTURE_2D_ARRAY:GL_TEXTURE_CUBE_MAP;
        glTexParameteri(target,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);glTexParameteri(target,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(target,GL_TEXTURE_MAX_LEVEL,7);glTexParameteri(target,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(target,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glTexParameterf(target,0x84FE,limit<8?limit:8);
    }
    glDisable(GL_TEXTURE_CUBE_MAP_SEAMLESS);glUniform1i(glGetUniformLocation(p,"cube"),0);glUniform1i(glGetUniformLocation(p,"faces"),1);
    GLint original;glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&original);
    glGenTextures(1,&colour);glBindTexture(GL_TEXTURE_2D,colour);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,64,64,0,GL_RGBA,GL_FLOAT,NULL);
    glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,colour,0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)glc_fail("cube footprint framebuffer incomplete");
    glViewport(0,0,64,64);float reference[64*64*4],pixels[64*64*4];
    for(int model=-1;model<144;++model){glActiveTexture(GL_TEXTURE1);glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_WRAP_S,model<48?GL_CLAMP_TO_EDGE:GL_REPEAT);glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_WRAP_T,model<48?GL_CLAMP_TO_EDGE:GL_REPEAT);glUniform1i(glGetUniformLocation(p,"model"),model);glDrawArrays(GL_TRIANGLES,0,3);glReadPixels(0,0,64,64,GL_RGBA,GL_FLOAT,pixels);
        if(model<0){for(int i=0;i<64*64*4;++i)reference[i]=pixels[i];continue;}
        double sum[3]={0};float maximum[3]={0};int exact[3]={0};
        for(int i=0;i<64*64;++i)for(int c=0;c<3;++c){float delta=fabsf(pixels[i*4+c]-reference[i*4+c]);sum[c]+=delta;if(delta>maximum[c])maximum[c]=delta;if(delta<.000001f)++exact[c];}
        double faceSum[6][3]={{0}};int faceCount[6][3]={{0}};
        for(int y=0;y<64;++y)for(int x=0;x<64;++x){
            float d[3]={(x+.5f)/64*4-2,((y+.5f)/64-.5f)*.3f,.6f};
            for(int c=0;c<3;++c){float q[3];
                if(c==0){q[0]=d[0];q[1]=d[1];q[2]=d[2];}
                else if(c==1){q[0]=d[2];q[1]=d[0];q[2]=d[1];}
                else{q[0]=-d[1];q[1]=-d[2];q[2]=-d[0];}
                int axis=fabsf(q[0])>=fabsf(q[1])&&fabsf(q[0])>=fabsf(q[2])?0:fabsf(q[1])>=fabsf(q[2])?1:2;
                int face=axis*2+(q[axis]<0);int i=(y*64+x)*4+c;
                faceSum[face][c]+=fabsf(pixels[i]-reference[i]);++faceCount[face][c];
            }
        }
        for(int face=0;face<6;++face)printf("cube-footprint-face,%d,%d,%.9g,%.9g,%.9g,%d,%d,%d\n",model,face,
            faceCount[face][0]?faceSum[face][0]/faceCount[face][0]:0,
            faceCount[face][1]?faceSum[face][1]/faceCount[face][1]:0,
            faceCount[face][2]?faceSum[face][2]/faceCount[face][2]:0,
            faceCount[face][0],faceCount[face][1],faceCount[face][2]);
        printf("cube-footprint,%d,%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%d,%d\n",model,(model%48)/16,model%8,(model%16)>=8,sum[0]/4096,sum[1]/4096,sum[2]/4096,maximum[0],maximum[1],maximum[2],exact[0],exact[1],exact[2]);
    }
    glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)original);glViewport(0,0,glc_width,glc_height);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    GLenum error=glGetError();if(error)glc_fail("cube footprint GL error %x",error);
    glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&colour);glDeleteTextures(1,&array);glDeleteTextures(1,&cube);glDeleteVertexArrays(1,&vao);glDeleteProgram(p);glActiveTexture(GL_TEXTURE0);
}
