#ifndef GLM_SHADER_FP64_ARITHMETIC_H
#define GLM_SHADER_FP64_ARITHMETIC_H

#include <sstream>
#include <string>

/* Binary64 arithmetic built from integer significands. This is separate from
   the compiler selection code so each primitive can be tested before adoption.
   All intermediate values fit in Metal's 64-bit ulong. */
inline std::string glm_fp64_arithmetic_helpers()
{
    std::ostringstream out;
    out << R"MSL(
inline bool glm_fp64_arith_nan(ulong a) { return (a & 0x7fffffffffffffffUL) > 0x7ff0000000000000UL; }
inline ulong glm_fp64_arith_quiet(ulong a) { return a | 0x0008000000000000UL; }
inline ulong glm_fp64_arith_shr_jam(ulong value, uint shift) {
    if (shift == 0u) return value;
    if (shift >= 64u) return value != 0UL ? 1UL : 0UL;
    return (value >> shift) | ((value & ((1UL << shift) - 1UL)) != 0UL ? 1UL : 0UL);
}
// Produce a normalized 53-bit significand and its unbiased exponent.
inline void glm_fp64_arith_decode(ulong magnitude, thread ulong& significand, thread int& exponent) {
    uint biased = uint(magnitude >> 52);
    significand = magnitude & 0x000fffffffffffffUL;
    exponent = biased != 0u ? int(biased) - 1023 : -1022;
    if (biased != 0u) significand |= 0x0010000000000000UL;
    else while ((significand & 0x0010000000000000UL) == 0UL) { significand <<= 1; --exponent; }
}
// The low three bits contain guard, round, and sticky information.
inline ulong glm_fp64_arith_pack(ulong sign, int exponent, ulong extended) {
    if (extended == 0UL) return sign;
    if (exponent < -1022) {
        extended = glm_fp64_arith_shr_jam(extended, uint(-1022 - exponent));
        exponent = -1022;
    }
    ulong significand = extended >> 3;
    ulong remainder = extended & 7UL;
    if (remainder > 4UL || (remainder == 4UL && (significand & 1UL) != 0UL)) ++significand;
    if (significand >= 0x0020000000000000UL) { significand >>= 1; ++exponent; }
    if (exponent > 1023) return sign | 0x7ff0000000000000UL;
    if (significand < 0x0010000000000000UL) return sign | significand;
    return sign | (ulong(exponent + 1023) << 52) | (significand & 0x000fffffffffffffUL);
}
inline ulong glm_fp64_add(ulong a, ulong b) {
    if (glm_fp64_arith_nan(a)) return glm_fp64_arith_quiet(a);
    if (glm_fp64_arith_nan(b)) return glm_fp64_arith_quiet(b);
    ulong am = a & 0x7fffffffffffffffUL, bm = b & 0x7fffffffffffffffUL;
    ulong as = a & 0x8000000000000000UL, bs = b & 0x8000000000000000UL;
    if (am == 0x7ff0000000000000UL || bm == 0x7ff0000000000000UL) {
        if (am == bm && as != bs) return 0x7ff8000000000000UL;
        return am == 0x7ff0000000000000UL ? a : b;
    }
    if (am == 0UL && bm == 0UL) return as & bs;
    if (am == 0UL) return b;
    if (bm == 0UL) return a;
    ulong ax, bx; int ae, be;
    glm_fp64_arith_decode(am,ax,ae); glm_fp64_arith_decode(bm,bx,be);
    // Keep the larger magnitude first so subtraction never wraps.
    if (ae < be || (ae == be && ax < bx)) {
        ulong swap_x = ax; ax = bx; bx = swap_x;
        ulong swap_s = as; as = bs; bs = swap_s;
        int swap_e = ae; ae = be; be = swap_e;
    }
    ax <<= 3;
    bx = glm_fp64_arith_shr_jam(bx << 3, uint(ae - be));
    ulong result;
    if (as == bs) {
        result = ax + bx;
        if ((result & 0x0100000000000000UL) != 0UL) {
            result = glm_fp64_arith_shr_jam(result,1u); ++ae;
        }
    } else {
        result = ax - bx;
        if (result == 0UL) return 0UL;
        while ((result & 0x0080000000000000UL) == 0UL) { result <<= 1; --ae; }
    }
    return glm_fp64_arith_pack(as,ae,result);
}
inline ulong glm_fp64_sub(ulong a, ulong b) {
    if (glm_fp64_arith_nan(a)) return glm_fp64_arith_quiet(a);
    if (glm_fp64_arith_nan(b)) return glm_fp64_arith_quiet(b);
    return glm_fp64_add(a,b ^ 0x8000000000000000UL);
}
// Exact 53 by 53 bit multiplication in two words, using four 32-bit products.
inline void glm_fp64_arith_product(ulong a, ulong b, thread ulong& high, thread ulong& low) {
    ulong a0 = a & 0xffffffffUL, a1 = a >> 32;
    ulong b0 = b & 0xffffffffUL, b1 = b >> 32;
    ulong p0 = a0 * b0, p1 = a0 * b1, p2 = a1 * b0, p3 = a1 * b1;
    low = p0;
    high = p3 + (p1 >> 32) + (p2 >> 32);
    ulong old = low; low += p1 << 32; if (low < old) ++high;
    old = low; low += p2 << 32; if (low < old) ++high;
}
inline ulong glm_fp64_mul(ulong a, ulong b) {
    if (glm_fp64_arith_nan(a)) return glm_fp64_arith_quiet(a);
    if (glm_fp64_arith_nan(b)) return glm_fp64_arith_quiet(b);
    ulong am = a & 0x7fffffffffffffffUL, bm = b & 0x7fffffffffffffffUL;
    ulong sign = (a ^ b) & 0x8000000000000000UL;
    bool ai = am == 0x7ff0000000000000UL, bi = bm == 0x7ff0000000000000UL;
    if (ai || bi) {
        if (am == 0UL || bm == 0UL) return 0x7ff8000000000000UL;
        return sign | 0x7ff0000000000000UL;
    }
    if (am == 0UL || bm == 0UL) return sign;
    ulong ax, bx; int ae, be;
    glm_fp64_arith_decode(am,ax,ae); glm_fp64_arith_decode(bm,bx,be);
    ulong high, low; glm_fp64_arith_product(ax,bx,high,low);
    bool carry = (high & 0x0000020000000000UL) != 0UL;
    uint shift = carry ? 50u : 49u;
    ulong extended = (high << (64u - shift)) | (low >> shift);
    if ((low & ((1UL << shift) - 1UL)) != 0UL) extended |= 1UL;
    return glm_fp64_arith_pack(sign,ae + be + (carry ? 1 : 0),extended);
}
inline ulong glm_fp64_div(ulong a, ulong b) {
    if (glm_fp64_arith_nan(a)) return glm_fp64_arith_quiet(a);
    if (glm_fp64_arith_nan(b)) return glm_fp64_arith_quiet(b);
    ulong am = a & 0x7fffffffffffffffUL, bm = b & 0x7fffffffffffffffUL;
    ulong sign = (a ^ b) & 0x8000000000000000UL;
    bool ai = am == 0x7ff0000000000000UL, bi = bm == 0x7ff0000000000000UL;
    if ((ai && bi) || (am == 0UL && bm == 0UL)) return 0x7ff8000000000000UL;
    if (ai || bm == 0UL) return sign | 0x7ff0000000000000UL;
    if (bi || am == 0UL) return sign;
    ulong ax, bx; int ae, be;
    glm_fp64_arith_decode(am,ax,ae); glm_fp64_arith_decode(bm,bx,be);
    int exponent = ae - be;
    if (ax < bx) { ax <<= 1; --exponent; }
    // The quotient is in [1,2). Generate its55 fractional bits exactly.
    ulong remainder = ax - bx, quotient = 1UL;
    for (uint bit = 0u; bit < 55u; ++bit) {
        remainder <<= 1; quotient <<= 1;
        if (remainder >= bx) { remainder -= bx; quotient |= 1UL; }
    }
    if (remainder != 0UL) quotient |= 1UL;
    return glm_fp64_arith_pack(sign,exponent,quotient);
}
inline ulong glm_fp64_sqrt(ulong a) {
    if (glm_fp64_arith_nan(a)) return glm_fp64_arith_quiet(a);
    ulong magnitude = a & 0x7fffffffffffffffUL;
    if (magnitude == 0UL) return a;
    if ((a & 0x8000000000000000UL) != 0UL) return 0x7ff8000000000000UL;
    if (magnitude == 0x7ff0000000000000UL) return a;
    ulong significand; int exponent;
    glm_fp64_arith_decode(magnitude,significand,exponent);
    if ((exponent & 1) != 0) { significand <<= 1; --exponent; }
    // sqrt(significand *2^58) has56 bits, including three rounding bits.
    ulong high = significand >> 6, low = significand << 58;
    ulong root = 0UL, remainder = 0UL;
    for (int pair = 55; pair >= 0; --pair) {
        uint shift = uint(pair * 2);
        ulong digit = shift >= 64u ? (high >> (shift - 64u)) & 3UL : (low >> shift) & 3UL;
        remainder = (remainder << 2) | digit;
        ulong trial = (root << 2) | 1UL;
        root <<= 1;
        if (remainder >= trial) { remainder -= trial; root |= 1UL; }
    }
    if (remainder != 0UL) root |= 1UL;
    return glm_fp64_arith_pack(0UL,exponent / 2,root);
}
struct glm_fp64_arith_wide { ulong high; ulong low; };
inline glm_fp64_arith_wide glm_fp64_arith_wide_shr(glm_fp64_arith_wide value, uint shift) {
    if (shift == 0u) return value;
    if (shift >= 128u) return glm_fp64_arith_wide{0UL, (value.high != 0UL || value.low != 0UL) ? 1UL : 0UL};
    bool lost;
    if (shift < 64u) {
        lost = (value.low & ((1UL << shift) - 1UL)) != 0UL;
        value.low = (value.low >> shift) | (value.high << (64u - shift));
        value.high >>= shift;
    } else if (shift == 64u) {
        lost = value.low != 0UL;
        value.low = value.high; value.high = 0UL;
    } else {
        uint extra = shift - 64u;
        lost = value.low != 0UL || (value.high & ((1UL << extra) - 1UL)) != 0UL;
        value.low = value.high >> extra; value.high = 0UL;
    }
    if (lost) value.low |= 1UL;
    return value;
}
inline ulong glm_fp64_fma(ulong a, ulong b, ulong c) {
    if (glm_fp64_arith_nan(a)) return glm_fp64_arith_quiet(a);
    if (glm_fp64_arith_nan(b)) return glm_fp64_arith_quiet(b);
    if (glm_fp64_arith_nan(c)) return glm_fp64_arith_quiet(c);
    ulong am = a & 0x7fffffffffffffffUL, bm = b & 0x7fffffffffffffffUL, cm = c & 0x7fffffffffffffffUL;
    ulong ps = (a ^ b) & 0x8000000000000000UL, cs = c & 0x8000000000000000UL;
    bool ai = am == 0x7ff0000000000000UL, bi = bm == 0x7ff0000000000000UL;
    if (ai || bi) {
        if (am == 0UL || bm == 0UL) return 0x7ff8000000000000UL;
        if (cm == 0x7ff0000000000000UL && ps != cs) return 0x7ff8000000000000UL;
        return ps | 0x7ff0000000000000UL;
    }
    if (cm == 0x7ff0000000000000UL) return c;
    if (am == 0UL || bm == 0UL) return glm_fp64_add(ps,c);
    // With a zero addend, the single correctly rounded product is already fused.
    if (cm == 0UL) return glm_fp64_mul(a,b);
    ulong ax, bx, cx; int ae, be, ce;
    glm_fp64_arith_decode(am,ax,ae); glm_fp64_arith_decode(bm,bx,be); glm_fp64_arith_decode(cm,cx,ce);
    glm_fp64_arith_wide product; glm_fp64_arith_product(ax,bx,product.high,product.low);
    bool carry = (product.high & 0x0000020000000000UL) != 0UL;
    int pe = ae + be + (carry ? 1 : 0);
    uint shift = carry ? 21u : 22u;
    product.high = (product.high << shift) | (product.low >> (64u - shift));
    product.low <<= shift;
    glm_fp64_arith_wide addend = {cx << 10, 0UL};
    int exponent;
    if (pe >= ce) { addend = glm_fp64_arith_wide_shr(addend,uint(pe - ce)); exponent = pe; }
    else { product = glm_fp64_arith_wide_shr(product,uint(ce - pe)); exponent = ce; }
    glm_fp64_arith_wide result;
    ulong sign;
    if (ps == cs) {
        result.low = product.low + addend.low;
        result.high = product.high + addend.high + (result.low < product.low ? 1UL : 0UL);
        sign = ps;
        if ((result.high & 0x8000000000000000UL) != 0UL) {
            result = glm_fp64_arith_wide_shr(result,1u); ++exponent;
        }
    } else {
        bool product_smaller = product.high < addend.high ||
            (product.high == addend.high && product.low < addend.low);
        if (product_smaller) {
            result.low = addend.low - product.low;
            result.high = addend.high - product.high - (addend.low < product.low ? 1UL : 0UL);
            sign = cs;
        } else {
            result.low = product.low - addend.low;
            result.high = product.high - addend.high - (product.low < addend.low ? 1UL : 0UL);
            sign = ps;
        }
        if (result.high == 0UL && result.low == 0UL) return 0UL;
        while ((result.high & 0x4000000000000000UL) == 0UL) {
            result.high = (result.high << 1) | (result.low >> 63); result.low <<= 1; --exponent;
        }
    }
    ulong extended = result.high >> 7;
    if (result.low != 0UL || (result.high & 127UL) != 0UL) extended |= 1UL;
    return glm_fp64_arith_pack(sign,exponent,extended);
}
)MSL";
    const char *functions[] = {"add", "sub", "mul", "div"};
    for (int lanes = 2; lanes <= 4; ++lanes) {
        std::string type = "ulong" + std::to_string(lanes);
        for (auto function : functions) {
            out << "inline " << type << " glm_fp64_" << function << "(" << type << " a," << type << " b) { return " << type << "(";
            for (int lane = 0; lane < lanes; ++lane)
                out << (lane ? "," : "") << "glm_fp64_" << function << "(a[" << lane << "],b[" << lane << "])";
            out << "); }\n";
        }
        out << "inline " << type << " glm_fp64_fma(" << type << " a," << type << " b," << type << " c) { return " << type << "(";
        for (int lane = 0; lane < lanes; ++lane)
            out << (lane ? "," : "") << "glm_fp64_fma(a[" << lane << "],b[" << lane << "],c[" << lane << "])";
        out << "); }\n";
        out << "inline " << type << " glm_fp64_sqrt(" << type << " a) { return " << type << "(";
        for (int lane = 0; lane < lanes; ++lane)
            out << (lane ? "," : "") << "glm_fp64_sqrt(a[" << lane << "])";
        out << "); }\n";
    }
    return out.str();
}

#endif
