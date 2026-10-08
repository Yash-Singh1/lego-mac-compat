#ifndef GLM_SHADER_FP64_MATRIX_H
#define GLM_SHADER_FP64_MATRIX_H

#include <sstream>
#include <string>

/* Column-major binary64 matrices. ulong vectors have the same alignment and
   column stride as their double GLSL counterparts. Arithmetic helpers must
   precede these declarations in the generated Metal source. */
inline std::string glm_fp64_matrix_helpers()
{
    std::ostringstream out;
    const char *spaces[] = {"thread", "constant", "device"};
    for (int columns = 2; columns <= 4; ++columns) for (int rows = 2; rows <= 4; ++rows) {
        std::string name = "glm_fp64_mat" + std::to_string(columns) + "x" + std::to_string(rows);
        std::string vector = "ulong" + std::to_string(rows);
        out << "struct " << name << " { " << vector << " columns[" << columns << "];\n";
        out << name << "() thread = default;\n";
        out << name << "(";
        for (int c = 0; c < columns; ++c) out << (c ? "," : "") << vector << " c" << c;
        out << ") thread : columns{";
        for (int c = 0; c < columns; ++c) out << (c ? "," : "") << "c" << c;
        out << "} {}\n";
        for (auto space : spaces) {
            if (std::string(space) != "constant")
                out << space << " " << vector << "& operator[](uint c) " << space << " { return columns[c]; }\n";
            out << "const " << space << " " << vector << "& operator[](uint c) const " << space << " { return columns[c]; }\n";
        }
        out << "};\n";
        const char *operations[] = {"add", "sub", "mul"};
        for (auto operation : operations) {
            out << "inline " << name << " glm_fp64_" << operation << "(" << name << " a," << name << " b) { return " << name << "(";
            for (int c = 0; c < columns; ++c)
                out << (c ? "," : "") << "glm_fp64_" << operation << "(a[" << c << "],b[" << c << "])";
            out << "); }\n";
        }
        out << "inline " << name << " glm_fp64_outer(ulong" << rows << " a,ulong" << columns << " b) { return " << name << "(";
        for (int c = 0; c < columns; ++c)
            out << (c ? "," : "") << "glm_fp64_mul(a," << vector << "(b[" << c << "]))";
        out << "); }\n";
    }
    out << R"MSL(
inline ulong glm_fp64_determinant(glm_fp64_mat2x2 a) {
    return glm_fp64_sub(glm_fp64_mul(a[0][0],a[1][1]),glm_fp64_mul(a[1][0],a[0][1]));
}
inline glm_fp64_mat2x2 glm_fp64_inverse(glm_fp64_mat2x2 a) {
    ulong determinant = glm_fp64_determinant(a);
    return glm_fp64_mat2x2(
        ulong2(glm_fp64_div(a[1][1],determinant),glm_fp64_div(a[0][1] ^ 0x8000000000000000UL,determinant)),
        ulong2(glm_fp64_div(a[1][0] ^ 0x8000000000000000UL,determinant),glm_fp64_div(a[0][0],determinant)));
}
)MSL";
    return out.str();
}

#endif
