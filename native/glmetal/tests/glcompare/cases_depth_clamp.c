/* Distinguish clamping vertex depth from clamping interpolated fragment depth. */
#include "glc_gl_core.h"
#include "glcompare.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const char *const clamp_variants[] = {"implicit", "fragcoord", "rawvarying"};
GLC_CASE_VARIANTS(core_depth_clamp_interpolation, clamp_variants, .profile = GLC_CORE)
{
    const char *vertex = "#version 410 core\n"
        "noperspective out float rawDepth; void main(){\n"
        " vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2)*2.0-1.0;\n"
        " gl_Position=vec4(p,2.0*p.x,1.0); rawDepth=p.x+0.5; }";
    const char *fragment[] = {
        "#version 410 core\nout vec4 frag; void main(){frag=vec4(0,1,0,1);}",
        "#version 410 core\nout vec4 frag; void main(){frag=vec4(0,1,0,1);"
        " gl_FragDepth=clamp(gl_FragCoord.z,0.0,1.0);}",
        "#version 410 core\nnoperspective in float rawDepth; out vec4 frag;"
        " void main(){frag=vec4(0,1,0,1);gl_FragDepth=clamp(rawDepth,0.0,1.0);}",
    };
    GLuint program = glc_program(vertex, fragment[glc_variant], NULL);
    glUseProgram(program);
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_CLAMP);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthRange(0, 1);
    glDepthFunc(GL_ALWAYS);
    glClearDepth(1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    const int x[] = {8, 32, 56};
    const float expected[] = {0, 0.515625f, 1};
    float depth[3] = {0};
    for (int i = 0; i < 3; ++i)
        glReadPixels(x[i], 32, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, depth + i);
    fprintf(stderr, "Depth clamp %s stored depths x8/32/56: %.9g %.9g %.9g; expected 0 .515625 1\n",
            clamp_variants[glc_variant], depth[0], depth[1], depth[2]);
    /* Also measure coverage before any assertion, to retain both observations. */
    glDepthFunc(GL_LESS);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    GLubyte color[3][4] = {{0}};
    for (int i = 0; i < 3; ++i)
        glReadPixels(x[i], 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, color[i]);
    fprintf(stderr, "Depth clamp %s LESS green channels x8/32/56: %u %u %u; expected 255 255 0\n",
            clamp_variants[glc_variant], color[0][1], color[1][1], color[2][1]);
    GLenum error = glGetError();
    if (error) glc_fail("Depth clamp %s GL error: %x", clamp_variants[glc_variant], error);
    for (int i = 0; i < 3; ++i)
        if (fabsf(depth[i] - expected[i]) > 0.000001f)
            glc_fail("Depth clamp %s depth x%d: %.9g != %.9g", clamp_variants[glc_variant], x[i], depth[i], expected[i]);
    if (color[0][1] != 255 || color[1][1] != 255 || color[2][1] != 0)
        glc_fail("Depth clamp %s far-plane LESS coverage differs", clamp_variants[glc_variant]);
}

