#include "glc_gl_core.h"
#include "glcompare.h"

static const char *const geometry_cube_variants[] = {"first_off", "second_off", "first_on", "second_on"};
GLC_CASE_VARIANTS(core_geometry_cube_sampler_array, geometry_cube_variants, .profile = GLC_CORE, .tolerance = 1)
{
    const char *sources[] = {
        "#version 410 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2-1,0,1);}",
        ("#version 410 core\nlayout(triangles) in;layout(triangle_strip,max_vertices=3) out;"
         "uniform samplerCube cubes[2];uniform int selected;flat out vec4 color;"
         "void main(){vec3 direction=vec3(.2,-.1,1);"
         "vec4 c=textureLod(cubes[selected],direction,1.5);"
         "for(int i=0;i<3;++i){gl_Position=gl_in[i].gl_Position;color=c;EmitVertex();}EndPrimitive();}"),
        "#version 410 core\nflat in vec4 color;out vec4 frag;void main(){frag=color;}"
    };
    const GLenum stages[] = {GL_VERTEX_SHADER, GL_GEOMETRY_SHADER, GL_FRAGMENT_SHADER};
    GLuint program = glCreateProgram();
    for (int i = 0; i < 3; ++i) {
        GLuint shader = glCreateShader(stages[i]);
        glShaderSource(shader, 1, sources + i, NULL);
        glCompileShader(shader);
        GLint ok;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[4096];
            glGetShaderInfoLog(shader, sizeof log, NULL, log);
            glc_fail("Cube geometry shader: %s", log);
        }
        glAttachShader(program, shader);
        glDeleteShader(shader);
    }
    glLinkProgram(program);
    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[4096];
        glGetProgramInfoLog(program, sizeof log, NULL, log);
        glc_fail("Cube geometry link: %s", log);
    }
    glUseProgram(program);
    GLuint vao, cubes[2], sampler;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenTextures(2, cubes);
    const GLint units[] = {1, 3};
    for (int cube = 0; cube < 2; ++cube) {
        glActiveTexture(GL_TEXTURE0 + units[cube]);
        glBindTexture(GL_TEXTURE_CUBE_MAP, cubes[cube]);
        for (int level = 0, size = 16; level < 5; ++level, size >>= 1) {
            float pixels[16 * 16 * 4];
            for (int i = 0; i < size * size; ++i) {
                pixels[4 * i] = cube ? .1f : (level + 1) / 8.0f;
                pixels[4 * i + 1] = cube ? (level + 1) / 8.0f : .2f;
                pixels[4 * i + 2] = .3f;
                pixels[4 * i + 3] = 1;
            }
            for (int face = 0; face < 6; ++face)
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, level, GL_RGBA32F,
                             size, size, 0, GL_RGBA, GL_FLOAT, pixels);
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, 4);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        if (!cube) {
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL, 1);
            glTexParameterf(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_LOD, .75f);
            glTexParameterf(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LOD, 1.25f);
        } else {
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameterf(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LOD, 0);
        }
    }
    glGenSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, .25f);
    glSamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, .5f);
    glBindSampler(3, sampler);
    glUniform1iv(glGetUniformLocation(program, "cubes[0]"), 2, units);
    glUniform1i(glGetUniformLocation(program, "selected"), glc_variant & 1);
    if (glc_variant & 2) glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    else glDisable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    GLenum error = glGetError();
    if (error) glc_fail("Cube geometry sampler array GL error %x", error);
    glBindSampler(3, 0);
    glDeleteSamplers(1, &sampler);
    glDeleteTextures(2, cubes);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
}
