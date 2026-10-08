#include <cstdint>
#include <cstring>
#include <cstdio>
#include <initializer_list>

// Derive a normal binary32 reciprocal directly from its rational significand.
// No host floating arithmetic is involved in the candidate algorithm.
static bool reciprocal_bits(uint32_t bits, uint32_t &result)
{
    const unsigned encoded_exponent = (bits >> 23) & 255;
    if (!encoded_exponent || encoded_exponent == 255) return false;
    const uint32_t mantissa = (bits & 0x7fffff) | 0x800000;
    const int exponent = int(encoded_exponent) - 127;
    int reciprocal_exponent;
    uint64_t significand;
    if (mantissa == 0x800000) {
        significand = 0x800000;
        reciprocal_exponent = -exponent;
    } else {
        const uint64_t numerator = uint64_t(1) << 47;
        significand = numerator / mantissa;
        const uint64_t remainder = numerator % mantissa;
        if (remainder * 2 > mantissa || (remainder * 2 == mantissa && (significand & 1))) ++significand;
        reciprocal_exponent = -exponent - 1;
        if (significand == 0x1000000) { significand >>= 1; ++reciprocal_exponent; }
    }
    if (reciprocal_exponent < -126 || reciprocal_exponent > 127) return false;
    result = (bits & 0x80000000) | (uint32_t(reciprocal_exponent + 127) << 23) |
             (uint32_t(significand) & 0x7fffff);
    return true;
}
static uint64_t compared = 0;
static bool check(uint32_t bits)
{
    uint32_t actual;
    if (!reciprocal_bits(bits, actual)) return true;
    ++compared;
    float denominator;
    std::memcpy(&denominator, &bits, 4);
    volatile float one = 1;
    const float reciprocal = one / denominator;
    uint32_t expected;
    std::memcpy(&expected, &reciprocal, 4);
    if (actual == expected) return true;
    std::printf("mismatch denominator=%08x actual=%08x expected=%08x\n", bits, actual, expected);
    return false;
}
int main()
{
    unsigned long long count = 0;
    for (unsigned exponent : {1u, 127u, 253u})
        for (uint32_t mantissa = 0; mantissa < 0x800000; ++mantissa) {
            const uint32_t bits = (exponent << 23) | mantissa;
            if (!check(bits) || !check(bits | 0x80000000)) return 1;
            count += 2;
        }
    uint32_t state = 0x6d5a123f;
    for (int i = 0; i < 2000000; ++i) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        if (!check(state)) return 1;
        ++count;
    }
    for (uint32_t exceptional : {0u, 0x80000000u, 1u, 0x007fffffu, 0x7f800000u, 0x7fc00000u, 0x7f7fffffu}) {
        uint32_t unused;
        if (reciprocal_bits(exceptional, unused)) return 2;
    }
    std::printf("reciprocal-bit CPU checks passed: %llu inputs, %llu normal reciprocal comparisons\n", count, static_cast<unsigned long long>(compared));
}
