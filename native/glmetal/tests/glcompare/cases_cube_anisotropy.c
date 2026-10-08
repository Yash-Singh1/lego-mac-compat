#include "glc_gl_core.h"
#include "glcompare.h"

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

static const char *const cube_anisotropy_names[] = {
    "shadow_clamp_off", "shadow_clamp_on", "shadow_repeat_off", "shadow_repeat_on",
    "colour_clamp_off", "colour_clamp_on", "colour_repeat_off", "colour_repeat_on"
};

GLC_CASE_VARIANTS(core_cube_anisotropy, cube_anisotropy_names,
                  .profile = GLC_CORE, .tolerance = 2)
{
    int shadow = glc_variant < 4;
    const char *vertex="#version 410 core\nout vec2 uv;void main(){"
        "vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);uv=p;gl_Position=vec4(p*2-1,0,1);}";
    const char *fragments[] = {
        "#version 410 core\nuniform samplerCubeShadow t;in vec2 uv;out vec4 frag;"
        "void main(){vec3 d=normalize(vec3(uv.x*4-2,(uv.y-.5)*.3,.6));float r=uv.y*.7+.15;"
        "vec3 dx=dFdx(d)*2,dy=dFdy(d)*.5;"
        "frag=vec4(texture(t,vec4(d,r)),texture(t,vec4(d.zxy,r),1.25),"
        "textureGrad(t,vec4(-d.yzx,r),-dx.yzx,-dy.yzx),1);}",
        "#version 410 core\nuniform samplerCube t;in vec2 uv;out vec4 frag;"
        "void main(){vec3 d=normalize(vec3(uv.x*4-2,(uv.y-.5)*.3,.6));"
        "vec3 dx=dFdx(d)*2,dy=dFdy(d)*.5;"
        "frag=vec4(texture(t,d).r,texture(t,d.zxy,1.25).g,"
        "textureGrad(t,-d.yzx,-dx.yzx,-dy.yzx).b,1);}"
    };
    GLuint program=glc_program(vertex,fragments[shadow ? 0 : 1],NULL);
    glUseProgram(program);
    GLuint vao,texture,sampler;
    glGenVertexArrays(1,&vao);glBindVertexArray(vao);
    glGenTextures(1,&texture);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_CUBE_MAP,texture);
    for(int level=0,size=32;level<6;++level,size>>=1){
        for(int face=0;face<6;++face){
            float values[32*32*4];
            for(int y=0;y<size;++y)for(int x=0;x<size;++x){
                float u=size>1?(float)x/(size-1):.5f;
                float v=size>1?(float)y/(size-1):.5f;
                if(shadow)values[y*size+x]=u*.65f+v*.08f+face*.025f+level*.012f;
                else {
                    int i=(y*size+x)*4;
                    values[i]=u*.65f+face*.025f+level*.012f;
                    values[i+1]=v*.65f+face*.025f+level*.012f;
                    values[i+2]=(float)((x+3*y+face)&3)/4+level*.018f;
                    values[i+3]=1;
                }
            }
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,level,
                         shadow ? GL_DEPTH_COMPONENT32F : GL_RGBA32F,size,size,0,
                         shadow ? GL_DEPTH_COMPONENT : GL_RGBA,GL_FLOAT,values);
        }
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAX_LEVEL,5);
    glTexParameterf(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAX_ANISOTROPY_EXT,1);
    glGenSamplers(1,&sampler);
    glSamplerParameteri(sampler,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(sampler,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    GLenum wrap=(glc_variant&2)?GL_REPEAT:GL_CLAMP_TO_EDGE;
    glSamplerParameteri(sampler,GL_TEXTURE_WRAP_S,wrap);
    glSamplerParameteri(sampler,GL_TEXTURE_WRAP_T,wrap);
    glSamplerParameteri(sampler,GL_TEXTURE_WRAP_R,wrap);
    GLfloat limit=1;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT,&limit);
    if(limit<2)glc_fail("Cube anisotropy unavailable");
    glSamplerParameterf(sampler,GL_TEXTURE_MAX_ANISOTROPY_EXT,limit<8?limit:8);
    if(shadow){
        glSamplerParameteri(sampler,GL_TEXTURE_COMPARE_MODE,GL_COMPARE_REF_TO_TEXTURE);
        glSamplerParameteri(sampler,GL_TEXTURE_COMPARE_FUNC,GL_LEQUAL);
    }
    glBindSampler(0,sampler);
    if(glc_variant&1)glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    else glDisable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    glUniform1i(glGetUniformLocation(program,"t"),0);
    glDrawArrays(GL_TRIANGLES,0,3);
    glDisable(GL_TEXTURE_CUBE_MAP_SEAMLESS);glBindSampler(0,0);
    GLenum error=glGetError();
    if(error)glc_fail("Cube anisotropy GL error %x",error);
    glDeleteSamplers(1,&sampler);glDeleteTextures(1,&texture);
    glDeleteVertexArrays(1,&vao);glDeleteProgram(program);
}
