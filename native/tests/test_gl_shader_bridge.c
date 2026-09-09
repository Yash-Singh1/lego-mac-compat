#include "gl_shader_bridge.h"
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static unsigned char *guest;
static uint32_t call(const char *name, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint32_t args[] = {a, b, c, d};
    uint64_t result = 0;
    assert(gl_shader_bridge32_dispatch(name, args, &result));
    return (uint32_t)result;
}
static uint32_t address(size_t offset) { return (uint32_t)(uintptr_t)(guest + offset); }
static GLuint shader(GLenum type, const char *text)
{
    strcpy((char *)guest + 128, "#version 120\n");
    strcpy((char *)guest + 256, text);
    uint32_t *strings = (void *)guest;
    strings[0] = address(128); strings[1] = address(256);
    GLint *lengths = (void *)(guest + 32);
    lengths[0] = (GLint)strlen((char *)guest + 128);
    lengths[1] = (GLint)strlen(text);
    GLuint object = call("_glCreateShaderObjectARB", type, 0, 0, 0);
    assert(object && glIsShader(object));
    call("glShaderSourceARB", object, 2, address(0), address(32));
    call("_glCompileShaderARB", object, 0, 0, 0);
    call("glGetObjectParameterivARB", object, GL_COMPILE_STATUS, address(64), 0);
    assert(*(GLint *)(guest + 64));
    return object;
}

