#include "glc_gl_core.h"
#include "glcompare.h"

/* Edge samples retain the original shadow-cube ramp and direction rotations.
   Mip levels use different offsets so an incorrect LOD is also visible. */
static const char *const cube_filter_names[] = {
    "off_nearest", "off_linear", "on_nearest", "on_linear",
    "off_nearest_bias", "off_linear_bias", "on_nearest_bias", "on_linear_bias",
    "off_nearest_grad", "off_linear_grad", "on_nearest_grad", "on_linear_grad",
    "off_nearest_repeat", "off_linear_repeat", "on_nearest_repeat", "on_linear_repeat",
    "off_nearest_bias_repeat", "off_linear_bias_repeat", "on_nearest_bias_repeat", "on_linear_bias_repeat",
    "off_nearest_grad_repeat", "off_linear_grad_repeat", "on_nearest_grad_repeat", "on_linear_grad_repeat",
    "off_nearest_sampler", "off_linear_sampler", "on_nearest_sampler", "on_linear_sampler",
    "off_nearest_bias_sampler", "off_linear_bias_sampler", "on_nearest_bias_sampler", "on_linear_bias_sampler",
    "off_nearest_grad_sampler", "off_linear_grad_sampler", "on_nearest_grad_sampler", "on_linear_grad_sampler"
};

GLC_CASE_VARIANTS(core_cube_shadow_filter, cube_filter_names,
                  .profile = GLC_CORE, .tolerance = 2)
{
    int mode = glc_variant % 12;
    int wrap_variant = glc_variant / 12;
    const char *vertex = "#version 410 core\nout vec2 uv;void main(){"
        "vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
        "uv=p;gl_Position=vec4(p*2.0-1.0,0,1);}";
    const char *fragments[] = {
        "#version 410 core\nuniform samplerCubeShadow t;in vec2 uv;out vec4 frag;"
        "void main(){vec3 d=normalize(vec3(uv*2.0-1.0,0.6));float r=uv.x*0.8+0.1;"
        "frag=vec4(texture(t,vec4(d,r)),texture(t,vec4(d.zxy,r)),texture(t,vec4(-d.yzx,r)),1);}",
        "#version 410 core\nuniform samplerCubeShadow t;in vec2 uv;out vec4 frag;"
        "void main(){vec3 d=normalize(vec3(uv*2.0-1.0,0.6));float r=uv.x*0.8+0.1;"
        "frag=vec4(texture(t,vec4(d,r),2.0),texture(t,vec4(d.zxy,r),2.0),texture(t,vec4(-d.yzx,r),2.0),1);}",
        "#version 410 core\nuniform samplerCubeShadow t;in vec2 uv;out vec4 frag;"
        "void main(){vec3 d=normalize(vec3(uv*2.0-1.0,0.6));float r=uv.x*0.8+0.1;"
        "vec3 x=dFdx(d)*4.0,y=dFdy(d)*4.0;"
        "frag=vec4(textureGrad(t,vec4(d,r),x,y),textureGrad(t,vec4(d.zxy,r),x.zxy,y.zxy),"
        "textureGrad(t,vec4(-d.yzx,r),-x.yzx,-y.yzx),1);}"
    };
    GLuint program = glc_program(vertex, fragments[mode / 4], NULL);
    glUseProgram(program);
    GLuint vao, texture;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenTextures(1, &texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, texture);
    for (int level = 0, size = 16; level < 5; ++level, size >>= 1) {
        for (int face = 0; face < 6; ++face) {
            float depth[256];
            for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x)
                depth[y * size + x] = (size > 1 ? (float)x / (size - 1) : 0.5f) * 0.8f +
                    0.03f * face + 0.004f * y + 0.017f * level;
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, level, GL_DEPTH_COMPONENT32F,
                         size, size, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
        }
    }
    int linear = mode & 1;
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER,
                    mode < 4 ? (linear ? GL_LINEAR : GL_NEAREST) :
                    (linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST));
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, 4);
    /* Group one leaves the texture's default REPEAT untouched. Group two
       binds a REPEAT sampler over a CLAMP_TO_EDGE texture. */
    if (wrap_variant != 1) {
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    GLuint sampler = 0;
    if (wrap_variant == 2) {
        glGenSamplers(1, &sampler);
        glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER,
                            mode < 4 ? (linear ? GL_LINEAR : GL_NEAREST) :
                            (linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST));
        glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, GL_REPEAT);
        glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_COMPARE_FUNC, GL_GREATER);
        glBindSampler(0, sampler);
    }
    if (mode & 2) glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    else glDisable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    glUniform1i(glGetUniformLocation(program, "t"), 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    if (sampler) {
        glBindSampler(0, 0);
        glDeleteSamplers(1, &sampler);
    }
    glDeleteTextures(1, &texture);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
}
