/* Regression for SIMD corruption on first-time Rosetta translation.
 * Build with: xcrun clang -arch x86_64 -O0 test_rosettax87_cold.c -o test_rosettax87_cold
 * Run normally and through the patched runtime_loader; repeat with argument
 * "sse" to reproduce without executing any x87 instructions.
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

__attribute__((noinline)) static double x87_value(void)
{
    double value;
    __asm__ volatile(
        "fld1; fld1; faddp; fld1; fadd %%st(1), %%st(0); "
        "fstpl %0; fstp %%st(0)"
        : "=m"(value) : : "st", "st(1)");
    return value;
}

__attribute__((noinline)) static uint64_t cold_sink(double value)
{
    union { double d; uint64_t u; } bits = { .d = value };
    return bits.u;
}

int main(int argc, char **argv)
{
    (void)argv;
    uint64_t first = cold_sink(argc == 1 ? x87_value() : 3.0);
    uint64_t second = cold_sink(argc == 1 ? x87_value() : 3.0);
    int failed = first != UINT64_C(0x4008000000000000) || second != first;
    printf("%s cold call: first=%016" PRIx64 " second=%016" PRIx64 " %s\n",
           argc == 1 ? "x87" : "SSE", first, second, failed ? "FAIL" : "PASS");
    return failed;
}
