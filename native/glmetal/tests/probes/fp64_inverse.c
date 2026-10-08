#include "glc_gl_core.h"
#include "glcompare.h"
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *const names[] = {"dmat3", "dmat4"};
GLC_CASE_VARIANTS(driver_fp64_inverse, names, .profile = GLC_CORE)
{
    int n = glc_variant + 3;
    char vertex[512];
    snprintf(vertex, sizeof vertex, "#version 410 core\nuniform dmat%d a;out dmat%d original;out dmat%d result;void main(){original=a;result=inverse(a);gl_Position=vec4(0);}", n,n,n);
    GLuint shader = glCreateShader(GL_VERTEX_SHADER), program = glCreateProgram();
    const char *source = vertex;
    glShaderSource(shader,1,&source,NULL);
    glCompileShader(shader);
    glAttachShader(program,shader);
    const char *varyings[] = {"original","result"};
    glTransformFeedbackVaryings(program,2,varyings,GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(program);
    GLint linked;
    glGetProgramiv(program,GL_LINK_STATUS,&linked);
    if (!linked) { char log[2048]; glGetProgramInfoLog(program,sizeof log,NULL,log); glc_fail("Inverse link: %s",log); }
    int failures=0;
    for (int v=0;v<2;++v) {
        char name[64]; GLsizei length; GLint size; GLenum type;
        glGetTransformFeedbackVarying(program,v,sizeof name,&length,&size,&type,name);
        if(type!=(GLenum)(n==3?GL_DOUBLE_MAT3:GL_DOUBLE_MAT4)||size!=1)++failures;
        fprintf(stderr,"inverse%d capture=%s type=%04x size=%d expected=%04x\n",n,name,type,size,n==3?GL_DOUBLE_MAT3:GL_DOUBLE_MAT4);
    }
    GLuint vao, buffer;
    glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    glGenBuffers(1,&buffer); glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,buffer);
    double output[32];
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER,sizeof output,NULL,GL_STREAM_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,buffer);
    glUseProgram(program); glEnable(GL_RASTERIZER_DISCARD);
    GLint location=glGetUniformLocation(program,"a");
    for (int test=0;test<4;++test) {
        double a[16]={0},expected[16]={0};
        for(int i=0;i<n;++i) { a[i*n+i]=test==0?ldexp(1,i):1; expected[i*n+i]=1/a[i*n+i]; }
        if(test==1) { a[0]=a[n+1]=expected[0]=expected[n+1]=0; a[1]=a[n]=expected[1]=expected[n]=1; }
        if(test==2) { a[n]=.5; expected[n]=-.5; }
        if(test==3 && n==3) {
            const double dense[] = {1,.125,.25,.125,65.0/64,5.0/32,.25,5.0/32,69.0/64};
            const double inverse[] = {4385.0/4096,-49.0/512,-15.0/64,-49.0/512,65.0/64,-.125,-15.0/64,-.125,1};
            memcpy(a,dense,sizeof dense); memcpy(expected,inverse,sizeof inverse);
        }
        if(test==3 && n==4) {
            const double hadamard[] = {1,1,1,1,1,-1,1,-1,1,1,-1,-1,1,-1,-1,1};
            memcpy(a,hadamard,sizeof hadamard);
            for(int c=0;c<n;++c)for(int r=0;r<n;++r)expected[c*n+r]=a[r*n+c]/4;
        }
        if(n==3)glUniformMatrix3dv(location,1,GL_FALSE,a); else glUniformMatrix4dv(location,1,GL_FALSE,a);
        memset(output,0xa5,sizeof output); glBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER,0,sizeof output,output);
        glBeginTransformFeedback(GL_POINTS); glDrawArrays(GL_POINTS,0,1); glEndTransformFeedback();
        glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER,0,sizeof output,output);
        double residual=0; int exact=1;
        for(int i=0;i<n*n;++i) { uint64_t bits; memcpy(&bits,&output[n*n+i],8); fprintf(stderr,"inverse%d matrix=%d component=%d value=%a bits=%016llx expected=%a\n",n,test,i,output[n*n+i],(unsigned long long)bits,expected[i]); if(output[i]!=a[i]||output[n*n+i]!=expected[i])exact=0; }
        for(int c=0;c<n;++c)for(int r=0;r<n;++r) { double sum=0;for(int k=0;k<n;++k)sum+=a[k*n+r]*output[n*n+c*n+k];double error=fabs(sum-(c==r));if(!isfinite(error))residual=INFINITY;else residual=fmax(residual,error); }
        /* gamma(2n) bounds n products plus additions; max row norm is2^3. */
        double bound=(2*n*DBL_EPSILON)/(1-2*n*DBL_EPSILON)*8;
        fprintf(stderr,"inverse%d matrix=%d exact=%d residual=%a arithmetic_bound=%a within_bound=%d\n",n,test,exact,residual,bound,residual<=bound);
        if(!exact||residual>bound)++failures;
        usleep(50000);
    }
    GLenum error=glGetError();if(error)glc_fail("Inverse diagnostic GL error %x",error);
    glDisable(GL_RASTERIZER_DISCARD);glUseProgram(0);glDeleteBuffers(1,&buffer);glDeleteVertexArrays(1,&vao);glDeleteProgram(program);glDeleteShader(shader);
    if(failures)glc_fail("Inverse diagnostic %d failed type/exact/residual checks",failures);
    glClearColor(.25,.5,.75,1);glClear(GL_COLOR_BUFFER_BIT);
}
