#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *const names[]={"smooth_fan","smooth_rotation0","smooth_rotation1","smooth_rotation2",
    "noperspective_fan","noperspective_rotation0","noperspective_rotation1","noperspective_rotation2",
    "rgba32f_z_clip","rgba32f_z_clamp","rgba32f_z_clamp_depth","rgba32f_z_clamp_fragcoord","rgba32f_z_clamp_rawdepth","rgba32f_z0_depth",
    "rgba8_z_clip","rgba8_z_clamp","rgba8_z_clamp_depth","rgba8_z_clamp_fragcoord","rgba8_z_clamp_rawdepth","rgba8_z0_depth",
    "rgba32f_z_vary_inside","rgba32f_z_constant_inside","rgba32f_z_constant_outside_clamp","rgba32f_z_reversed_clamp","rgba32f_z_near_only_clamp","rgba32f_z_far_only_clamp"};
static const float vertices[][6]={
    {-.9f,-.9f,1,0,0,1},{.9f,-.85f,0,1,0,.8f},{.85f,.9f,0,0,1,.6f},{-.85f,.85f,1,1,0,.4f}
};
static const int pixels[][2]={
    {35,55},{33,51},{34,50},{42,46},{27,44},{23,38},{35,34},{19,33},{40,30},{36,28},
    {23,17},{21,15},{24,15},{38,15},{20,13},{25,12},{30,11},{43,9},{16,8},{37,7},
    {31,57},{27,48},{43,47},{23,39},{32,35},{28,23},{44,16},{18,11},{32,32}
};
GLC_CASE_VARIANTS(driver_triangle_interpolation,names,.profile=GLC_CORE)
{
    char vertex[768],fragment[512];
    int extended=glc_variant>=8;
    int mode=extended?(glc_variant-8)%6:0;
    int z_diagnostic=glc_variant>=20;
    int rgba8=glc_variant>=14&&glc_variant<20;
    int clamp=z_diagnostic||(extended&&mode>=1&&mode<=4);
    int depth=extended&&!z_diagnostic&&mode>=2;
    const char *qualifier=!extended&&glc_variant>=4?"noperspective":"smooth";
    const char *z=extended&&mode!=5?"1.5-color.r*3.0":"0.0";
    const char *z_patterns[]={"0.75-1.5*color.r","0.75","1.5","-1.5+3.0*color.r","-1.5+1.5*color.r","1.5*color.r"};
    if(z_diagnostic)z=z_patterns[glc_variant-20];
    snprintf(vertex,sizeof vertex,"#version 410 core\nlayout(location=0)in vec2 position;layout(location=1)in vec4 color;%s out vec4 v;noperspective out float rawDepth;void main(){gl_Position=vec4(position,%s,1);v=color;rawDepth=(gl_Position.z+gl_Position.w)*0.5/gl_Position.w;}",qualifier,z);
    const char *depth_write=extended&&!z_diagnostic&&mode==3?"gl_FragDepth=clamp(gl_FragCoord.z,0.0,1.0);":extended&&!z_diagnostic&&mode==4?"gl_FragDepth=clamp(rawDepth,0.0,1.0);":"";
    snprintf(fragment,sizeof fragment,"#version 410 core\n%s in vec4 v;noperspective in float rawDepth;out vec4 result;void main(){result=v;%s}",qualifier,depth_write);
    GLint draw,read,viewport[4];glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw);glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);glGetIntegerv(GL_VIEWPORT,viewport);
    GLuint program=glc_program(vertex,fragment,NULL),vao,buffer,texture,fbo,depthbuffer;
    glGenVertexArrays(1,&vao);glBindVertexArray(vao);glGenBuffers(1,&buffer);glBindBuffer(GL_ARRAY_BUFFER,buffer);
    glBufferData(GL_ARRAY_BUFFER,sizeof vertices,vertices,GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,sizeof vertices[0],NULL);glVertexAttribPointer(1,4,GL_FLOAT,GL_FALSE,sizeof vertices[0],(void*)8);
    glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);glTexImage2D(GL_TEXTURE_2D,0,rgba8?GL_RGBA8:GL_RGBA32F,64,64,0,GL_RGBA,GL_FLOAT,NULL);
    glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    glGenRenderbuffers(1,&depthbuffer);glBindRenderbuffer(GL_RENDERBUFFER,depthbuffer);glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT32F,64,64);glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depthbuffer);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)glc_fail("Interpolation framebuffer incomplete");
    glViewport(0,0,64,64);if(depth)glEnable(GL_DEPTH_TEST);else glDisable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);glDisable(GL_BLEND);if(clamp)glEnable(GL_DEPTH_CLAMP);else glDisable(GL_DEPTH_CLAMP);glUseProgram(program);glClearColor(-1,-1,-1,-1);glClearDepth(1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    int order=extended?0:glc_variant%4;
    if(!order)glDrawArrays(GL_TRIANGLE_FAN,0,4);
    else {
        GLuint indices[6],base[]={0,1,2,0,2,3};
        for(int t=0;t<2;++t)for(int v=0;v<3;++v)indices[t*3+v]=base[t*3+(v+order-1)%3];
        GLuint elements;glGenBuffers(1,&elements);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,elements);glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof indices,indices,GL_STATIC_DRAW);glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,NULL);glDeleteBuffers(1,&elements);
    }
    float output[64*64][4];glReadPixels(0,0,64,64,GL_RGBA,GL_FLOAT,output);
    for(unsigned i=0;i<sizeof pixels/sizeof pixels[0];++i){int x=pixels[i][0],y=pixels[i][1];float *value=output[y*64+x];uint32_t bits[4];memcpy(bits,value,sizeof bits);fprintf(stderr,"interpolation %s pixel=%d,%d bits=%08x,%08x,%08x,%08x value=%a,%a,%a,%a\n",names[glc_variant],x,y,bits[0],bits[1],bits[2],bits[3],value[0],value[1],value[2],value[3]);}
    GLenum error=glGetError();if(error)glc_fail("Interpolation diagnostic error %x",error);
    glUseProgram(0);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,draw);
    glBlitFramebuffer(0,0,64,64,0,0,64,64,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,read);glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
    glDeleteFramebuffers(1,&fbo);glDeleteRenderbuffers(1,&depthbuffer);glDeleteTextures(1,&texture);glDeleteBuffers(1,&buffer);glDeleteVertexArrays(1,&vao);glDeleteProgram(program);
    usleep(50000);
}
