#ifndef GLM_SHADER_FP64_CONVERSION_H
#define GLM_SHADER_FP64_CONVERSION_H

#include <sstream>
#include <string>

/* All 32-bit integers are exactly representable by IEEE binary64. Encode
   magnitude and exponent directly, including INT_MIN without signed overflow. */
inline std::string glm_fp64_conversion_scalar_helpers()
{
    return R"MSL(
inline ulong glm_fp64_from_uint(uint a) {
    if (a == 0u) return 0UL;
    uint exponent = 31u - clz(a);
    return (ulong(exponent + 1023u) << 52) |
           ((ulong(a) << (52u - exponent)) & 0x000fffffffffffffUL);
}
inline ulong glm_fp64_from_int(int a) {
    uint magnitude = a < 0 ? 0u - uint(a) : uint(a);
    return glm_fp64_from_uint(magnitude) | (a < 0 ? 0x8000000000000000UL : 0UL);
}
)MSL";
}

inline std::string glm_fp64_conversion_helpers()
{
    std::ostringstream out;
    out << glm_fp64_conversion_scalar_helpers();
    for (int n = 2; n <= 4; ++n) for (const char *type : {"int", "uint"}) {
        out << "inline ulong" << n << " glm_fp64_from_" << type << "(" << type << n
            << " a) { return ulong" << n << "(";
        for (int i = 0; i < n; ++i)
            out << (i ? "," : "") << "glm_fp64_from_" << type << "(a[" << i << "])";
        out << "); }\n";
    }
    return out.str();
}

#endif
