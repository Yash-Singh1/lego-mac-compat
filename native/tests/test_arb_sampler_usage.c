#include "arb_sampler_usage.h"

#include <assert.h>
#include <string.h>

int main(void)
{
    static const char source[] =
        "!!ARBfp1.0\n"
        "TEX R0, fragment.texcoord[0], texture[6], 2D;\n"
        "TXP R1, fragment.texcoord[1], texture[3], CUBE;\n"
        "TEX R2, fragment.texcoord[2], texture[0], 2D;\n"
        "TEX R3, fragment.texcoord[3], texture[31], RECT;\n"
        "END\n";
    struct arb_sampler_usage usage;
    arb_sampler_usage_parse(source, strlen(source), &usage);
    assert(usage.texture_2d == ((1u << 6) | 1u));
    assert(usage.texture_cube == (1u << 3));
    assert(usage.texture_rect == (1u << 31));
    assert(usage.texture_1d == 0);
    assert(usage.texture_3d == 0);

    static const char truncated[] = "texture[6], 2";
    arb_sampler_usage_parse(truncated, sizeof(truncated) - 1, &usage);
    assert(usage.texture_2d == 0);
    return 0;
}
