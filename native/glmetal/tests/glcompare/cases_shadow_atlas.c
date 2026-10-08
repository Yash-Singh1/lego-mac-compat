/* Shadow atlas lookup with projective coordinates: explicit LOD zero,
 * projected coordinates, and a shader-side Y flip. No game assets required.
 * Build only: make -j2 glcompare. Run later with --filter shadow_atlas.
 */
#include "glc_gl_core.h"
#include "glcompare.h"
#include <stdio.h>

static const char *const atlas_names[] = {
    "upload_depth24", "render_depth24", "blit_depth24",
    "upload_depth32f", "render_depth32f", "blit_depth32f"
};
static float atlas_depth(unsigned x, unsigned y)
{
    /* Asymmetric in both axes; values are exact binary fractions. */
    return (1 + ((x * 3 + y * 5 + (y > 1 ? 2 : 0)) % 7)) / 8.0f;
}
static void atlas_texture(GLuint texture, GLenum format, const float *data)
{
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, format, 4, 4, 0, GL_DEPTH_COMPONENT, GL_FLOAT, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
}
static void atlas_framebuffer(GLuint framebuffer, GLuint texture)
{
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, texture, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        glc_fail("Shadow atlas depth-only framebuffer incomplete");
}

GLC_CASE_VARIANTS(core_shadow_atlas, atlas_names, .profile = GLC_CORE, .tolerance = 1)
{
    unsigned mode = glc_variant % 3;
    GLenum format = glc_variant < 3 ? GL_DEPTH_COMPONENT24 : GL_DEPTH_COMPONENT32F;
    GLfloat depth[16];
    for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x)
        depth[y * 4 + x] = atlas_depth(x, y);
    GLint original;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &original);
    GLuint textures[2], framebuffers[2], vao;
    glGenTextures(2, textures);
    glGenFramebuffers(2, framebuffers);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glActiveTexture(GL_TEXTURE0);
    atlas_texture(textures[0], format, mode == 0 ? depth : NULL);
    if (mode != 0) {
        atlas_framebuffer(framebuffers[0], textures[0]);
        GLuint depth_program = glc_program(
            "#version 150 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
            "gl_Position=vec4(p*2-1,0,1);}",
            "#version 150 core\nout vec4 unused;void main(){ivec2 p=ivec2(gl_FragCoord.xy);"
            "int value=(p.x*3+p.y*5+(p.y>1?2:0))%7;"
            "gl_FragDepth=float(1+value)/8.;unused=vec4(1);}", NULL);
        glUseProgram(depth_program);
        glViewport(0, 0, 4, 4);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
        glDepthMask(GL_TRUE);
        glClearDepth(1);
        glClear(GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glUseProgram(0);
        glDeleteProgram(depth_program);
        if (mode == 2) {
            atlas_texture(textures[1], format, NULL);
            atlas_framebuffer(framebuffers[1], textures[1]);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffers[0]);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers[1]);
            glBlitFramebuffer(0, 0, 4, 4, 0, 0, 4, 4, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)original);
    glViewport(0, 0, glc_width, glc_height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DITHER);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    GLuint atlas = textures[mode == 2 ? 1 : 0];
    glBindTexture(GL_TEXTURE_2D, atlas);
    GLuint program = glc_program(
        "#version 150 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
        "gl_Position=vec4(p*2-1,0,1);}",
        "#version 150 core\nuniform sampler2DShadow atlas;uniform int pass;out vec4 color;"
        "void main(){ivec2 cell=ivec2(gl_FragCoord.xy)/8;"
        "vec2 uv=(vec2(cell%4)+.5)/4.;float ref;"
        "if(cell.y<4)ref=cell.x<4?.35:.65;else ref=cell.x<4?-.25:1.25;"
        "vec4 q=vec4(uv,ref,0);q.y=1.-q.y;"
        "float lod=textureLod(atlas,q.xyz,0.);"
        "float w=cell.x<4?.5:2.;"
        "float projected=textureProj(atlas,vec4(q.xyz*w,w));"
        "color=vec4(lod,projected,pass==0?0.:1.,1.);}", NULL);
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "atlas"), 0);
    /* Distinct nonsaturating refs verify the reference position; out-of-range
     * refs cover clamping for normalized depth versus float-depth behavior.
     * Against atlas values strictly within (0,1), both rules give same result. */
    unsigned char pixels[64 * 64 * 4];
    for (unsigned pass = 0; pass < 2; ++pass) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, pass ? GL_GREATER : GL_LEQUAL);
        glUniform1i(glGetUniformLocation(program, "pass"), pass);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        GLenum error = glGetError();
        if (error) glc_fail("Shadow atlas %s pass %u GL error 0x%x", atlas_names[glc_variant], pass, error);
        for (unsigned cy = 0; cy < 8; ++cy) for (unsigned cx = 0; cx < 8; ++cx) {
            float ref = cy < 4 ? (cx < 4 ? .35f : .65f) : (cx < 4 ? -.25f : 1.25f);
            float stored = atlas_depth(cx % 4, 3 - cy % 4);
            unsigned char expected = (pass ? ref > stored : ref <= stored) ? 255 : 0;
            const unsigned char *pixel = pixels + ((cy * 8 + 4) * 64 + cx * 8 + 4) * 4;
            if (pixel[0] != expected || pixel[1] != expected || pixel[2] != (pass ? 255 : 0) || pixel[3] != 255)
                glc_fail("Shadow atlas %s %s cell %u,%u ref %.3f depth %.3f: lod=%u proj=%u expected=%u",
                         atlas_names[glc_variant], pass ? "greater" : "lequal", cx, cy, ref, stored,
                         pixel[0], pixel[1], expected);
        }
    }
    glUseProgram(0);
    glDeleteProgram(program);
    glDeleteFramebuffers(2, framebuffers);
    glDeleteTextures(2, textures);
    glDeleteVertexArrays(1, &vao);
}
