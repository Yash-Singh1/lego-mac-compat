#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdio.h>

/* Every face has the same mip-constant colour. Sampling reveals the actual
   mip interpolation independently of face selection and depth comparison. */
static const char *const cube_aniso_lod_names[] = {
    "clamp_off", "repeat_off", "clamp_on", "repeat_on",
    "clamp_aniso_off", "repeat_aniso_off", "clamp_aniso_on", "repeat_aniso_on"
};
GLC_CASE_VARIANTS(core_cube_aniso_lod_probe, cube_aniso_lod_names, .profile = GLC_CORE, .tolerance = 2)
{
    const char *vertex="#version 410 core\nout vec2 uv;void main(){"
        "vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);uv=p;gl_Position=vec4(p*2-1,0,1);}";
    const char *fragment="#version 410 core\nuniform samplerCube t;in vec2 uv;out vec4 frag;"
        "void main(){vec3 d=normalize(vec3(uv.x*4-2,(uv.y-.5)*.3,.6));"
        "vec3 dx=dFdx(d)*2,dy=dFdy(d)*.5;"
        "frag=vec4(textureGrad(t,-d.yzx,-dx.yzx,-dy.yzx).r,"
        "texture(t,d.zxy,1.25).r,texture(t,d).r,(textureQueryLod(t,d).y+2)/8);}";
    GLuint program=glc_program(vertex,fragment,NULL);
    glUseProgram(program);
    GLuint vao,cube,colour,fbo;
    glGenVertexArrays(1,&vao);glBindVertexArray(vao);
    glGenTextures(1,&cube);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_CUBE_MAP,cube);
    for(int level=0,size=128;level<8;++level,size>>=1){
        float values[16384*4];for(int i=0;i<size*size;++i){for(int c=0;c<3;++c)values[i*4+c]=(float)level/8;values[i*4+3]=1;}
        for(int face=0;face<6;++face)glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,level,GL_RGBA32F,size,size,0,GL_RGBA,GL_FLOAT,values);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MIN_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAX_LEVEL,7);
    GLfloat limit=1;glGetFloatv(0x84FF,&limit);
    glTexParameterf(GL_TEXTURE_CUBE_MAP,0x84FE,glc_variant < 4 ? 1 : (limit < 8 ? limit : 8));
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
        printf("cube-aniso-lod,%s,%d,%d,%.9g,%.9g,%.9g,%.9g\n",cube_aniso_lod_names[glc_variant],x,y,p[0]*8,p[1]*8,p[2]*8,p[3]*8);}
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
