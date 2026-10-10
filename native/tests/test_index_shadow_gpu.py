#!/usr/bin/env python3
"""Exercise production index publications with queued GLMetal draws and wraps.

Runs one offscreen GPU workload at a time. Reports submission/completion times,
not gameplay FPS. Requires `make -C native/glmetal glcompare` and a built driver.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
glm = root / 'glmetal'
source = (root / 'src/objc_bridge.m').read_text()
start = source.index('struct index_shadow {')
end = source.index('\nstatic void discard_buffer_state(', start)
code = source[start:end]
prefix = r'''
#include "glc_gl_core.h"
#include "glcompare.h"
#include <OpenGL/OpenGL.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
extern void *glc_lookup(const char *);
enum lp32_title { title_mw3 };
struct profile { enum lp32_title title; };
static struct profile profile;
static struct profile *lp32_profile(void) { return &profile; }
static bool lp32_title_is_mw_sdl(enum lp32_title t) { (void)t; return true; }
static bool lp32_gl_backend_is_replacement(void) { return true; }
static CGLContextObj probe_current(void) {
    CGLContextObj (*fn)(void) = glc_lookup("CGLGetCurrentContext"); return fn();
}
static CGLShareGroupObj probe_group(CGLContextObj ctx) {
    CGLShareGroupObj (*fn)(CGLContextObj) = glc_lookup("CGLGetShareGroup"); return fn(ctx);
}
#define CGLGetCurrentContext probe_current
#define CGLGetShareGroup probe_group
static GLuint element, array;
static GLuint bound_buffer_for_target(GLenum t) { return t == GL_ELEMENT_ARRAY_BUFFER ? element : array; }
static void probe_bind(GLenum t, GLuint b) {
    glBindBuffer(t,b);
    if (t == GL_ELEMENT_ARRAY_BUFFER) element = b;
    if (t == GL_ARRAY_BUFFER) array = b;
}
#undef glBindBuffer
#define glBindBuffer probe_bind
'''
case = r'''
static double seconds(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
    return now.tv_sec + now.tv_nsec * 1e-9;
}
static void draw_frame(unsigned frame) {
    glClearColor(0,0,1,1); glClear(GL_COLOR_BUFFER_BIT);
    for (unsigned draw=0;draw<512;++draw) {
        unsigned tile = (draw*73+frame*31)%512;
        GLuint base=tile*4, indices[]={base,base+1,base+2,base+2,base+1,base+3};
        if (!index_shadow_copy(element,0,sizeof(indices),indices,true)) glc_fail("shadow update failed");
        const void *pointer=NULL;uint32_t restore=0;
        if (!objc_bridge32_index_shadow_acquire(0,6,GL_UNSIGNED_INT,&pointer,&restore)) glc_fail("publication failed");
        glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,pointer);
        objc_bridge32_index_shadow_release(restore);
    }
    glFlush();
}
static unsigned check_frame(void) {
    unsigned char image[64*32*4];glReadPixels(0,0,64,32,GL_RGBA,GL_UNSIGNED_BYTE,image);
    unsigned bad=0;
    for(unsigned y=0;y<32;++y) for(unsigned x=0;x<64;++x) {
        unsigned tile=(y/2)*32+x/2,at=(y*64+x)*4;
        if(image[at]!=(tile&1?255:0)||image[at+1]!=(tile&2?255:0)||image[at+2]!=0||image[at+3]!=255)++bad;
    }
    if(bad) glc_fail("queued index publications corrupted %u pixels",bad);
    return bad;
}
GLC_CASE(bridge_index_arena, .profile=GLC_CORE, .tolerance=0)
{
    GLuint p=glc_program(
        "#version 410 core\nflat out vec4 tint;void main(){"
        "int tile=gl_VertexID/4,corner=gl_VertexID%4;"
        "vec2 xy=vec2(tile%32,tile/32)+vec2(corner&1,corner>>1);"
        "gl_Position=vec4(xy/vec2(32,16)*2-1,0,1);"
        "tint=vec4(float(tile&1),float((tile>>1)&1),0,1);}",
        "#version 410 core\nflat in vec4 tint;out vec4 color;void main(){color=tint;}",NULL);
    glUseProgram(p);
    GLuint vao,buffer;glGenVertexArrays(1,&vao);glBindVertexArray(vao);
    glGenBuffers(1,&buffer);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,buffer);
    GLuint zeros[6]={0};glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(zeros),zeros,GL_STREAM_DRAW);
    index_shadow_buffer_data(GL_ELEMENT_ARRAY_BUFFER,sizeof(zeros),zeros,GL_STREAM_DRAW);
    glDisable(GL_BLEND);glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glDisable(GL_DITHER);
    glViewport(0,0,64,32);
    draw_frame(0);glFinish();
    unsigned frames=getenv("LP32_INDEX_PROBE_FRAMES")?atoi(getenv("LP32_INDEX_PROBE_FRAMES")):256;
    if (frames<1||frames>4096) glc_fail("invalid frame count");
    double begin=seconds();
    for(unsigned frame=0;frame<frames;++frame) {
        draw_frame(frame);
        /* The 2 MB arena wraps during this frame, after its earlier draws. */
        if(frame==169) check_frame();
    }
    double submitted=seconds();glFinish();double completed=seconds();
    unsigned bad=check_frame();
    fprintf(stderr,"index-probe arena=%d frames=%u draws=%u submission_ms=%.3f completion_ms=%.3f bad_pixels=%u\n",
        !getenv("LP32_NO_INDEX_ARENA"),frames,frames*512,(submitted-begin)*1000,(completed-begin)*1000,bad);
    if(bad) glc_fail("queued index publications corrupted %u pixels",bad);
    if(glGetError()!=GL_NO_ERROR) glc_fail("GL error after index publications");
    index_shadow_release_buffer(buffer,false);glDeleteBuffers(1,&buffer);glDeleteVertexArrays(1,&vao);glDeleteProgram(p);
}
'''
with tempfile.TemporaryDirectory(prefix='index-shadow-gpu-') as directory:
    temp=Path(directory)
    (temp/'probe.c').write_text(prefix+code+case)
    subprocess.run(['clang','-arch','x86_64','-arch','arm64','-O2','-Werror','-Wno-deprecated-declarations',
                    '-I'+str(glm/'tests/glcompare'),'-I'+str(glm/'build/gen'),'-dynamiclib',str(temp/'probe.c'),
                    str(glm/'tests/glcompare/glc_util.c'),'-Wl,-undefined,dynamic_lookup','-o',str(temp/'cases.dylib')],check=True)
    for arch in ('x86_64','arm64'):
        for mode in ('off','on'):
            env={**os.environ,'GLMETAL_THREAD_ALL':'1','LP32_INDEX_STATS':'1'}
            env.pop('LP32_NO_INDEX_ARENA',None)
            if mode=='off':env['LP32_NO_INDEX_ARENA']='1'
            result=subprocess.run(['arch','-'+arch,str(glm/'build/glcompare'),'--provider',str(glm/'build/libGLMetal.dylib'),
                                   '--cases',str(temp/'cases.dylib'),'--out',str(temp/(arch+'-'+mode))],
                                   env=env,capture_output=True,text=True,timeout=90)
            print(arch,mode,result.stdout,result.stderr,flush=True)
            if result.returncode:raise SystemExit(result.returncode)
