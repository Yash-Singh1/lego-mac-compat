/* First-use commands must survive exhaustion of a presenting context's compile budget.
 * Build: make -j2 compile-budget-probe
 * glm_compile_budget --provider PATH
 */
#define GL_SILENCE_DEPRECATION 1
#import <Cocoa/Cocoa.h>
#include <OpenGL/gl3.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static void *provider;
static void (*p_glClearColor)(GLfloat,GLfloat,GLfloat,GLfloat);
static void (*p_glClearDepth)(GLdouble);
static void (*p_glClear)(GLbitfield);
static void (*p_glViewport)(GLint,GLint,GLsizei,GLsizei);
static void (*p_glReadPixels)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*);
static void (*p_glGenTextures)(GLsizei,GLuint*);
static void (*p_glBindTexture)(GLenum,GLuint);
static void (*p_glTexImage2D)(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*);
static void (*p_glTexParameteri)(GLenum,GLenum,GLint);
static void (*p_glGenFramebuffers)(GLsizei,GLuint*);
static void (*p_glBindFramebuffer)(GLenum,GLuint);
static void (*p_glFramebufferTexture2D)(GLenum,GLenum,GLenum,GLuint,GLint);
static GLenum (*p_glCheckFramebufferStatus)(GLenum);
static GLuint (*p_glCreateShader)(GLenum);
static void (*p_glShaderSource)(GLuint,GLsizei,const GLchar*const*,const GLint*);
static void (*p_glCompileShader)(GLuint);
static void (*p_glGetShaderiv)(GLuint,GLenum,GLint*);
static void (*p_glGetShaderInfoLog)(GLuint,GLsizei,GLsizei*,GLchar*);
static GLuint (*p_glCreateProgram)(void);
static void (*p_glAttachShader)(GLuint,GLuint);
static void (*p_glLinkProgram)(GLuint);
static void (*p_glGetProgramiv)(GLuint,GLenum,GLint*);
static void (*p_glUseProgram)(GLuint);
static void (*p_glGenVertexArrays)(GLsizei,GLuint*);
static void (*p_glBindVertexArray)(GLuint);
static void (*p_glDrawArrays)(GLenum,GLint,GLsizei);
static GLenum (*p_glGetError)(void);
static void (*p_glDrawBuffer)(GLenum);
static void (*p_glReadBuffer)(GLenum);
static void resolve(void) {
#define LOAD(n) do { p_##n=dlsym(provider,#n); if(!p_##n){fprintf(stderr,"missing %s\n",#n);exit(2);} } while(0)
LOAD(glClearColor);
LOAD(glDrawBuffer);
LOAD(glReadBuffer);
LOAD(glClearDepth);
LOAD(glClear);
LOAD(glViewport);
LOAD(glReadPixels);
LOAD(glGenTextures);
LOAD(glBindTexture);
LOAD(glTexImage2D);
LOAD(glTexParameteri);
LOAD(glGenFramebuffers);
LOAD(glBindFramebuffer);
LOAD(glFramebufferTexture2D);
LOAD(glCheckFramebufferStatus);
LOAD(glCreateShader);
LOAD(glShaderSource);
LOAD(glCompileShader);
LOAD(glGetShaderiv);
LOAD(glGetShaderInfoLog);
LOAD(glCreateProgram);
LOAD(glAttachShader);
LOAD(glLinkProgram);
LOAD(glGetProgramiv);
LOAD(glUseProgram);
LOAD(glGenVertexArrays);
LOAD(glBindVertexArray);
LOAD(glDrawArrays);
LOAD(glGetError);
}
static GLuint shader(GLenum type,const char *source) {
 GLuint s=p_glCreateShader(type);p_glShaderSource(s,1,&source,NULL);p_glCompileShader(s);
 GLint ok=0;p_glGetShaderiv(s,GL_COMPILE_STATUS,&ok);if(!ok){char log[2048];p_glGetShaderInfoLog(s,sizeof(log),NULL,log);fprintf(stderr,"shader: %s\n",log);exit(2);}return s;
}
int main(int argc,char **argv) {
 if(argc!=3 || strcmp(argv[1],"--provider")){fprintf(stderr,"usage: %s --provider PATH\n",argv[0]);return 2;}
 setenv("GLMETAL_COMPILE_BUDGET_MS","0",1);unsetenv("GLMETAL_SYNC_COMPILES");
 provider=dlopen(argv[2],RTLD_NOW|RTLD_LOCAL);if(!provider){fprintf(stderr,"%s\n",dlerror());return 2;}
 bool (*initialize)(void)=dlsym(provider,"glmetal_initialize");void (*install)(void)=dlsym(provider,"glmetal_install_appkit_overrides");
 if(!initialize||!install||!initialize())return 2;install();resolve();
 @autoreleasepool {
 [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
 NSWindow *w=[[NSWindow alloc]initWithContentRect:NSMakeRect(80,80,64,64) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];[w orderFront:nil];
 NSOpenGLPixelFormatAttribute attrs[]={NSOpenGLPFAOpenGLProfile,NSOpenGLProfileVersion3_2Core,NSOpenGLPFADoubleBuffer,NSOpenGLPFAColorSize,24,0};
 NSOpenGLPixelFormat *fmt=[[NSOpenGLPixelFormat alloc]initWithAttributes:attrs];
 NSOpenGLContext *ctx=[[NSOpenGLContext alloc]initWithFormat:fmt shareContext:nil];if(!ctx){fprintf(stderr,"no context\n");return 2;}[ctx setView:w.contentView];[ctx makeCurrentContext];
 p_glClearColor(0,0,0,1);p_glClear(GL_COLOR_BUFFER_BIT);[ctx flushBuffer];
 GLuint tex[2],fbo;p_glGenTextures(2,tex);p_glGenFramebuffers(1,&fbo);p_glBindFramebuffer(GL_FRAMEBUFFER,fbo);
 /* Different from the window's RGBA8 format, so the clear needs a fresh pipeline. */
 p_glBindTexture(GL_TEXTURE_2D,tex[0]);p_glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA16F,8,8,0,GL_RGBA,GL_FLOAT,NULL);
 p_glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex[0],0);
 p_glBindTexture(GL_TEXTURE_2D,tex[1]);p_glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH24_STENCIL8,8,8,0,GL_DEPTH_STENCIL,GL_UNSIGNED_INT_24_8,NULL);
 p_glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,tex[1],0);
 if(p_glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){fprintf(stderr,"incomplete FBO\n");return 2;}
 p_glViewport(0,0,8,8);p_glClearColor(.25,.5,.75,1);p_glClearDepth(.375);p_glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
 unsigned char color[4]={0};float depth=-1;p_glReadPixels(4,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,color);p_glReadPixels(4,4,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&depth);
 int clear_ok=abs(color[0]-64)<=1&&abs(color[1]-128)<=1&&abs(color[2]-191)<=1&&fabsf(depth-.375f)<.00001f;
 printf("first clear: %s rgba=%u,%u,%u,%u depth=%.8f\n",clear_ok?"PASS":"FAIL",color[0],color[1],color[2],color[3],depth);
 GLuint vs=shader(GL_VERTEX_SHADER,"#version 150 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2-1,0,1);}");
 GLuint fs=shader(GL_FRAGMENT_SHADER,"#version 150 core\nout vec4 c;void main(){c=vec4(.8125,.1875,.4375,1);}");
 GLuint prog=p_glCreateProgram();p_glAttachShader(prog,vs);p_glAttachShader(prog,fs);p_glLinkProgram(prog);GLint linked=0;p_glGetProgramiv(prog,GL_LINK_STATUS,&linked);if(!linked)return 2;
 GLuint vao;p_glGenVertexArrays(1,&vao);p_glBindVertexArray(vao);p_glUseProgram(prog);p_glDrawArrays(GL_TRIANGLES,0,3);
 p_glReadPixels(4,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,color);
 int draw_ok=abs(color[0]-207)<=1&&abs(color[1]-48)<=1&&abs(color[2]-112)<=1;
 GLenum error=p_glGetError();printf("first draw: %s rgba=%u,%u,%u,%u GLerror=%x\n",draw_ok?"PASS":"FAIL",color[0],color[1],color[2],color[3],error);
 /* Depth-only targets can use a packed depth/stencil format without a color attachment. */
 GLuint shadow_texture,shadow_fbo;
 p_glGenTextures(1,&shadow_texture);p_glGenFramebuffers(1,&shadow_fbo);
 p_glBindTexture(GL_TEXTURE_2D,shadow_texture);
 p_glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH24_STENCIL8,8,16,0,GL_DEPTH_STENCIL,GL_UNSIGNED_INT_24_8,NULL);
 p_glBindFramebuffer(GL_FRAMEBUFFER,shadow_fbo);
 p_glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,shadow_texture,0);
 p_glDrawBuffer(GL_NONE);p_glReadBuffer(GL_NONE);
 if(p_glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){fprintf(stderr,"incomplete depth-only FBO\n");return 2;}
 p_glViewport(0,0,8,16);p_glClearDepth(1);p_glClear(GL_DEPTH_BUFFER_BIT);
 float shadow_depth=-1;p_glReadPixels(4,8,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&shadow_depth);
 GLenum shadow_error=p_glGetError();int shadow_ok=fabsf(shadow_depth-1)<.00001f&&!shadow_error;
 printf("first depth-only clear: %s depth=%.8f GLerror=%x\n",shadow_ok?"PASS":"FAIL",shadow_depth,shadow_error);
 [NSOpenGLContext clearCurrentContext];[w close];return clear_ok&&draw_ok&&shadow_ok&&!error?0:1;
 }
}
