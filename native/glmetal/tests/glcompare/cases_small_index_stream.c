/* Small orphaned index uploads with frequent storage replacement.
 * Build only: make -j2 glcompare
 * Run separately after game profiling has stopped:
 *   build/glcompare --provider "$PWD/build/libGLMetal.dylib" \
 *     --cases "$PWD/build/libglcases.dylib" --out build/small-index --filter small_index_stream
 * GLM_STREAM_FRAMES defaults to 64 and accepts 1..4096. The report includes
 * CPU submission time and completion time separately. This is a workload
 * benchmark, not a fixed timing threshold. Compare the same provider/arch,
 * frame count and command-thread configuration before and after changes.
 */
#include "glc_gl_core.h"
#include "glcompare.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { STREAM_TILES = 512, STREAM_WIDTH = 64, STREAM_HEIGHT = 32 };
static const char *const stream_variants[] = {"data", "orphan_subdata"};

static double stream_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec * 1e-9;
}

static unsigned stream_frames(void)
{
    const char *value = getenv("GLM_STREAM_FRAMES");
    if (!value) return 64;
    char *end = NULL;
    errno = 0;
    unsigned long frames = strtoul(value, &end, 10);
    if (errno || end == value || *end || frames < 1 || frames > 4096)
        glc_fail("GLM_STREAM_FRAMES must be an integer in 1..4096");
    return (unsigned)frames;
}

static void stream_frame(unsigned frame, GLuint buffer, const GLuint samplers[2])
{
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
    for (unsigned draw = 0; draw < STREAM_TILES; ++draw) {
        /* The odd multiplier visits every tile. Frame rotation also keeps
         * accidental reuse from producing an unchanged frame by coincidence. */
        unsigned tile = (draw * 73 + frame * 31) % STREAM_TILES;
        GLuint base = tile * 4;
        GLuint indices[] = {base, base + 1, base + 2, base + 2, base + 1, base + 3};
        if (glc_variant == 1) {
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, NULL, GL_STREAM_DRAW);
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof indices, indices);
        } else {
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, indices, GL_STREAM_DRAW);
        }
        unsigned state = (tile + frame) & 1;
        /* Mutate both objects after prior draws have referenced them. The
         * sampling state for those draws must survive these later mutations. */
        GLuint sampler = samplers[(draw >> 1) & 1];
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, state ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glBindSampler(0, sampler);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL);
    }
    glFlush();
}

GLC_CASE_VARIANTS(core_small_index_stream, stream_variants, .profile = GLC_CORE,
                  .tolerance = 0)
{
    unsigned frames = stream_frames();
    GLuint program = glc_program(
        "#version 410 core\nvoid main(){"
        "int tile=gl_VertexID/4;int corner=gl_VertexID%4;"
        "vec2 p=vec2(tile%32,tile/32)+vec2(corner&1,corner>>1);"
        "gl_Position=vec4(p/vec2(32,16)*2-1,0,1);}",
        "#version 410 core\nuniform sampler2D image;out vec4 color;"
        "void main(){color=texture(image,vec2(1.25,.5));}", NULL);
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "image"), 0);
    GLuint vao, buffer, texture, samplers[2];
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &buffer);
    glGenTextures(1, &texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    /* Keep the texture complete even on implementations that decide
     * completeness before applying the bound sampler override. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    const unsigned char texels[] = {255, 0, 0, 255, 0, 255, 0, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glGenSamplers(2, samplers);
    for (unsigned i = 0; i < 2; ++i) {
        glSamplerParameteri(samplers[i], GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glSamplerParameteri(samplers[i], GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glSamplerParameteri(samplers[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DITHER);
    glViewport(0, 0, STREAM_WIDTH, STREAM_HEIGHT);
    /* Compile pipelines and populate caches before measuring. */
    stream_frame(0, buffer, samplers);
    glFinish();
    double start = stream_seconds();
    for (unsigned frame = 0; frame < frames; ++frame)
        stream_frame(frame, buffer, samplers);
    double submitted = stream_seconds();
    glFinish();
    double completed = stream_seconds();
    unsigned char pixels[STREAM_WIDTH * STREAM_HEIGHT * 4];
    glReadPixels(0, 0, STREAM_WIDTH, STREAM_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    GLenum error = glGetError();
    if (error) glc_fail("Small indexed stream GL error 0x%x", error);
    for (unsigned tile = 0; tile < STREAM_TILES; ++tile) {
        unsigned state = (tile + frames - 1) & 1;
        for (unsigned dy = 0; dy < 2; ++dy) for (unsigned dx = 0; dx < 2; ++dx) {
            unsigned x = (tile % 32) * 2 + dx, y = (tile / 32) * 2 + dy;
            const unsigned char *pixel = pixels + (y * STREAM_WIDTH + x) * 4;
            if (pixel[0] != (state ? 0 : 255) || pixel[1] != (state ? 255 : 0) ||
                pixel[2] != 0 || pixel[3] != 255)
                glc_fail("Index/sampler version lost at tile %u pixel %u,%u: %u,%u,%u,%u",
                         tile, x, y, pixel[0], pixel[1], pixel[2], pixel[3]);
        }
    }
    fprintf(stderr, "small_index_stream/%s: frames=%u draws=%u bytes_per_upload=24 "
            "submission_ms=%.3f completion_ms=%.3f completed_frames_per_second=%.2f\n",
            stream_variants[glc_variant], frames, frames * STREAM_TILES,
            (submitted - start) * 1000, (completed - start) * 1000,
            frames / (completed - start));
    glBindSampler(0, 0);
    glDeleteSamplers(2, samplers);
    glDeleteTextures(1, &texture);
    glDeleteBuffers(1, &buffer);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
}
