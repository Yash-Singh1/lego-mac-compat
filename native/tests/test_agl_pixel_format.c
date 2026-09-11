#include "agl_pixel_format.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
int main(void){
    const int fullscreen[]={4,5,72,54,50,32,73,12,24,13,8,0};
    const int expected[]={4,5,72,50,32,73,12,24,13,8,0};
    int output[32];
    assert(lp32_agl_window_attributes(fullscreen,12,output,32)==11);
    assert(!memcmp(output,expected,sizeof(expected)));
    const int samples[]={4,56,54,54,12,24,0};
    assert(lp32_agl_window_attributes(samples,7,output,32)==6);
    assert(output[1]==56 && output[2]==54 && output[3]==12);
    assert(!lp32_agl_window_attributes(samples,5,output,32));
    assert(!lp32_agl_window_attributes(fullscreen,12,output,3));
    const int unknown[]={9999,54,0};assert(!lp32_agl_window_attributes(unknown,3,output,32));
    void *library=dlopen("/System/Library/Frameworks/AGL.framework/AGL",RTLD_NOW);assert(library);
    void *(*choose)(void*,int,const int*)=dlsym(library,"aglChoosePixelFormat");
    unsigned char (*describe)(void*,int,int*)=dlsym(library,"aglDescribePixelFormat");
    void (*destroy)(void*)=dlsym(library,"aglDestroyPixelFormat");assert(choose && describe && destroy);
    assert(lp32_agl_window_attributes(fullscreen,12,output,32)==11);
    void *format=choose(NULL,0,output);assert(format);
    int depth=0,stencil=0;assert(describe(format,12,&depth) && describe(format,13,&stencil));
    assert(depth>=24 && stencil>=8);destroy(format);dlclose(library);
    printf("AGL pixel format PASS (attribute arity, limits, native depth=%d stencil=%d)\n",depth,stencil);
}
