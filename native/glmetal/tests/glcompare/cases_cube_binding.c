#include "glc_gl_core.h"
#include "glcompare.h"

static const char *const cube_binding_names[] = {
    "helper_off", "helper_on", "array_off", "array_on", "array_helper_grad_off", "array_helper_grad_on"
};

GLC_CASE_VARIANTS(core_cube_shadow_binding, cube_binding_names,
                  .profile = GLC_CORE, .tolerance = 2)
{
    const char *vertex = "#version 410 core\nout vec2 uv;void main(){"
        "vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
        "uv=p;gl_Position=vec4(p*2.0-1.0,0,1);}";
    const char *fragments[] = {
        "#version 410 core\nuniform samplerCubeShadow t[2];uniform int pick;in vec2 uv;out vec4 frag;"
        "float sample_cube(samplerCubeShadow a,vec4 p){return texture(a,p,1.0);}"
        "void main(){vec3 d=normalize(vec3(uv*2.0-1.0,.6));float r=uv.x*.8+.1;"
        "frag=vec4(sample_cube(t[pick],vec4(d,r)),sample_cube(t[1-pick],vec4(d.zxy,r)),"
        "sample_cube(t[pick],vec4(-d.yzx,r)),1);}",
        "#version 410 core\nuniform samplerCubeShadow t[2];uniform int pick;in vec2 uv;out vec4 frag;"
        "void main(){vec3 d=normalize(vec3(uv*2.0-1.0,.6));float r=uv.x*.8+.1;"
        "frag=vec4(texture(t[pick],vec4(d,r)),texture(t[1-pick],vec4(d.zxy,r)),"
        "texture(t[pick],vec4(-d.yzx,r)),1);}",
        "#version 410 core\nuniform samplerCubeShadow t[2];uniform int pick;in vec2 uv;out vec4 frag;"
        "float sample_cube(samplerCubeShadow a,vec4 p,vec3 dx,vec3 dy){return textureGrad(a,p,dx,dy);}"
        "void main(){vec3 d=normalize(vec3(uv*2.0-1.0,.6));float r=uv.x*.8+.1;"
        "vec3 x=dFdx(d)*4.0,y=dFdy(d)*4.0;"
        "frag=vec4(sample_cube(t[pick],vec4(d,r),x,y),sample_cube(t[1-pick],vec4(d.zxy,r),x.zxy,y.zxy),"
        "sample_cube(t[pick],vec4(-d.yzx,r),-x.yzx,-y.yzx),1);}"
    };
    GLuint program = glc_program(vertex, fragments[glc_variant / 2], NULL);
    glUseProgram(program);
    GLuint vao, textures[2];
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenTextures(2, textures);
    for (int unit = 0; unit < 2; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_CUBE_MAP, textures[unit]);
        for (int level = 0, size = 16; level < 5; ++level, size >>= 1) {
            for (int face = 0; face < 6; ++face) {
                float depth[256];
                for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x)
                    depth[y * size + x] = (size > 1 ? (float)x / (size - 1) : .5f) * .65f +
                        .025f * face + .003f * y + .018f * level + .16f * unit;
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, level, GL_DEPTH_COMPONENT32F,
                             size, size, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
            }
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL, unit);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, 4);
        glTexParameterf(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_LOD_BIAS, .75f * unit);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    }
    GLint units[] = {0, 1};
    glUniform1iv(glGetUniformLocation(program, "t[0]"), 2, units);
    GLint pick = glGetUniformLocation(program, "pick");
    if (glc_variant & 1) glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    else glDisable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    /* Uniform selection is dynamic but remains dynamically uniform, as
       GLSL 4.10 requires for indexing arrays of opaque sampler objects. */
    int half = glc_width / 2;
    for (int tile = 0; tile < 2; ++tile) {
        glViewport(tile * half, 0, tile ? glc_width - half : half, glc_height);
        glUniform1i(pick, tile);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glViewport(0, 0, glc_width, glc_height);
    glDisable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    glActiveTexture(GL_TEXTURE0);
    glDeleteTextures(2, textures);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
}