static const char *const original_clamp_variants[] = {
    "implicit_no_blend", "implicit_blend", "fragcoord_no_blend", "fragcoord_blend", "rawvarying_no_blend", "rawvarying_blend"
};
GLC_CASE_VARIANTS(core_depth_clamp_original_quad, original_clamp_variants, .profile = GLC_CORE, .tolerance = 1)
{
    const char *vertex = "#version 410 core\n"
        "in vec2 position; in vec4 color; out vec4 v; noperspective out float rawDepth;\n"
        "void main(){gl_Position=vec4(position,1.5-color.r*3.0,1.0);"
        " v=color; rawDepth=(gl_Position.z+gl_Position.w)*0.5/gl_Position.w;}";
    const char *fragment[] = {
        "#version 410 core\nin vec4 v; out vec4 frag; void main(){frag=v;}",
        "#version 410 core\nin vec4 v; out vec4 frag;"
        " void main(){frag=v;gl_FragDepth=clamp(gl_FragCoord.z,0.0,1.0);}",
        "#version 410 core\nin vec4 v; noperspective in float rawDepth; out vec4 frag;"
        " void main(){frag=v;gl_FragDepth=clamp(rawDepth,0.0,1.0);}",
    };
    const char *attributes[] = {"position", "color", NULL};
    GLuint program = glc_program(vertex, fragment[glc_variant / 2], attributes);
    glUseProgram(program);
    /* Exact position/color data from core_blend_per_target_and_depth_clamp. */
    const float vertices[] = {
        -0.9f,-0.9f, 1,0,0,1, 0,0,
         0.9f,-0.85f, 0,1,0,0.8f, 1,0,
         0.85f,0.9f, 0,0,1,0.6f, 1,1,
        -0.85f,0.85f, 1,1,0,0.4f, 0,1,
    };
    GLuint vao, buffer;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 32, (void *)0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 32, (void *)8);
    glEnable(GL_DEPTH_CLAMP);
    glEnable(GL_DEPTH_TEST);
    if (glc_variant & 1) glEnable(GL_BLEND);
    else glDisable(GL_BLEND);
    glBlendFunci(0, GL_ONE_MINUS_DST_COLOR, GL_ONE);
    glClearColor(0.2f, 0.3f, 0.4f, 1);
    const int x[] = {8, 32, 56};
    const float expected[] = {0, 0.534199048f, 1};
    GLubyte direct[3][4] = {{0}}, repeat[3][4] = {{0}};
    float direct_depth[3] = {0}, always_depth[3] = {0}, clear_depth[3] = {0};
    GLint initial_function;
    GLdouble initial_clear;
    glGetIntegerv(GL_DEPTH_FUNC, &initial_function);
    glGetDoublev(GL_DEPTH_CLEAR_VALUE, &initial_clear);
    /* Preserve the original case's first clear/draw sequence before any readback. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    for (int i = 0; i < 3; ++i) {
        glReadPixels(x[i], 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, direct[i]);
        glReadPixels(x[i], 32, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, direct_depth + i);
    }
    glDepthFunc(GL_ALWAYS);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    for (int i = 0; i < 3; ++i)
        glReadPixels(x[i], 32, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, clear_depth + i);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    for (int i = 0; i < 3; ++i)
        glReadPixels(x[i], 32, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, always_depth + i);
    glDepthFunc(GL_LESS);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    for (int i = 0; i < 3; ++i)
        glReadPixels(x[i], 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, repeat[i]);
    fprintf(stderr, "Original depth clamp %s initial func=%x clear=%a\n",
            original_clamp_variants[glc_variant], initial_function, initial_clear);
    for (int i = 0; i < 3; ++i)
        fprintf(stderr, "Original depth clamp %s x%d direct RGBA=%u/%u/%u/%u depth=%a clear=%a ALWAYS=%a repeat RGBA=%u/%u/%u/%u\n",
                original_clamp_variants[glc_variant], x[i], direct[i][0], direct[i][1], direct[i][2], direct[i][3],
                direct_depth[i], clear_depth[i], always_depth[i], repeat[i][0], repeat[i][1], repeat[i][2], repeat[i][3]);
    GLenum error = glGetError();
    if (error) glc_fail("Original depth clamp %s GL error: %x", original_clamp_variants[glc_variant], error);
    if (initial_function != GL_LESS || initial_clear != 1)
        glc_fail("Original depth clamp unexpected initial state");
    for (int i = 0; i < 3; ++i) {
        if (clear_depth[i] != 1 || (i == 1 ? fabsf(always_depth[i] - expected[i]) > 0.0001f : always_depth[i] != expected[i]))
            glc_fail("Original depth clamp %s depth at x%d differs", original_clamp_variants[glc_variant], x[i]);
        for (int channel = 0; channel < 4; ++channel)
            if (abs((int)direct[i][channel] - (int)repeat[i][channel]) > 1)
                glc_fail("Original depth clamp %s direct/repeat color differs at x%d", original_clamp_variants[glc_variant], x[i]);
    }
    const GLubyte background[] = {51,77,102,255};
    for (int channel = 0; channel < 4; ++channel)
        if (abs((int)direct[2][channel] - (int)background[channel]) > 1)
            glc_fail("Original depth clamp %s far-plane pixel passed LESS", original_clamp_variants[glc_variant]);
}

/* Both viewports receive the same vertices, but use opposite depth ranges. */
GLC_CASE(core_depth_clamp_indexed_ranges, .profile = GLC_CORE)
{
    const char *sources[] = {
        "#version 410 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2)*2.-1.;gl_Position=vec4(p,2.*p.x,1);}",
        ("#version 410 core\nlayout(triangles,invocations=2) in;layout(triangle_strip,max_vertices=3) out;"
        "void main(){for(int i=0;i<3;++i){gl_Position=gl_in[i].gl_Position;gl_ViewportIndex=gl_InvocationID;EmitVertex();}EndPrimitive();}"),
        "#version 410 core\nout vec4 frag;void main(){frag=vec4(0,1,0,1);}"
    };
    const GLenum stages[] = {GL_VERTEX_SHADER, GL_GEOMETRY_SHADER, GL_FRAGMENT_SHADER};
    GLuint program = glCreateProgram();
    for (int i=0;i<3;++i) {
        GLuint shader=glCreateShader(stages[i]);
        glShaderSource(shader,1,sources+i,NULL);glCompileShader(shader);
        GLint ok=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
        if(!ok){char log[4096];glGetShaderInfoLog(shader,sizeof log,NULL,log);glc_fail("Indexed depth shader: %s",log);}
        glAttachShader(program,shader);glDeleteShader(shader);
    }
    glLinkProgram(program);
    GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);
    if(!linked){char log[4096];glGetProgramInfoLog(program,sizeof log,NULL,log);glc_fail("Indexed depth program: %s",log);}
    glUseProgram(program);
    GLuint vao;glGenVertexArrays(1,&vao);glBindVertexArray(vao);
    glViewportIndexedf(0,0,0,32,64);glViewportIndexedf(1,32,0,32,64);
    glDepthRangeIndexed(0,.2,.8);glDepthRangeIndexed(1,.8,.2);
    glEnable(GL_DEPTH_CLAMP);glEnable(GL_DEPTH_TEST);glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);glClearDepth(1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES,0,3);
    const int local_x[]={4,16,28};
    const float expected[2][3]={{.2f,.51875f,.8f},{.8f,.48125f,.2f}};
    float depth[2][3];
    for(int v=0;v<2;++v)for(int i=0;i<3;++i)
        glReadPixels(v*32+local_x[i],32,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&depth[v][i]);
    for(int v=0;v<2;++v)
        fprintf(stderr,"Indexed depth clamp viewport %d depths=%a/%a/%a\n",v,depth[v][0],depth[v][1],depth[v][2]);
    GLenum error=glGetError();if(error)glc_fail("Indexed depth clamp GL error %x",error);
    for(int v=0;v<2;++v)for(int i=0;i<3;++i)
        if(fabsf(depth[v][i]-expected[v][i])>0.00001f)
            glc_fail("Indexed depth clamp viewport %d sample %d differs",v,i);
}

