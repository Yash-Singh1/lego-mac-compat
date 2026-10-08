#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdio.h>

/* Every face has the same mip-constant colour. Sampling reveals the actual
   mip interpolation independently of face selection and depth comparison. */
static const char *const cube_lod_names[] = {
    "clamp_off", "repeat_off", "clamp_on", "repeat_on",
    "clamp_nearest_off", "repeat_nearest_off", "clamp_nearest_on", "repeat_nearest_on"
};
GLC_CASE_VARIANTS(core_cube_lod_probe, cube_lod_names, .profile = GLC_CORE, .tolerance = 2)
{
    const char *vertex="#version 410 core\nout vec2 uv;void main(){"
        "vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);uv=p;gl_Position=vec4(p*2-1,0,1);}";
    const char *fragment="#version 410 core\nuniform samplerCube t;in vec2 uv;out vec4 frag;"
        "void main(){vec3 d=normalize(vec3(uv*2-1,.6));vec3 dx=dFdx(d)*4,dy=dFdy(d)*4;"
        "vec3 a=abs(d);vec3 p,px,py;"
        "if(a.x>=a.y&&a.x>=a.z){p=vec3(d.z,d.y,d.x);px=vec3(dx.z,dx.y,dx.x);py=vec3(dy.z,dy.y,dy.x);}"
        "else if(a.y>=a.z){p=vec3(d.x,d.z,d.y);px=vec3(dx.x,dx.z,dx.y);py=vec3(dy.x,dy.z,dy.y);}"
        "else{p=d;px=dx;py=dy;}"
        "vec2 x=(px.xy-p.xy*px.z/p.z)/p.z*.5,y=(py.xy-p.xy*py.z/p.z)/p.z*.5;"
        "float l=log2(max(length(x),length(y))*16);"
        "frag=vec4(textureGrad(t,d,dx,dy).r,texture(t,d,2.0).r,clamp(l,0,4)/8,(textureQueryLod(t,d).y+2)/8);}";
    GLuint program=glc_program(vertex,fragment,NULL);
    glUseProgram(program);
    GLuint vao,cube,colour,fbo;
    glGenVertexArrays(1,&vao);glBindVertexArray(vao);
    glGenTextures(1,&cube);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_CUBE_MAP,cube);
    for(int level=0,size=16;level<5;++level,size>>=1){
        float values[256*4];for(int i=0;i<size*size;++i){for(int c=0;c<3;++c)values[i*4+c]=(float)level/8;values[i*4+3]=1;}
        for(int face=0;face<6;++face)glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,level,GL_RGBA32F,size,size,0,GL_RGBA,GL_FLOAT,values);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MIN_FILTER,
                    glc_variant < 4 ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAG_FILTER,glc_variant < 4 ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAX_LEVEL,4);
    GLenum wrap=(glc_variant & 1)?GL_REPEAT:GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_S,wrap);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_T,wrap);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_R,wrap);
    if(glc_variant & 2)glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    else glDisable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    glUniform1i(glGetUniformLocation(program,"t"),0);
    GLint target;glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&target);
    glGenTextures(1,&colour);glBindTexture(GL_TEXTURE_2D,colour);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,64,64,0,GL_RGBA,GL_FLOAT,NULL);
    glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,colour,0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)glc_fail("cube LOD float framebuffer incomplete");
    glViewport(0,0,64,64);glDrawArrays(GL_TRIANGLES,0,3);
    float pixels[64*64*4];glReadPixels(0,0,64,64,GL_RGBA,GL_FLOAT,pixels);
    for(int y=0;y<64;++y)for(int x=0;x<64;++x){float *p=&pixels[(y*64+x)*4];
        printf("cube-lod,%s,%d,%d,%.9g,%.9g,%.9g,%.9g\n",cube_lod_names[glc_variant],x,y,p[0]*8,p[1]*8,p[2]*8,p[3]*8);}
    glBindFramebuffer(GL_READ_FRAMEBUFFER,fbo);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)target);
    glBlitFramebuffer(0,0,64,64,0,0,glc_width,glc_height,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)target);glViewport(0,0,glc_width,glc_height);
    glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_TRUE);
    glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glDisable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&colour);glDeleteTextures(1,&cube);
    glDeleteVertexArrays(1,&vao);glDeleteProgram(program);
}
