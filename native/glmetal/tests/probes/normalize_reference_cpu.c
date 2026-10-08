#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t bits(float value)
{
    uint32_t result;
    memcpy(&result, &value, sizeof result);
    return result;
}

int main(void)
{
    const int pixels[][2] = {{37, 43}, {41, 20}, {41, 43}};
    /* Recorded native GLSL normalized directions from the float diagnostic. */
    const uint32_t expected[][3] = {
        {0x3efdc060, 0x3d9f2be0, 0x3f5d74b2},
        {0x3f33b3fd, 0xbd82856c, 0x3f35983e},
        {0x3f33b3fd, 0x3d82856c, 0x3f35983e}
    };
    for (int i = 0; i < 3; ++i) {
        float d[] = {((pixels[i][0] + .5f) / 64) * 4 - 2,
                     ((pixels[i][1] + .5f) / 64 - .5f) * .3f, .6f};
        float xx = d[0] * d[0], yy = d[1] * d[1], zz = d[2] * d[2];
        float dot = (xx + yy) + zz;
        float inverse = (float)(1.0 / sqrt((double)dot));
        for (int j = 0; j < 3; ++j) {
            float result = d[j] * inverse;
            if (bits(result) != expected[i][j]) {
                fprintf(stderr, "Pixel%d component%d got%08x expected%08x\n", i, j, bits(result), expected[i][j]);
                return 1;
            }
        }
    }
    puts("Binary32 dot/rounded reciprocal multiplication matches nine recorded native direction components");
    return 0;
}