static const char *const offset_variants[]={"implicit","user_depth"};
GLC_CASE_VARIANTS(core_depth_clamp_polygon_offset, offset_variants, .profile = GLC_CORE)
{
    const char *vertex="#version 410 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2)*2.-1.;gl_Position=vec4(p,.5*p.x,1);}";
    const char *fragment[]={
        "#version 410 core\nout vec4 frag;void main(){frag=vec4(0,1,0,1);}",
        "#version 410 core\nout vec4 frag;void main(){frag=vec4(0,1,0,1);gl_FragDepth=.375;}"
    };
    GLuint program=glc_program(vertex,fragment[glc_variant],NULL);glUseProgram(program);
    GLuint vao;glGenVertexArrays(1,&vao);glBindVertexArray(vao);
    glEnable(GL_DEPTH_TEST);glDepthFunc(GL_ALWAYS);glDepthMask(GL_TRUE);glDepthRange(0,1);
    glEnable(GL_POLYGON_OFFSET_FILL);glPolygonOffset(1,256);
    float depth[2];
    for(int clamp=0;clamp<2;++clamp){
        if(clamp)glEnable(GL_DEPTH_CLAMP);else glDisable(GL_DEPTH_CLAMP);
        glClearDepth(1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES,0,3);
        glReadPixels(32,32,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,depth+clamp);
    }
    fprintf(stderr,"Depth clamp polygon offset %s disabled=%a enabled=%a\n",offset_variants[glc_variant],depth[0],depth[1]);
    GLenum error=glGetError();if(error)glc_fail("Depth clamp polygon offset GL error %x",error);
    if(fabsf(depth[0]-depth[1])>0.00001f)
        glc_fail("Depth clamp polygon offset %s changed stored depth",offset_variants[glc_variant]);
    if(glc_variant==1 && fabsf(depth[1]-.375f)>0.00001f)
        glc_fail("Depth clamp overwrote user fragment depth");
}
