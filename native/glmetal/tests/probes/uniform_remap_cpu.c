#include "programs.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#include "uniform_remap_impl.h"

static void check(GLenum type, int bytes, int prefix, int matrix_stride)
{
    struct glm_uniform_info from = {.name="value",.type=type,.array_size=1,.offset=prefix,
        .sampler_slot=-1,.matrix_stride=matrix_stride};
    struct glm_uniform_info to = from;
    to.offset = 0;
    struct glm_program source = {0};
    source.result.uniforms = &from; source.result.uniform_count = 1;
    source.result.global_size = prefix + bytes;
    source.globals = malloc((size_t)source.result.global_size);
    memset(source.globals, 0xa5, (size_t)prefix);
    for (int i=0;i<bytes;++i) source.globals[prefix+i]=(unsigned char)(i+1);
    struct glm_compile_result target = {.uniforms=&to,.uniform_count=1,.global_size=bytes};
    unsigned char *output = calloc(1, (size_t)bytes);
    remap_uniforms(&source, &target, output, NULL);
    assert(!memcmp(source.globals+prefix, output, (size_t)bytes));
    free(output); free(source.globals);
}

int main(void)
{
    check(GL_FLOAT,4,0,0);
    check(GL_FLOAT_VEC3,12,16,0);
    check(GL_DOUBLE,8,0,0);
    check(GL_DOUBLE_VEC3,24,32,0);
    check(GL_DOUBLE_VEC4,32,32,0);
    check(GL_DOUBLE_MAT2x3,64,32,32);
    check(GL_DOUBLE_MAT4x2,64,16,16);
    puts("Uniform remapping preserves scalar/vector/matrix bytes within exact allocations");
    return 0;
}