static void test_hdr_toe(GLuint vertex)
{
    /* Arithmetic from the captured HDR pass, with texture samples supplied
       as uniforms. The original shader loses both detail and hue below its
       knee when mAutoExposureRangeMin is zero. */
    const char *source =
        "// D3DPS_VERSION(3,0)\n"
        "uniform vec4 values; uniform vec4 bloom;\n"
        "uniform vec4 mHDRkneeLow;\n"
        "uniform vec4 mHDRkneeF;\n"
        "uniform vec4 mBloomScale;\n"
        "uniform vec4 mAutoExposureRangeMin;\n"
        "const vec4 c0 = vec4(0.5,0.0,1.0,0.693147);\n"
        "vec4 cmp(vec4 a,vec4 b,vec4 c) { return vec4(a.x>=0.0?b.x:c.x,"
        "a.y>=0.0?b.y:c.y,a.z>=0.0?b.z:c.z,a.w>=0.0?b.w:c.w); }\n"
        "void main(void) { vec4 r0,r1,r2,r3;\n"
        "r0.x=values.w; r0.yzw=values.xyz+bloom.xyz*mBloomScale.xyz;\n"
        "r1.xyz=bloom.xyz*mBloomScale.w;\n"
        "r2.xyz = vec3(r0.yzww * r0.xxxx + (-(mHDRkneeLow)).xxxx);\n"
        "r3.xyz=vec3(max(r2,c0.yyyy)); r2.z=c0.z;\n"
        "r2.xyw=(r3.xyzz*mHDRkneeF.xxxx+r2.zzzz).xyw;\n"
        "r3.x=log2(abs(r2.x)); r3.y=log2(abs(r2.y)); r3.z=log2(abs(r2.w));\n"
        "r2.xyw=(r3.xyzz*c0.wwww).xyw; r0.x=1.0/mHDRkneeF.x;\n"
        "r2.xyw=(r2*r0.xxxx+mHDRkneeLow.xxxx).xyw;\n"
        "r3.xyz=vec3(cmp(vec4(-r0.yzw+mAutoExposureRangeMin.xyz,0.0),c0.yyyy,c0.zzzz));\n"
        "r0.x=dot(r3.xyz,r3.xyz);\n"
        "r0.xyz = vec3(cmp((-(r0)).xxxx, r0.yzww, r2.xyww));\n"
        "r0.w=1.0-mBloomScale.w; gl_FragColor=vec4(r0.xyz*r0.w+r1.xyz,1.0); }\n";
    const GLfloat knee = .0883883461f, shoulder = .952796817f;
    const GLfloat colors[][4] = {
        {0,0,0,1.05257988f}, {.0517883f,.0447083f,.0445557f,1.05257988f},
        {.001f,.02f,.07f,1.05257988f}, {0,.01f,.5f,1.05257988f},
        {.2f,1,4,1.05257988f}, {.0883783461f,.0883883461f,.0883983461f,1},
        {.01f,.2f,1,2}, {.005f,.006f,.007f,.5f}
    };
    for (int repaired = 0; repaired < 2; ++repaired) {
        if (!repaired) setenv("LP32_KEEP_TFU_GLSL_HDR", "1", 1);
        else unsetenv("LP32_KEEP_TFU_GLSL_HDR");
        GLuint fs = shader(GL_FRAGMENT_SHADER, source), p = glCreateProgram();
        if (repaired) {
            GLint length=0; glGetShaderiv(fs,GL_SHADER_SOURCE_LENGTH,&length);
            assert(length>0 && length<4096-256);
            glGetShaderSource(fs,length,NULL,(GLchar *)(guest+256));
            *(uint32_t *)guest=address(256);
            call("glShaderSourceARB",fs,1,address(0),0);
        }
        call("glCompileShader", fs, 0, 0, 0);
        GLint ok = 0; glGetShaderiv(fs, GL_COMPILE_STATUS, &ok); assert(ok);
        glAttachShader(p, vertex); glAttachShader(p, fs); glLinkProgram(p);
        glGetProgramiv(p, GL_LINK_STATUS, &ok); assert(ok); glUseProgram(p);
        glUniform4f(glGetUniformLocation(p,"mHDRkneeLow"),knee,knee,knee,knee);
        glUniform4f(glGetUniformLocation(p,"mHDRkneeF"),shoulder,shoulder,shoulder,shoulder);
        glUniform4f(glGetUniformLocation(p,"mAutoExposureRangeMin"),0,0,0,0);
        for (unsigned bloom_on=0; bloom_on<2; ++bloom_on) {
            GLfloat weight=bloom_on?.2f:0, glow=bloom_on?.015f:0;
            glUniform4f(glGetUniformLocation(p,"mBloomScale"),1.2f,1.2f,1.2f,weight);
            glUniform4f(glGetUniformLocation(p,"bloom"),glow,glow,glow,1);
            for (unsigned i=0; i<sizeof(colors)/sizeof(colors[0]); ++i) {
                glUniform4fv(glGetUniformLocation(p,"values"),1,colors[i]);
                glBegin(GL_QUADS);
                glVertex2f(-1,-1); glVertex2f(1,-1); glVertex2f(1,1); glVertex2f(-1,1);
                glEnd();
                GLfloat pixel[4]; glReadPixels(2,2,1,1,GL_RGBA,GL_FLOAT,pixel);
                for (unsigned c=0;c<3;++c) {
                    GLfloat linear=(colors[i][c]+glow*1.2f)*colors[i][3];
                    GLfloat expected=linear<=knee?linear:knee+log1pf((linear-knee)*shoulder)/shoulder;
                    expected=expected*(1-weight)+glow*weight;
                    if (repaired || linear>knee) assert(isfinite(pixel[c]) && fabsf(pixel[c]-expected)<.00003f);
                    if (!repaired && i==1 && !bloom_on) assert(fabsf(pixel[c]-knee)<.00001f);
                }
                assert(pixel[3]==1);
            }
        }
        assert(glGetError()==GL_NO_ERROR);
        glUseProgram(0); glDeleteProgram(p); glDeleteShader(fs);
    }
    puts("TFU HDR: PASS (shadow detail, hue, black, knee continuity, exposure, shoulder and bloom)");
}
int main(void)
{
    guest = mmap((void *)0x20000000, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(guest != MAP_FAILED && (uintptr_t)guest + 4096 <= UINT32_MAX);
    CGLPixelFormatAttribute attributes[] = {kCGLPFAAccelerated, 0};
    CGLPixelFormatObj format = NULL;
    CGLContextObj context = NULL;
    GLint count;
    assert(CGLChoosePixelFormat(attributes, &format, &count) == kCGLNoError);
    assert(CGLCreateContext(format, NULL, &context) == kCGLNoError);
    CGLDestroyPixelFormat(format);
    assert(CGLSetCurrentContext(context) == kCGLNoError);
    GLuint vertex = shader(GL_VERTEX_SHADER, "void main() { gl_Position = gl_Vertex; }");
    GLuint fragment = shader(GL_FRAGMENT_SHADER, "uniform vec4 tint; void main() { gl_FragColor = tint; }");
    GLuint program = call("glCreateProgramObjectARB", 0, 0, 0, 0);
    call("glAttachObjectARB", program, vertex, 0, 0);
    call("glAttachObjectARB", program, fragment, 0, 0);
    call("glLinkProgramARB", program, 0, 0, 0);
    call("glGetObjectParameterivARB", program, GL_LINK_STATUS, address(64), 0);
    assert(*(GLint *)(guest + 64));
    call("glUseProgram", program, 0, 0, 0);
    strcpy((char *)guest + 128, "tint");
    GLint uniform = (GLint)call("glGetUniformLocationARB", program, address(128), 0, 0);
    assert(uniform >= 0);
    GLfloat color[] = {1, 0, 0, 1};
    memcpy(guest + 256, color, sizeof(color));
    call("glUniform4fv", uniform, 1, address(256), 0);
    GLuint framebuffer, texture;
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffersEXT(1, &framebuffer); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, framebuffer);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, texture, 0);
    assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT);
    glViewport(0, 0, 4, 4);
    glBegin(GL_QUADS);
    glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(1, 1); glVertex2f(-1, 1);
    glEnd();
    unsigned char pixel[4];
    glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    assert(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255);
    assert(glGetError() == GL_NO_ERROR);
    /* Use TFU's actual generated helpers, with a negative RSQ argument and
       an invalid value in the unselected CMP lanes. The check renders the
       repaired code on the host GPU, through the multi-string guest API. */
    const char *legacy_fragment =
        "// D3DPS_VERSION(3,0)\n"
        "uniform vec4 values;\n"
        "vec4 cmp(in vec4 src0, in vec4 src1, in vec4 src2) {\n"
        "return mix(src2, src1, vec4(greaterThanEqual(src0, vec4(0.0))));\n}\n"
        "float cmp(in float src0, in float src1, in float src2) {\n"
        "return mix(src2, src1, float(src0 >= 0.0));\n}\n"
        "void main() {\n"
        "float bad = sqrt(values.y);\n"
        "vec4 chosen = cmp(vec4(0.0,-1.0,1.0,-1.0), "
        "vec4(values.z,bad,values.w,bad), vec4(bad,values.z,bad,values.w));\n"
        "gl_FragColor = vec4(inversesqrt(values.x), "
        "cmp(0.0,chosen.x,bad), chosen.z, 1.0);\n}\n";
    for (int repaired = 0; repaired < 2; ++repaired) {
        if (repaired) gl_shader_bridge32_enable_tfu_compat();
        GLuint legacy_shader = shader(GL_FRAGMENT_SHADER, legacy_fragment);
        GLuint legacy_program = glCreateProgram();
        glAttachShader(legacy_program, vertex); glAttachShader(legacy_program, legacy_shader);
        glLinkProgram(legacy_program);
        GLint linked = 0; glGetProgramiv(legacy_program, GL_LINK_STATUS, &linked); assert(linked);
        glUseProgram(legacy_program);
        GLfloat values[] = {-4.0f, -1.0f, .25f, .75f};
        glUniform4fv(glGetUniformLocation(legacy_program, "values"), 1, values);
        glBegin(GL_QUADS);
        glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(1, 1); glVertex2f(-1, 1);
        glEnd();
        glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        printf("TFU GLSL %s pixel: %u,%u,%u,%u\n", repaired ? "repaired" : "original", pixel[0], pixel[1], pixel[2], pixel[3]);
        if (repaired) {
            assert(pixel[0] >= 127 && pixel[0] <= 128 && pixel[1] == 64 && pixel[2] == 191 && pixel[3] == 255);
        }
        assert(glGetError() == GL_NO_ERROR);
        glUseProgram(0); glDeleteProgram(legacy_program); glDeleteShader(legacy_shader);
    }
    /* Reflection/specular shaders use LOG followed by arithmetic. Check the
       floating-point result directly: an RGBA8 readback hides NaNs/infinity. */
    GLuint log_shader = shader(GL_FRAGMENT_SHADER,
        "// D3DPS_VERSION(3,0)\n"
        "uniform vec4 values;\n"
        "void main(void) { gl_FragColor = vec4(log2(abs(values.x)), "
        "log2(abs(values.y)), log2(abs(values.z)), log2(abs(values.w))); }\n");
    GLuint log_program = glCreateProgram();
    /* Recompiling a stored/repaired source must not inject a second helper. */
    call("glCompileShader", log_shader, 0, 0, 0);
    GLint recompiled = 0; glGetShaderiv(log_shader, GL_COMPILE_STATUS, &recompiled); assert(recompiled);
    glAttachShader(log_program, vertex); glAttachShader(log_program, log_shader);
    glLinkProgram(log_program);
    GLint log_linked = 0; glGetProgramiv(log_program, GL_LINK_STATUS, &log_linked); assert(log_linked);
    glUseProgram(log_program);
    GLfloat log_values[] = {0.0f, -0.0f, -4.0f, .25f};
    glUniform4fv(glGetUniformLocation(log_program, "values"), 1, log_values);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F_ARB, 4, 4, 0, GL_RGBA, GL_FLOAT, NULL);
    glBegin(GL_QUADS);
    glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(1, 1); glVertex2f(-1, 1);
    glEnd();
    GLfloat log_pixel[4];
    glReadPixels(2, 2, 1, 1, GL_RGBA, GL_FLOAT, log_pixel);
    assert(isfinite(log_pixel[0]) && log_pixel[0] == -FLT_MAX);
    assert(isfinite(log_pixel[1]) && log_pixel[1] == -FLT_MAX);
    assert(log_pixel[2] == 2.0f && log_pixel[3] == -2.0f);
    assert(glGetError() == GL_NO_ERROR);
    glUseProgram(0); glDeleteProgram(log_program); glDeleteShader(log_shader);
    /* The captured reflection meshes contain zero normal/tangent vectors.
       Exercise their NRM -> ambient-light arithmetic in a floating target;
       normalized integer targets would conceal the invalid original output. */
    const char *normal_fragment =
        "// D3DPS_VERSION(3,0)\n"
        "uniform vec4 values;\n"
        "void main(void) { vec3 n = normalize(vec3(values)); "
        "gl_FragColor = vec4(vec3(0.25) + n * 0.5, 1.0); }\n";
    for (int repaired = 0; repaired < 2; ++repaired) {
        if (!repaired) setenv("LP32_KEEP_TFU_GLSL_NRM", "1", 1);
        else unsetenv("LP32_KEEP_TFU_GLSL_NRM");
        GLuint normal_shader = shader(GL_FRAGMENT_SHADER, normal_fragment);
        call("glCompileShader", normal_shader, 0, 0, 0);
        glGetShaderiv(normal_shader, GL_COMPILE_STATUS, &recompiled); assert(recompiled);
        GLuint normal_program = glCreateProgram();
        glAttachShader(normal_program, vertex); glAttachShader(normal_program, normal_shader);
        glLinkProgram(normal_program);
        GLint linked = 0; glGetProgramiv(normal_program, GL_LINK_STATUS, &linked); assert(linked);
        glUseProgram(normal_program);
        const GLfloat vectors[][4] = {{0, -0.0f, 0, 0}, {3, 4, 0, 0}, {-3, -4, 0, 0}};
        for (unsigned i = 0; i < 3; ++i) {
            glUniform4fv(glGetUniformLocation(normal_program, "values"), 1, vectors[i]);
            glBegin(GL_QUADS);
            glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(1, 1); glVertex2f(-1, 1);
            glEnd();
            GLfloat value[4]; glReadPixels(2, 2, 1, 1, GL_RGBA, GL_FLOAT, value);
            printf("TFU NRM %s vector %u: %g,%g,%g,%g\n", repaired ? "repaired" : "original",
                i, value[0], value[1], value[2], value[3]);
            if (repaired || i) {
                for (unsigned c = 0; c < 3; ++c) {
                    GLfloat expected = 0.25f + vectors[i][c] * (i ? 0.1f : 0.0f);
                    assert(isfinite(value[c]) && fabsf(value[c] - expected) < 0.00001f);
                }
                assert(value[3] == 1.0f);
            }
        }
        assert(glGetError() == GL_NO_ERROR);
        glUseProgram(0); glDeleteProgram(normal_program); glDeleteShader(normal_shader);
    }
    test_hdr_toe(vertex);
    call("glUseProgram", 0, 0, 0, 0);
    call("glDetachObjectARB", program, vertex, 0, 0);
    call("glDeleteShader", vertex, 0, 0, 0);
    call("glDeleteObjectARB", program, 0, 0, 0);
    call("glDeleteObjectARB", fragment, 0, 0, 0);
    assert(!glIsShader(vertex) && !glIsShader(fragment) && !glIsProgram(program));
    glDeleteFramebuffersEXT(1, &framebuffer); glDeleteTextures(1, &texture);
    CGLSetCurrentContext(NULL); CGLDestroyContext(context);
    munmap(guest, 4096);
    puts("GLSL bridge: PASS (ARB/core handles, multi-string source, uniform, rendered pixel, TFU D3D9 RSQ/CMP/LOG/NRM semantics)");
    return 0;
}
