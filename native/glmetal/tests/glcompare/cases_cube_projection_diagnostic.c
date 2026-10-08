/* Diagnose selected-face texel ties without asserting a mathematical winner.
   FMA residual correction prevents the driver reciprocal lowering from hiding
   exact projected-ratio ties in the independent array sampling model. */
#include "glc_gl_core.h"
#include "glcompare.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t cube_projection_bits(float value)
{ uint32_t bits; memcpy(&bits,&value,4); return bits; }
static const char *const cube_projection_modes[] = {"cube_array", "reflection_equivalent"};
GLC_CASE_VARIANTS(core_cube_projection_diagnostic, cube_projection_modes, .profile = GLC_CORE)
{
    const int reflection=glc_variant;
    GLint original; glGetIntegerv(GL_FRAMEBUFFER_BINDING,&original);
    const char *vs_array="#version 410 core\nlayout(location=0) in vec2 p;out vec3 direction;out float layer;out vec2 uv;void main(){uv=p*.5+.5;vec2 c=fract(uv*vec2(2,1))*2-1;direction=vec3(0);layer=0;gl_Position=vec4(p,0,1);}";
    /* Array directions are computed per fragment exactly as in gl4_textures. */
    const char *vs_reflection="#version 410 core\nlayout(location=0) in vec3 p;layout(location=1) in vec3 n;out vec3 direction;out float layer;out vec2 uv;void main(){uv=vec2(0);gl_Position=vec4(p,1);direction=reflect(normalize(p),n);layer=0;}";
    char fragment[4096];
    snprintf(fragment,sizeof fragment,
        "#version 410 core\nin vec3 direction;in float layer;in vec2 uv;uniform int mode;uniform %s cube;uniform sampler2DArray faces;out vec4 color;"
        "vec3 project(vec3 d){vec3 a=abs(d);vec3 p;float face;"
        "if(a.x>=a.y&&a.x>=a.z){if(d.x>=0){p=vec3(-d.z,-d.y,d.x);face=0;}else{p=vec3(d.z,-d.y,-d.x);face=1;}}"
        "else if(a.y>=a.z){if(d.y>=0){p=vec3(d.x,d.z,d.y);face=2;}else{p=vec3(d.x,-d.z,-d.y);face=3;}}"
        "else{if(d.z>=0){p=vec3(d.x,-d.y,d.z);face=4;}else{p=vec3(-d.x,-d.y,-d.z);face=5;}}"
        "vec2 q=p.xy/p.z;vec2 residual=fma(-q,vec2(p.z),p.xy);q=fma(residual,vec2(1.0/p.z),q);return vec3(q*.5+.5,face);}"
        "void main(){%s vec3 projected=project(d);if(mode==2){color=vec4(d,projected.y);return;}"
        "color=mode==0?texture(cube,%s):texture(faces,vec3(projected.xy,projected.z+6*l));}",
        reflection?"samplerCube":"samplerCubeArray",
        reflection?"vec3 d=direction;float l=layer;":"vec2 c=fract(uv*vec2(2,1))*2-1;vec3 d=normalize(vec3(c,.6)*vec3(1,1,uv.y>.5?-1:1));float l=floor(uv.x*2);",
        reflection?"d":"vec4(d,l)");
    GLuint program=glc_program(reflection?vs_reflection:vs_array,fragment,NULL);
    GLuint textures[3],framebuffer,vao,buffer;
    glGenTextures(3,textures); glGenFramebuffers(1,&framebuffer); glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    glGenBuffers(1,&buffer); glBindBuffer(GL_ARRAY_BUFFER,buffer);
    float vertices[26*6]; int count;
    if(reflection){
        memset(vertices,0,sizeof vertices); vertices[2]=-.5f; vertices[5]=1;
        for(int i=0;i<=24;++i){float a=(float)i/24*6.2831853f;float x=.9f*cosf(a),y=.9f*sinf(a);int at=(i+1)*6;
            vertices[at]=x;vertices[at+1]=y;vertices[at+2]=-.5f;vertices[at+3]=x;vertices[at+4]=y;vertices[at+5]=.3f;}
        count=26; glBufferData(GL_ARRAY_BUFFER,sizeof vertices,vertices,GL_STATIC_DRAW);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,6*sizeof(float),NULL);
        glEnableVertexAttribArray(1);glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,6*sizeof(float),(void*)(3*sizeof(float)));
    }else{
        const float quad[]={-1,-1,1,-1,1,1,-1,1};count=4;glBufferData(GL_ARRAY_BUFFER,sizeof quad,quad,GL_STATIC_DRAW);
        glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,NULL);
    }
    glEnableVertexAttribArray(0);
    const GLenum cube_target=reflection?GL_TEXTURE_CUBE_MAP:GL_TEXTURE_CUBE_MAP_ARRAY;
    const int face_count=reflection?6:12;
    unsigned char pixels[12*8*8*4];
    for(int face=0;face<face_count;++face)for(int y=0;y<8;++y)for(int x=0;x<8;++x){int at=(face*64+y*8+x)*4;
        pixels[at]=reflection?(unsigned char)(face*40+x*8):(unsigned char)(x*255/7);
        pixels[at+1]=reflection?(unsigned char)(255-face*40):(unsigned char)(y*255/7);
        pixels[at+2]=reflection?(unsigned char)(y*30):(unsigned char)((face%4)*60+(face/4)*40);pixels[at+3]=255;}
    glActiveTexture(GL_TEXTURE0);glBindTexture(cube_target,textures[0]);
    if(reflection)for(int face=0;face<6;++face)glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,0,GL_RGBA8,8,8,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels+face*256);
    else glTexImage3D(cube_target,0,GL_RGBA8,8,8,face_count,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    glTexParameteri(cube_target,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(cube_target,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D_ARRAY,textures[1]);
    glTexImage3D(GL_TEXTURE_2D_ARRAY,0,GL_RGBA8,8,8,face_count,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_WRAP_S,GL_REPEAT);glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_WRAP_T,GL_REPEAT);
    glUniform1i(glGetUniformLocation(program,"cube"),0);glUniform1i(glGetUniformLocation(program,"faces"),1);
    glActiveTexture(GL_TEXTURE2);glBindTexture(GL_TEXTURE_2D,textures[2]);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,64,64,0,GL_RGBA,GL_FLOAT,NULL);
    glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,textures[2],0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)glc_fail("Cube projection target incomplete");
    float output[3][64*64*4];glViewport(0,0,64,64);glClearColor(0,0,0,0);
    for(int mode=0;mode<3;++mode){glUniform1i(glGetUniformLocation(program,"mode"),mode);glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_FAN,0,count);glReadPixels(0,0,64,64,GL_RGBA,GL_FLOAT,output[mode]);}
    int differences=0;
    for(int y=0;y<64;++y)for(int x=0;x<64;++x){int at=(y*64+x)*4;int differs=memcmp(output[0]+at,output[1]+at,4*sizeof(float))!=0;
        if(differs)++differences;
        const int selected=reflection?((x==20&&y==43)||(x==43&&y==43)||(x==21&&y==42)||(x==42&&y==42)):
            ((y==45&&(x==2||x==29||x==34||x==61))||(y==44&&(x==3||x==28||x==35||x==60)));
        if(selected|| (differs&&differences<=12))fprintf(stderr,"Cube projection %s (%d,%d) native=(%.9g,%.9g,%.9g) array=(%.9g,%.9g,%.9g) dirbits=%08x,%08x,%08x V=%08x\n",
            cube_projection_modes[glc_variant],x,y,output[0][at],output[0][at+1],output[0][at+2],output[1][at],output[1][at+1],output[1][at+2],
            cube_projection_bits(output[2][at]),cube_projection_bits(output[2][at+1]),cube_projection_bits(output[2][at+2]),cube_projection_bits(output[2][at+3]));}
    fprintf(stderr,"Cube projection %s native/array differences=%d\n",cube_projection_modes[glc_variant],differences);
    glUniform1i(glGetUniformLocation(program,"mode"),0);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLE_FAN,0,count);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)original);glBlitFramebuffer(0,0,64,64,0,0,64,64,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)original);GLenum error=glGetError();if(error)glc_fail("Cube projection GL error %x",error);
    glDeleteProgram(program);glDeleteTextures(3,textures);glDeleteFramebuffers(1,&framebuffer);glDeleteBuffers(1,&buffer);glDeleteVertexArrays(1,&vao);
}
