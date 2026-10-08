#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *const names[]={"original","levels_one","patch_constant","patch_state","linearized_tes"};
GLC_CASE_VARIANTS(driver_tess_patch_precision,names,.profile=GLC_CORE)
{
    const char *vs="#version 410 core\nlayout(location=0)in vec2 position;layout(location=1)in vec4 color;out vec4 vc;void main(){gl_Position=vec4(position,0,1);vc=color;}";
    const char *tcs="#version 410 core\nlayout(vertices=3)out;in vec4 vc[];out vec4 tc[];patch out vec4 pc;patch out float shade;uniform int simple;void main(){gl_out[gl_InvocationID].gl_Position=gl_in[gl_InvocationID].gl_Position;tc[gl_InvocationID]=vc[gl_InvocationID];pc=(vc[0]+vc[1]+vc[2])/3.0;shade=float(gl_PrimitiveID)*0.5+0.25;gl_TessLevelOuter[0]=simple!=0?1.0:2.0+float(gl_PrimitiveID);gl_TessLevelOuter[1]=simple!=0?1.0:3.0;gl_TessLevelOuter[2]=simple!=0?1.0:4.0;gl_TessLevelInner[0]=simple!=0?1.0:3.0;}";
    const char *tes="#version 410 core\nlayout(triangles)in;in vec4 tc[];patch in vec4 pc;patch in float shade;out vec4 ec;uniform int mode;void main(){vec3 b=gl_TessCoord;gl_Position=b.x*gl_in[0].gl_Position+b.y*gl_in[1].gl_Position+b.z*gl_in[2].gl_Position;vec3 rgb=mix(pc.rgb,tc[0].rgb*b.x+tc[1].rgb*b.y+tc[2].rgb*b.z,shade);if(mode==2)rgb=pc.rgb;if(mode==3)rgb=vec3(shade,gl_TessLevelOuter[0]/8.0,float(gl_PrimitiveID));if(mode==4)rgb=mix(pc.rgb,tc[0].rgb,shade)*b.x+mix(pc.rgb,tc[1].rgb,shade)*b.y+mix(pc.rgb,tc[2].rgb,shade)*b.z;ec=vec4(rgb,gl_TessLevelOuter[0]/8.0+gl_TessLevelInner[0]/8.0+float(gl_PrimitiveID)*0.1);}";
    const char *fs="#version 410 core\nin vec4 ec;out vec4 result;void main(){result=ec;}";
    const char *sources[]={vs,tcs,tes,fs};const GLenum types[]={GL_VERTEX_SHADER,GL_TESS_CONTROL_SHADER,GL_TESS_EVALUATION_SHADER,GL_FRAGMENT_SHADER};
    GLuint program=glCreateProgram();
    for(int i=0;i<4;++i){GLuint shader=glCreateShader(types[i]);glShaderSource(shader,1,&sources[i],NULL);glCompileShader(shader);GLint ok;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);if(!ok){char log[2048];glGetShaderInfoLog(shader,sizeof log,NULL,log);glc_fail("Tess precision stage%d: %s",i,log);}glAttachShader(program,shader);glDeleteShader(shader);}
    glLinkProgram(program);GLint linked;glGetProgramiv(program,GL_LINK_STATUS,&linked);if(!linked)glc_fail("Tess precision link failed");
    GLint draw,read,viewport[4];glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw);glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);glGetIntegerv(GL_VIEWPORT,viewport);
    const float vertices[][6]={{-.9f,-.9f,1,0,0,1},{.9f,-.85f,0,1,0,.8f},{.85f,.9f,0,0,1,.6f},{-.85f,.85f,1,1,0,.4f}};
    const GLushort indices[]={0,1,2,0,2,3};GLuint vao,buffers[2],texture,fbo;
    glGenVertexArrays(1,&vao);glBindVertexArray(vao);glGenBuffers(2,buffers);glBindBuffer(GL_ARRAY_BUFFER,buffers[0]);glBufferData(GL_ARRAY_BUFFER,sizeof vertices,vertices,GL_STATIC_DRAW);glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,sizeof vertices[0],NULL);glVertexAttribPointer(1,4,GL_FLOAT,GL_FALSE,sizeof vertices[0],(void*)8);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,buffers[1]);glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof indices,indices,GL_STATIC_DRAW);
    glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,64,64,0,GL_RGBA,GL_FLOAT,NULL);glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)glc_fail("Tess precision framebuffer incomplete");
    glViewport(0,0,64,64);glDisable(GL_DEPTH_TEST);glDisable(GL_BLEND);glUseProgram(program);glUniform1i(glGetUniformLocation(program,"simple"),glc_variant==1);glUniform1i(glGetUniformLocation(program,"mode"),glc_variant);glPatchParameteri(GL_PATCH_VERTICES,3);glClearColor(.1,.1,.1,1);glClear(GL_COLOR_BUFFER_BIT);glDrawElements(GL_PATCHES,6,GL_UNSIGNED_SHORT,NULL);
    float pixels[4096][4];glReadPixels(0,0,64,64,GL_RGBA,GL_FLOAT,pixels);const int samples[][2]={{26,58},{57,33},{5,31},{26,31},{21,24},{12,22},{16,21},{55,18},{10,17},{32,32}};
    for(unsigned i=0;i<sizeof samples/sizeof samples[0];++i){int x=samples[i][0],y=samples[i][1];uint32_t bits[4];memcpy(bits,pixels[y*64+x],sizeof bits);fprintf(stderr,"tess_patch %s pixel=%d,%d bits=%08x,%08x,%08x,%08x\n",names[glc_variant],x,y,bits[0],bits[1],bits[2],bits[3]);}
    GLenum error=glGetError();if(error)glc_fail("Tess precision error%x",error);glUseProgram(0);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,draw);glBlitFramebuffer(0,0,64,64,0,0,64,64,GL_COLOR_BUFFER_BIT,GL_NEAREST);glBindFramebuffer(GL_READ_FRAMEBUFFER,read);glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&texture);glDeleteBuffers(2,buffers);glDeleteVertexArrays(1,&vao);glDeleteProgram(program);usleep(50000);
}
