#ifndef GLM_SHADER_FP64_H
#define GLM_SHADER_FP64_H

#include <spirv_cross/spirv_msl.hpp>
#include <spirv_cross/GLSL.std.450.h>
#include <map>
#include <set>
#include <sstream>
#include <vector>
#include "shader_fp64_arithmetic.h"
#include "shader_fp64_matrix.h"
#include "shader_fp64_conversion.h"

/* Cofactor expressions use only software binary64 arithmetic. The matrix
 * argument is copied once into each helper, so operands with side effects are
 * evaluated once by the caller. */
inline std::string glm_fp64_minor_expression(const std::vector<int> &rows,
                                            const std::vector<int> &columns)
{
    if (rows.size() == 1)
        return "a[" + std::to_string(columns[0]) + "][" + std::to_string(rows[0]) + "]";
    std::vector<int> remaining_rows(rows.begin() + 1, rows.end());
    std::string sum;
    for (size_t i = 0; i < columns.size(); ++i) {
        std::vector<int> remaining_columns = columns;
        remaining_columns.erase(remaining_columns.begin() + i);
        std::string term = "glm_fp64_mul(a[" + std::to_string(columns[i]) + "][" +
            std::to_string(rows[0]) + "]," + glm_fp64_minor_expression(remaining_rows, remaining_columns) + ")";
        if (i == 0) sum = term;
        else sum = std::string(i % 2 ? "glm_fp64_sub(" : "glm_fp64_add(") + sum + "," + term + ")";
    }
    return sum;
}

inline std::string glm_fp64_large_inverse_helpers()
{
    std::ostringstream out;
    for (int n = 3; n <= 4; ++n) {
        std::string type = "glm_fp64_mat" + std::to_string(n) + "x" + std::to_string(n);
        out << "inline " << type << " glm_fp64_inverse(" << type << " a) {\n";
        for (int row = 0; row < n; ++row) for (int column = 0; column < n; ++column) {
            std::vector<int> rows, columns;
            for (int i = 0; i < n; ++i) {
                if (i != row) rows.push_back(i);
                if (i != column) columns.push_back(i);
            }
            out << "ulong cofactor_" << row << "_" << column << " = "
                << glm_fp64_minor_expression(rows, columns);
            if ((row + column) % 2) out << " ^ 0x8000000000000000UL";
            out << ";\n";
        }
        out << "ulong determinant = glm_fp64_mul(a[0][0],cofactor_0_0);\n";
        for (int c = 1; c < n; ++c)
            out << "determinant = glm_fp64_add(determinant,glm_fp64_mul(a[" << c << "][0],cofactor_0_" << c << "));\n";
        out << "return " << type << "(";
        for (int c = 0; c < n; ++c) {
            out << (c ? "," : "") << "ulong" << n << "(";
            for (int r = 0; r < n; ++r)
                out << (r ? "," : "") << "glm_fp64_div(cofactor_" << c << "_" << r << ",determinant)";
            out << ")";
        }
        out << "); }\n";
    }
    return out.str();
}

inline bool glm_fp32_reciprocal_bits(uint32_t bits, uint32_t &result)
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

/* This path keeps IEEE binary64 values as bits. Eligibility is deliberately
   conservative: unsupported floating operations must select the old lowering
   for the entire program, never execute as integer arithmetic. */
inline bool glm_fp64_supported(const std::vector<uint32_t> &words)
{
    using namespace spirv_cross;
    using namespace spv;
    if (words.size() < 5) return false;
    try {
        Compiler reflect(words);
        std::set<uint32_t> double_types, all_types;
        auto contains_double = [&](auto &&self, uint32_t id) -> bool {
            const auto &type = reflect.get_type(id);
            if (type.basetype == SPIRType::Double) return true;
            for (auto member : type.member_types) if (self(self, member)) return true;
            return false;
        };
        bool invalid_layout = false;
        reflect.get_ir().for_each_typed_id<SPIRType>([&](uint32_t id, const SPIRType &type) {
            all_types.insert(id);
            if (contains_double(contains_double, id)) double_types.insert(id);
            if (type.basetype == SPIRType::Double &&
                (type.columns > 4 || type.vecsize > 4 || (type.columns > 1 && type.vecsize < 2))) invalid_layout = true;
            for (uint32_t member = 0; member < type.member_types.size(); ++member) {
                const auto &field = reflect.get_type(type.member_types[member]);
                if (field.basetype == SPIRType::Double && field.columns > 1 &&
                    reflect.has_member_decoration(id, member, DecorationRowMajor)) invalid_layout = true;
            }
        });
        reflect.get_ir().for_each_typed_id<SPIRVariable>([&](uint32_t, const SPIRVariable &var) {
            if (!double_types.count(var.basetype)) return;
            switch (var.storage) {
            case StorageClassFunction: case StorageClassPrivate: case StorageClassUniform:
            case StorageClassOutput: break;
            default: invalid_layout = true; break;
            }
        });
        if (invalid_layout) return false;
        std::map<uint32_t, uint32_t> result_types;
        std::set<uint32_t> glsl_imports;
        for (size_t at = 5; at < words.size();) {
            uint32_t n = words[at] >> 16;
            Op opcode = static_cast<Op>(words[at] & 0xffff);
            if (!n || at + n > words.size()) return false;
            /* Result-producing instructions start with their result type.
               Type declarations and annotations also mention types first,
               but do not define typed values. Exclude those explicitly. */
            bool declaration = opcode >= OpTypeVoid && opcode <= OpTypeForwardPointer;
            bool annotation = opcode == OpName || opcode == OpMemberName || opcode == OpDecorate ||
                opcode == OpMemberDecorate || opcode == OpSource || opcode == OpSourceExtension ||
                opcode == OpEntryPoint || opcode == OpExecutionMode || opcode == OpString ||
                opcode == OpExtInstImport || opcode == OpLine;
            if (!declaration && !annotation && n >= 3 && all_types.count(words[at + 1]))
                result_types[words[at + 2]] = words[at + 1];
            if (opcode == OpExtInstImport && n > 2) {
                std::string name;
                for (uint32_t w = 2; w < n; ++w)
                    for (int byte = 0; byte < 4; ++byte) {
                        char c = char(words[at + w] >> (byte * 8));
                        if (!c) break;
                        name += c;
                    }
                if (name == "GLSL.std.450") glsl_imports.insert(words[at + 1]);
            }
            at += n;
        }
        for (size_t at = 5; at < words.size();) {
            uint32_t n = words[at] >> 16;
            Op opcode = static_cast<Op>(words[at] & 0xffff);
            auto double_value = [&](uint32_t id) {
                auto found = result_types.find(id);
                return found != result_types.end() && double_types.count(found->second) != 0;
            };
            bool declaration = opcode >= OpTypeVoid && opcode <= OpTypeForwardPointer;
            bool metadata = declaration || opcode == OpName || opcode == OpMemberName ||
                opcode == OpDecorate || opcode == OpMemberDecorate || opcode == OpSource ||
                opcode == OpSourceExtension || opcode == OpEntryPoint || opcode == OpExecutionMode ||
                opcode == OpString || opcode == OpExtInstImport || opcode == OpLine || opcode == OpNoLine ||
                opcode == OpCapability || opcode == OpExtension || opcode == OpMemoryModel;
            if (metadata) { at += n; continue; }
            bool typed = n >= 3 && all_types.count(words[at + 1]);
            bool involved = typed && double_types.count(words[at + 1]) != 0;
            auto matrix_type = [&](uint32_t id) {
                const auto &type = reflect.get_type(id);
                return type.basetype == SPIRType::Double && type.columns > 1;
            };
            bool matrix_involved = typed && matrix_type(words[at + 1]);
            /* Operand roles matter: constant payloads, composite indices,
               storage classes and extended opcode numbers are literals, not IDs. */
            auto value = [&](uint32_t operand) {
                if (operand < n) {
                    involved |= double_value(words[at + operand]);
                    auto found = result_types.find(words[at + operand]);
                    matrix_involved |= found != result_types.end() && matrix_type(found->second);
                }
            };
            switch (opcode) {
            case OpConstant: case OpConstantNull: case OpSpecConstant:
            case OpFunctionParameter: case OpUndef: break;
            case OpVariable: value(4); break;
            case OpLoad: value(3); break;
            case OpStore: case OpCopyMemory: case OpCopyMemorySized:
                value(1); value(2); break;
            case OpArrayLength: case OpCompositeExtract: value(3); break;
            case OpCompositeInsert: case OpVectorShuffle: value(3); value(4); break;
            case OpFunction: break;
            case OpExtInst:
                for (uint32_t i = 5; i < n; ++i) value(i);
                break;
            case OpSpecConstantOp:
                for (uint32_t i = 4; i < n; ++i) value(i);
                break;
            case OpSwitch: value(1); break;
            case OpBranch: case OpSelectionMerge: case OpLoopMerge:
            case OpLabel: case OpReturn: case OpFunctionEnd: break;
            default:
                for (uint32_t i = typed ? 3u : 1u; i < n; ++i) value(i);
                break;
            }
            if (matrix_involved) {
                // Matrix helpers cover structural copies and component arithmetic.
                // Everything else must retain the whole-program fallback.
                switch (opcode) {
                case OpConstant: case OpConstantComposite: case OpConstantNull: case OpUndef:
                case OpVariable: case OpLoad: case OpStore: case OpCopyMemory:
                case OpAccessChain: case OpInBoundsAccessChain:
                case OpCompositeConstruct: case OpCompositeExtract: case OpCompositeInsert:
                case OpCopyObject: case OpPhi: case OpFunction: case OpFunctionParameter:
                case OpFunctionCall: case OpReturnValue: case OpFAdd: case OpFSub: case OpFMul:
                case OpOuterProduct: break;
                case OpExtInst: {
                    if (n < 6 || (words[at + 4] != GLSLstd450Determinant && words[at + 4] != GLSLstd450MatrixInverse)) return false;
                    auto argument = result_types.find(words[at + 5]);
                    if (argument == result_types.end()) return false;
                    const auto &type = reflect.get_type(argument->second);
                    if (type.columns != type.vecsize || type.columns < 2 || type.columns > 4) return false;
                    if (words[at + 4] == GLSLstd450Determinant && type.columns != 2) return false;
                    break;
                }
                default: return false;
                }
            }
            if (involved) {
                switch (opcode) {
                case OpConvertSToF: case OpConvertUToF: {
                    if (n != 4 || !double_types.count(words[at + 1])) return false;
                    auto operand = result_types.find(words[at + 3]);
                    if (operand == result_types.end()) return false;
                    const auto &source = reflect.get_type(operand->second);
                    const auto &target = reflect.get_type(words[at + 1]);
                    if (source.width != 32 || source.columns != 1 || target.columns != 1 ||
                        source.vecsize != target.vecsize ||
                        source.basetype != (opcode == OpConvertSToF ? SPIRType::Int : SPIRType::UInt)) return false;
                    break;
                }
                case OpName: case OpMemberName: case OpDecorate: case OpMemberDecorate:
                case OpTypeFloat: case OpTypeVector: case OpTypeArray: case OpTypeRuntimeArray:
                case OpTypeStruct: case OpTypePointer: case OpTypeFunction:
                case OpConstant: case OpConstantComposite: case OpConstantNull: case OpUndef:
                case OpVariable: case OpLoad: case OpStore: case OpCopyMemory:
                case OpAccessChain: case OpInBoundsAccessChain: case OpArrayLength:
                case OpCompositeConstruct: case OpCompositeExtract: case OpCompositeInsert:
                case OpVectorShuffle: case OpVectorExtractDynamic: case OpVectorInsertDynamic:
                case OpCopyObject: case OpPhi: case OpSelect:
                case OpFunction: case OpFunctionParameter: case OpFunctionCall: case OpReturnValue:
                case OpOuterProduct: case OpFNegate: case OpIsNan: case OpIsInf:
                case OpFAdd: case OpFSub: case OpFMul: case OpFDiv: case OpFMod: case OpVectorTimesScalar: case OpDot:
                case OpFOrdEqual: case OpFOrdNotEqual: case OpFOrdLessThan: case OpFOrdLessThanEqual:
                case OpFOrdGreaterThan: case OpFOrdGreaterThanEqual:
                case OpFUnordEqual: case OpFUnordNotEqual: case OpFUnordLessThan: case OpFUnordLessThanEqual:
                case OpFUnordGreaterThan: case OpFUnordGreaterThanEqual: case OpOrdered: case OpUnordered:
                case OpSource: case OpSourceExtension: case OpEntryPoint: case OpExecutionMode:
                case OpCapability: case OpExtension: case OpMemoryModel: case OpLine: case OpNoLine:
                case OpLabel: case OpBranch: case OpBranchConditional: case OpSelectionMerge:
                case OpLoopMerge: case OpReturn: case OpFunctionEnd: break;
                case OpExtInst: {
                    if (n < 6 || !glsl_imports.count(words[at + 3])) return false;
                    switch (words[at + 4]) {
                    case GLSLstd450FAbs: case GLSLstd450FSign: case GLSLstd450Floor:
                    case GLSLstd450Ceil: case GLSLstd450Trunc: case GLSLstd450Round:
                    case GLSLstd450RoundEven: case GLSLstd450FMin: case GLSLstd450FMax:
                    case GLSLstd450FClamp: case GLSLstd450PackDouble2x32:
                    case GLSLstd450UnpackDouble2x32: case GLSLstd450Step:
                    case GLSLstd450Frexp: case GLSLstd450FrexpStruct: case GLSLstd450Ldexp:
                    case GLSLstd450Modf: case GLSLstd450ModfStruct: case GLSLstd450Determinant: case GLSLstd450MatrixInverse: break;
                    case GLSLstd450Fract: case GLSLstd450FMix: case GLSLstd450Cross:
                    case GLSLstd450FaceForward: case GLSLstd450Reflect: break;
                    case GLSLstd450Sqrt: case GLSLstd450InverseSqrt: case GLSLstd450Length:
                    case GLSLstd450Distance: case GLSLstd450Normalize: case GLSLstd450Refract:
                    case GLSLstd450SmoothStep: case GLSLstd450Fma: break;
                    default: return false;
                    }
                    break;
                }
                default: return false;
                }
            }
            at += n;
        }
        return true;
    } catch (...) { return false; }
}

class GLMCompilerMSL : public spirv_cross::CompilerMSL {
public:
    explicit GLMCompilerMSL(const std::vector<uint32_t> &words, bool enable_fp64 = true) : CompilerMSL(words)
    {
        if (!enable_fp64) return;
        if (!glm_fp64_supported(words)) throw spirv_cross::CompilerError("Unsupported binary64 program.");
        ir.for_each_typed_id<spirv_cross::SPIRType>([&](uint32_t id, spirv_cross::SPIRType &type) {
            if (type.basetype == spirv_cross::SPIRType::Double) {
                original_double_types.insert(id);
                uses_matrices |= type.columns > 1;
                type.basetype = spirv_cross::SPIRType::UInt64;
                type.width = 64;
            }
        });
    }

protected:
    std::set<uint32_t> original_double_types;
    bool uses_matrices = false;
    bool is_double(uint32_t id) const { return original_double_types.count(expression_type_id(id)) != 0; }
    bool is_double_type(uint32_t id) const { return original_double_types.count(id) != 0; }

    static bool fp64_matrix(const spirv_cross::SPIRType &type) {
        return type.basetype == spirv_cross::SPIRType::UInt64 && type.columns > 1;
    }
    std::string type_to_glsl(const spirv_cross::SPIRType &type, uint32_t id = 0) override {
        if (fp64_matrix(type) && !type.pointer) {
            std::string name = "glm_fp64_mat" + std::to_string(type.columns) + "x" + std::to_string(type.vecsize);
            if (type.array.empty() || using_builtin_array()) return name;
            add_spv_func_and_recompile(SPVFuncImplUnsafeArray);
            std::string result, sizes;
            for (uint32_t i = 0; i < type.array.size(); ++i) {
                result += "spvUnsafeArray<";
                sizes += ", " + to_array_size(type, i) + ">";
            }
            return result + name + sizes;
        }
        return CompilerMSL::type_to_glsl(type, id);
    }
    void emit_struct_member(const spirv_cross::SPIRType &parent, uint32_t member, uint32_t index,
                            const std::string &qualifier = "", uint32_t offset = 0) override {
        const auto &type = get<spirv_cross::SPIRType>(member);
        if (!fp64_matrix(type)) { CompilerMSL::emit_struct_member(parent, member, index, qualifier, offset); return; }
        if (has_member_decoration(parent.self, index, spv::DecorationRowMajor))
            throw spirv_cross::CompilerError("Unsupported row-major binary64 matrix.");
        if (has_extended_member_decoration(parent.self, index, spirv_cross::SPIRVCrossDecorationPaddingTarget))
            statement("char _m", index, "_pad[", get_extended_member_decoration(parent.self, index,
                      spirv_cross::SPIRVCrossDecorationPaddingTarget), "];");
        statement(type_to_glsl(type), " ", qualifier, to_member_name(parent, index),
                  member_attribute_qualifier(parent, index), type_to_array_glsl(type, 0), ";");
    }

    void helper_call(uint32_t type, uint32_t id, const char *name, const uint32_t *args, uint32_t count)
    {
        std::string rhs = std::string("glm_fp64_") + name + "(";
        for (uint32_t i = 0; i < count; ++i) rhs += (i ? ", " : "") + to_unpacked_expression(args[i]);
        emit_op(type, id, rhs + ")", false);
        for (uint32_t i = 0; i < count; ++i) inherit_expression_dependencies(id, args[i]);
    }

    void emit_instruction(const spirv_cross::Instruction &instruction) override
    {
        using namespace spv;
        const uint32_t *ops = stream(instruction);
        // Apple lowers constant binary32 division to a rounded reciprocal
        // multiplication, including precise division. Keep precise products
        // isolated so a following add cannot contract with this multiplication.
        if (instruction.op == OpFDiv && !is_double(ops[2])) {
            const auto &type = get<spirv_cross::SPIRType>(ops[0]);
            const auto *constant = maybe_get<spirv_cross::SPIRConstant>(ops[3]);
            if (type.basetype == spirv_cross::SPIRType::Float && type.width == 32 &&
                type.columns == 1 && constant && !constant->specialization) {
                auto reciprocal = *constant;
                bool supported = true;
                for (uint32_t i = 0; i < type.vecsize; ++i) {
                    uint32_t bits;
                    if (constant->m.c[0].id[i] ||
                        !glm_fp32_reciprocal_bits(constant->m.c[0].r[i].u32, bits)) {
                        supported = false;
                        break;
                    }
                    reciprocal.m.c[0].r[i].u32 = bits;
                }
                if (supported) {
                    const std::string left = to_enclosed_expression(ops[2]);
                    const std::string right = constant_expression(reciprocal);
                    const bool precise = msl_options.invariant_float_math || has_legacy_nocontract(ops[0], ops[1]);
                    emit_op(ops[0], ops[1], precise ? "glm_fp32_precise_mul(" + left + ", " + right + ")" :
                            left + " * " + right, should_forward(ops[2]));
                    inherit_expression_dependencies(ops[1], ops[2]);
                    inherit_expression_dependencies(ops[1], ops[3]);
                    return;
                }
            }
        }
        if (instruction.op == OpFDiv && !is_double(ops[2])) {
            const auto &type = get<spirv_cross::SPIRType>(ops[0]);
            if (type.basetype == spirv_cross::SPIRType::Float && type.width == 32 && type.columns == 1) {
                const bool precise = msl_options.invariant_float_math || has_legacy_nocontract(ops[0], ops[1]);
                const std::string function = precise ? "glm_fp32_precise_div" : "glm_fp32_div";
                emit_op(ops[0], ops[1], function + "(" + to_expression(ops[2]) + ", " +
                        to_expression(ops[3]) + ")", false);
                inherit_expression_dependencies(ops[1], ops[2]);
                inherit_expression_dependencies(ops[1], ops[3]);
                return;
            }
        }
        if ((instruction.op == OpConvertSToF || instruction.op == OpConvertUToF) && is_double_type(ops[0])) {
            helper_call(ops[0], ops[1], instruction.op == OpConvertSToF ? "from_int" : "from_uint", ops + 2, 1);
            return;
        }
        const char *helper = nullptr;
        switch (static_cast<Op>(instruction.op)) {
        case OpOuterProduct: helper = "outer"; break;
        case OpFAdd: helper = "add"; break;
        case OpFSub: helper = "sub"; break;
        case OpFMul: helper = "mul"; break;
        case OpFDiv: helper = "div"; break;
        case OpFMod: helper = "mod"; break;
        case OpDot: helper = "dot"; break;
        case OpFNegate: helper = "neg"; break;
        case OpIsNan: helper = "isnan"; break;
        case OpIsInf: helper = "isinf"; break;
        case OpFOrdEqual: helper = "eq"; break;
        case OpFOrdNotEqual: helper = "ne"; break;
        case OpFOrdLessThan: helper = "lt"; break;
        case OpFOrdLessThanEqual: helper = "le"; break;
        case OpFOrdGreaterThan: helper = "gt"; break;
        case OpFOrdGreaterThanEqual: helper = "ge"; break;
        case OpFUnordEqual: helper = "ueq"; break;
        case OpFUnordNotEqual: helper = "une"; break;
        case OpFUnordLessThan: helper = "ult"; break;
        case OpFUnordLessThanEqual: helper = "ule"; break;
        case OpFUnordGreaterThan: helper = "ugt"; break;
        case OpFUnordGreaterThanEqual: helper = "uge"; break;
        case OpOrdered: helper = "ordered"; break;
        case OpUnordered: helper = "unordered"; break;
        default: break;
        }
        if (instruction.op == OpVectorTimesScalar && is_double(ops[2])) {
            emit_op(ops[0], ops[1], "glm_fp64_mul(" + to_unpacked_expression(ops[2]) + ", " +
                    type_to_glsl(get<spirv_cross::SPIRType>(ops[0])) + "(" + to_unpacked_expression(ops[3]) + "))", false);
            inherit_expression_dependencies(ops[1], ops[2]);
            inherit_expression_dependencies(ops[1], ops[3]);
            return;
        }
        if (helper && is_double(ops[2])) {
            uint32_t count = instruction.op == OpFNegate || instruction.op == OpIsNan || instruction.op == OpIsInf ? 1 : 2;
            helper_call(ops[0], ops[1], helper, ops + 2, count);
            return;
        }
        CompilerMSL::emit_instruction(instruction);
    }

    void emit_glsl_op(uint32_t type, uint32_t id, uint32_t operation, const uint32_t *args, uint32_t count) override
    {
        if (operation == GLSLstd450Normalize && get_execution_model() == spv::ExecutionModelFragment) {
            const auto &operand = expression_type(args[0]);
            if (operand.basetype == spirv_cross::SPIRType::Float && operand.vecsize > 1 && operand.columns == 1) {
                // Ordered products/sums match the measured interpolated cube
                // fragment directions. Uniform-input fragment probes differ,
                // so this is not a universal native arithmetic model.
                // Vertex stages retain SPIRV-Cross's existing lowering.
                // SPIRV-Cross's fast::normalize bypasses the strict Metal
                // math option. Native dot also fuses its accumulation;
                // the helper preserves binary32 products and ordered sums.
                emit_unary_func_op(type, id, args[0], "glm_fp32_normalize");
                return;
            }
        }
        if (operation == GLSLstd450FindILsb && !maybe_get<spirv_cross::SPIRConstant>(args[0]) &&
            !maybe_get<spirv_cross::SPIRConstantOp>(args[0])) {
            auto basetype = expression_type(args[0]).basetype;
            emit_unary_func_op_cast(type, id, args[0], "ctz", basetype, basetype);
            return;
        }
        bool involved = is_double_type(type);
        for (uint32_t i = 0; i < count; ++i) involved |= is_double(args[i]);
        if (!involved) { CompilerMSL::emit_glsl_op(type, id, operation, args, count); return; }
        if (operation == GLSLstd450FrexpStruct || operation == GLSLstd450ModfStruct) {
            const auto &result = get<spirv_cross::SPIRType>(type);
            emit_uninitialized_temporary_expression(type, id);
            const char *function = operation == GLSLstd450FrexpStruct ? "frexp" : "modf";
            statement(to_expression(id), ".", to_member_name(result, 0), " = glm_fp64_", function,
                      "(", to_unpacked_expression(args[0]), ", ", to_expression(id), ".", to_member_name(result, 1), ");");
            inherit_expression_dependencies(id, args[0]);
            return;
        }
        if (operation == GLSLstd450Frexp || operation == GLSLstd450Modf) {
            /* A Metal vector component cannot bind to a reference. Always
               pass a local out temporary, then store to the SPIR-V destination. */
            register_call_out_argument(args[1]);
            auto output_type = get_pointee_type_id(expression_type_id(args[1]));
            std::string temporary = "glm_fp64_out_" + std::to_string(id);
            statement(type_to_glsl(get<spirv_cross::SPIRType>(output_type)), " ", temporary, ";");
            const char *function = operation == GLSLstd450Frexp ? "frexp" : "modf";
            emit_op(type, id, std::string("glm_fp64_") + function + "(" + to_unpacked_expression(args[0]) + ", " + temporary + ")", false);
            statement(to_expression(args[1]), " = ", temporary, ";");
            inherit_expression_dependencies(id, args[0]);
            return;
        }
        const char *helper = nullptr;
        uint32_t arity = 1;
        switch (operation) {
        case GLSLstd450Determinant: helper = "determinant"; break;
        case GLSLstd450MatrixInverse: helper = "inverse"; break;
        case GLSLstd450FAbs: helper = "abs"; break;
        case GLSLstd450FSign: helper = "sign"; break;
        case GLSLstd450Floor: helper = "floor"; break;
        case GLSLstd450Ceil: helper = "ceil"; break;
        case GLSLstd450Trunc: helper = "trunc"; break;
        case GLSLstd450Round: helper = "round"; break;
        case GLSLstd450RoundEven: helper = "round_even"; break;
        case GLSLstd450Step: helper = "step"; arity = 2; break;
        case GLSLstd450Ldexp: helper = "ldexp"; arity = 2; break;
        case GLSLstd450Fract: helper = "fract"; break;
        case GLSLstd450FMix: helper = "mix"; arity = 3; break;
        case GLSLstd450Cross: helper = "cross"; arity = 2; break;
        case GLSLstd450FaceForward: helper = "faceforward"; arity = 3; break;
        case GLSLstd450Reflect: helper = "reflect"; arity = 2; break;
        case GLSLstd450Sqrt: helper = "sqrt"; break;
        case GLSLstd450InverseSqrt: helper = "inversesqrt"; break;
        case GLSLstd450Length: helper = "length"; break;
        case GLSLstd450Distance: helper = "distance"; arity = 2; break;
        case GLSLstd450Normalize: helper = "normalize"; break;
        case GLSLstd450Refract: helper = "refract"; arity = 3; break;
        case GLSLstd450SmoothStep: helper = "smoothstep"; arity = 3; break;
        case GLSLstd450Fma: helper = "fma"; arity = 3; break;
        case GLSLstd450FMin: helper = "min"; arity = 2; break;
        case GLSLstd450FMax: helper = "max"; arity = 2; break;
        case GLSLstd450FClamp: helper = "clamp"; arity = 3; break;
        case GLSLstd450PackDouble2x32: helper = "pack"; break;
        case GLSLstd450UnpackDouble2x32: helper = "unpack"; break;
        default: throw spirv_cross::CompilerError("Unsupported operation on binary64 bit storage.");
        }
        helper_call(type, id, helper, args, arity);
    }

    void emit_header() override
    {
        // Apple contracts ordinary GLSL multiply-add expressions. Keep
        // strict NaN/Inf arithmetic, while allowing this single rounding.
        // SPIRV-Cross isolates NoContraction operations in optnone helpers.
        add_pragma_line("#pragma clang fp contract(fast)", false);
        CompilerMSL::emit_header();
        statement("template<typename T> [[clang::optnone]] T glm_fp32_precise_mul(T a, T b) { return a * b; }");
        statement(R"MSL(
// Only normal finite denominators with normal finite reciprocals use Apple's
// reciprocal multiplication. Exceptional denominators retain strict division.
inline float glm_fp32_div(float a, float b) {
    uint magnitude = as_type<uint>(b) & 0x7fffffffu;
    if (magnitude < 0x00800000u || magnitude > 0x7e800000u) return a / b;
    float reciprocal = 1.0f / b;
    return a * reciprocal;
}
[[clang::optnone]] float glm_fp32_precise_div(float a, float b) {
    uint magnitude = as_type<uint>(b) & 0x7fffffffu;
    if (magnitude < 0x00800000u || magnitude > 0x7e800000u) return a / b;
    float reciprocal = 1.0f / b;
    return a * reciprocal;
}
)MSL");
        for (int n = 2; n <= 4; ++n) {
            std::string vector = "float" + std::to_string(n);
            statement("inline ", vector, " glm_fp32_normalize(", vector, " v) {");
            statement("#pragma clang fp contract(off)");
            statement("float length2 = v.x * v.x;");
            for (int component = 1; component < n; ++component) {
                char axis = "xyzw"[component];
                statement("length2 = length2 + v.", axis, " * v.", axis, ";");
            }
            statement("float inverse = rsqrt(length2); return v * inverse;");
            statement("}");
            for (const char *name : {"glm_fp32_div", "glm_fp32_precise_div"}) {
                std::string type = "float" + std::to_string(n);
                std::string declaration = "inline " + type + " " + name + "(" + type + " a, " + type + " b) { return " + type + "(";
                for (int i = 0; i < n; ++i)
                    declaration += (i ? ", " : "") + std::string(name) + "(a[" + std::to_string(i) + "], b[" + std::to_string(i) + "])";
                statement(declaration + "); }");
            }
        }
        if (!original_double_types.empty()) statement(helpers());
        if (uses_matrices) {
            statement(glm_fp64_matrix_helpers());
            statement(glm_fp64_large_inverse_helpers());
        }
    }

    static std::string helpers()
    {
        std::ostringstream out;
        out << glm_fp64_arithmetic_helpers();
        out << glm_fp64_conversion_helpers();
        out << R"MSL(
inline bool glm_fp64_isnan(ulong a) { return (a & 0x7fffffffffffffffUL) > 0x7ff0000000000000UL; }
inline bool glm_fp64_isinf(ulong a) { return (a & 0x7fffffffffffffffUL) == 0x7ff0000000000000UL; }
inline ulong glm_fp64_neg(ulong a) { return a ^ 0x8000000000000000UL; }
inline ulong glm_fp64_abs(ulong a) { return a & 0x7fffffffffffffffUL; }
inline ulong glm_fp64_sign(ulong a) {
    if (glm_fp64_abs(a) == 0UL || glm_fp64_isnan(a)) return 0UL;
    return (a & 0x8000000000000000UL) | 0x3ff0000000000000UL;
}
inline bool glm_fp64_unordered(ulong a, ulong b) { return glm_fp64_isnan(a) || glm_fp64_isnan(b); }
inline bool glm_fp64_ordered(ulong a, ulong b) { return !glm_fp64_unordered(a,b); }
inline bool glm_fp64_eq(ulong a, ulong b) {
    return glm_fp64_ordered(a,b) && (a == b || ((a | b) & 0x7fffffffffffffffUL) == 0UL);
}
inline bool glm_fp64_lt(ulong a, ulong b) {
    if (glm_fp64_unordered(a,b) || glm_fp64_eq(a,b)) return false;
    bool an = (a >> 63) != 0UL, bn = (b >> 63) != 0UL;
    return an != bn ? an : an ? a > b : a < b;
}
inline bool glm_fp64_ne(ulong a, ulong b) { return glm_fp64_ordered(a,b) && !glm_fp64_eq(a,b); }
inline bool glm_fp64_le(ulong a, ulong b) { return glm_fp64_lt(a,b) || glm_fp64_eq(a,b); }
inline bool glm_fp64_gt(ulong a, ulong b) { return glm_fp64_lt(b,a); }
inline bool glm_fp64_ge(ulong a, ulong b) { return glm_fp64_le(b,a); }
inline bool glm_fp64_ueq(ulong a, ulong b) { return glm_fp64_unordered(a,b) || glm_fp64_eq(a,b); }
inline bool glm_fp64_une(ulong a, ulong b) { return glm_fp64_unordered(a,b) || glm_fp64_ne(a,b); }
inline bool glm_fp64_ult(ulong a, ulong b) { return glm_fp64_unordered(a,b) || glm_fp64_lt(a,b); }
inline bool glm_fp64_ule(ulong a, ulong b) { return glm_fp64_unordered(a,b) || glm_fp64_le(a,b); }
inline bool glm_fp64_ugt(ulong a, ulong b) { return glm_fp64_unordered(a,b) || glm_fp64_gt(a,b); }
inline bool glm_fp64_uge(ulong a, ulong b) { return glm_fp64_unordered(a,b) || glm_fp64_ge(a,b); }
inline ulong glm_fp64_min(ulong a, ulong b) { return glm_fp64_lt(b,a) ? b : a; }
inline ulong glm_fp64_max(ulong a, ulong b) { return glm_fp64_lt(a,b) ? b : a; }
inline ulong glm_fp64_clamp(ulong a, ulong lo, ulong hi) { return glm_fp64_min(glm_fp64_max(a,lo),hi); }
// Modes: 0 truncates, 1 floors, 2 ceils, 3 rounds ties away, 4 rounds ties even.
inline ulong glm_fp64_integral(ulong a, uint mode) {
    ulong sign = a & 0x8000000000000000UL;
    ulong magnitude = a & 0x7fffffffffffffffUL;
    int exponent = int((magnitude >> 52) & 0x7ffUL) - 1023;
    if (exponent >= 52 || magnitude == 0UL) return a;
    if (exponent < 0) {
        bool increment = mode == 1u ? sign != 0UL : mode == 2u ? sign == 0UL :
            mode >= 3u ? magnitude > 0x3fe0000000000000UL ||
                (mode == 3u && magnitude == 0x3fe0000000000000UL) : false;
        return sign | (increment ? 0x3ff0000000000000UL : 0UL);
    }
    uint shift = uint(52 - exponent);
    ulong unit = 1UL << shift, mask = unit - 1UL, fraction = magnitude & mask;
    if (fraction == 0UL) return a;
    ulong truncated = magnitude & ~mask;
    bool increment = mode == 1u ? sign != 0UL : mode == 2u ? sign == 0UL :
        mode >= 3u ? fraction > (unit >> 1) ||
            (fraction == (unit >> 1) && (mode == 3u || (truncated & unit) != 0UL)) : false;
    return sign | (truncated + (increment ? unit : 0UL));
}
inline ulong glm_fp64_trunc(ulong a) { return glm_fp64_integral(a,0u); }
inline ulong glm_fp64_floor(ulong a) { return glm_fp64_integral(a,1u); }
inline ulong glm_fp64_ceil(ulong a) { return glm_fp64_integral(a,2u); }
inline ulong glm_fp64_round(ulong a) { return glm_fp64_integral(a,3u); }
inline ulong glm_fp64_round_even(ulong a) { return glm_fp64_integral(a,4u); }
inline ulong glm_fp64_fract(ulong a) { return glm_fp64_sub(a,glm_fp64_floor(a)); }
inline ulong glm_fp64_mix(ulong x, ulong y, ulong a) {
    return glm_fp64_add(glm_fp64_mul(x,glm_fp64_sub(0x3ff0000000000000UL,a)),glm_fp64_mul(y,a));
}
inline ulong glm_fp64_dot(ulong x, ulong y) { return glm_fp64_mul(x,y); }
inline ulong glm_fp64_faceforward(ulong n, ulong i, ulong nref) {
    return glm_fp64_lt(glm_fp64_mul(nref,i),0UL) ? n : glm_fp64_neg(n);
}
inline ulong glm_fp64_reflect(ulong i, ulong n) {
    return glm_fp64_sub(i,glm_fp64_mul(glm_fp64_mul(0x4000000000000000UL,glm_fp64_mul(n,i)),n));
}
inline ulong glm_fp64_inversesqrt(ulong a) { return glm_fp64_div(0x3ff0000000000000UL,glm_fp64_sqrt(a)); }
inline ulong glm_fp64_length(ulong a) { return glm_fp64_abs(a); }
inline ulong glm_fp64_distance(ulong a, ulong b) { return glm_fp64_abs(glm_fp64_sub(a,b)); }
inline ulong glm_fp64_normalize(ulong a) { return glm_fp64_div(a,glm_fp64_abs(a)); }
inline ulong glm_fp64_mod(ulong x, ulong y) { return glm_fp64_sub(x,glm_fp64_mul(y,glm_fp64_floor(glm_fp64_div(x,y)))); }
inline ulong glm_fp64_smoothstep(ulong edge0, ulong edge1, ulong x) {
    ulong t=glm_fp64_clamp(glm_fp64_div(glm_fp64_sub(x,edge0),glm_fp64_sub(edge1,edge0)),0UL,0x3ff0000000000000UL);
    return glm_fp64_mul(glm_fp64_mul(t,t),glm_fp64_sub(0x4008000000000000UL,glm_fp64_mul(0x4000000000000000UL,t)));
}
inline ulong glm_fp64_refract(ulong i, ulong n, ulong eta) {
    ulong d=glm_fp64_mul(n,i);
    ulong k=glm_fp64_sub(0x3ff0000000000000UL,glm_fp64_mul(glm_fp64_mul(eta,eta),glm_fp64_sub(0x3ff0000000000000UL,glm_fp64_mul(d,d))));
    return glm_fp64_lt(k,0UL) ? 0UL : glm_fp64_sub(glm_fp64_mul(eta,i),glm_fp64_mul(glm_fp64_add(glm_fp64_mul(eta,d),glm_fp64_sqrt(k)),n));
}
inline ulong glm_fp64_step(ulong edge, ulong value) { return glm_fp64_lt(value,edge) ? 0UL : 0x3ff0000000000000UL; }
inline ulong glm_fp64_frexp(ulong a, thread int& exponent) {
    ulong sign = a & 0x8000000000000000UL;
    ulong magnitude = a & 0x7fffffffffffffffUL;
    uint biased = uint(magnitude >> 52);
    exponent = 0;
    if (magnitude == 0UL || biased == 0x7ffu) return a;
    ulong significand = magnitude & 0x000fffffffffffffUL;
    if (biased != 0u) exponent = int(biased) - 1022;
    else {
        exponent = -1021;
        while ((significand & 0x0010000000000000UL) == 0UL) { significand <<= 1; --exponent; }
    }
    return sign | 0x3fe0000000000000UL | (significand & 0x000fffffffffffffUL);
}
inline ulong glm_fp64_ldexp(ulong a, int power) {
    ulong sign = a & 0x8000000000000000UL;
    ulong magnitude = a & 0x7fffffffffffffffUL;
    uint biased = uint(magnitude >> 52);
    if (magnitude == 0UL || biased == 0x7ffu) return a;
    ulong significand = magnitude & 0x000fffffffffffffUL;
    long exponent;
    if (biased != 0u) { significand |= 0x0010000000000000UL; exponent = long(biased) - 1023L; }
    else {
        exponent = -1022L;
        while ((significand & 0x0010000000000000UL) == 0UL) { significand <<= 1; --exponent; }
    }
    exponent += long(power);
    if (exponent > 1023L) return sign | 0x7ff0000000000000UL;
    if (exponent >= -1022L)
        return sign | (ulong(exponent + 1023L) << 52) | (significand & 0x000fffffffffffffUL);
    long shift_long = -1022L - exponent;
    if (shift_long > 53L) return sign;
    uint shift = uint(shift_long);
    ulong rounded = significand >> shift;
    ulong fraction = significand & ((1UL << shift) - 1UL);
    ulong halfway = 1UL << (shift - 1u);
    if (fraction > halfway || (fraction == halfway && (rounded & 1UL) != 0UL)) ++rounded;
    // Rounding can carry into the minimum normal exponent, whose encoding is the same bit.
    return sign | rounded;
}
inline ulong glm_fp64_modf(ulong a, thread ulong& integer) {
    // Apple GL emits positive fractional zero, while the integral result retains its sign.
    integer = glm_fp64_trunc(a);
    ulong sign = a & 0x8000000000000000UL;
    ulong magnitude = a & 0x7fffffffffffffffUL;
    if (glm_fp64_isnan(a)) return a;
    int exponent = int(magnitude >> 52) - 1023;
    if (exponent < 0) return magnitude == 0UL ? 0UL : a;
    if (exponent >= 52) return 0UL;
    ulong fraction = magnitude & ((1UL << uint(52 - exponent)) - 1UL);
    if (fraction == 0UL) return 0UL;
    while ((fraction & 0x0010000000000000UL) == 0UL) { fraction <<= 1; --exponent; }
    return sign | (ulong(exponent + 1023) << 52) | (fraction & 0x000fffffffffffffUL);
}
inline ulong glm_fp64_pack(uint2 a) { return ulong(a.x) | (ulong(a.y) << 32); }
inline uint2 glm_fp64_unpack(ulong a) { return uint2(uint(a),uint(a >> 32)); }
)MSL";
        const char *unary[] = {"neg", "abs", "sign", "trunc", "floor", "ceil", "round", "round_even", "isnan", "isinf", "fract", "inversesqrt"};
        const char *binary[] = {"eq", "ne", "lt", "le", "gt", "ge", "ueq", "une", "ult", "ule", "ugt", "uge", "ordered", "unordered", "min", "max", "step"};
        for (int n = 2; n <= 4; ++n) {
            std::string u = "ulong" + std::to_string(n), b = "bool" + std::to_string(n);
            for (int f = 0; f < 12; ++f) {
                std::string result = f == 8 || f == 9 ? b : u;
                out << "inline " << result << " glm_fp64_" << unary[f] << "(" << u << " a) { return " << result << "(";
                for (int i = 0; i < n; ++i) out << (i ? "," : "") << "glm_fp64_" << unary[f] << "(a[" << i << "])";
                out << "); }\n";
            }
            for (int f = 0; f < 17; ++f) {
                std::string result = f >= 14 ? u : b;
                out << "inline " << result << " glm_fp64_" << binary[f] << "(" << u << " a," << u << " b) { return " << result << "(";
                for (int i = 0; i < n; ++i) out << (i ? "," : "") << "glm_fp64_" << binary[f] << "(a[" << i << "],b[" << i << "])";
                out << "); }\n";
            }
            std::string ints = "int" + std::to_string(n);
            out << "inline " << u << " glm_fp64_ldexp(" << u << " a," << ints << " powers) { return " << u << "(";
            for (int i = 0; i < n; ++i) out << (i ? "," : "") << "glm_fp64_ldexp(a[" << i << "],powers[" << i << "])";
            out << "); }\n";
            const char *output_functions[] = {"frexp", "modf"};
            for (int f = 0; f < 2; ++f) {
                std::string output_vector = f == 0 ? ints : u;
                out << "inline " << u << " glm_fp64_" << output_functions[f] << "(" << u
                    << " a, thread " << output_vector << "& whole) { " << u << " result; ";
                for (int i = 0; i < n; ++i)
                    out << (f == 0 ? "int" : "ulong") << " e" << i << "; result[" << i
                        << "] = glm_fp64_" << output_functions[f] << "(a[" << i << "],e" << i << "); ";
                out << "whole = " << output_vector << "(";
                for (int i = 0; i < n; ++i) out << (i ? "," : "") << "e" << i;
                out << "); return result; }\n";
            }
            out << "inline " << u << " glm_fp64_clamp(" << u << " a," << u << " lo," << u << " hi) { return " << u << "(";
            for (int i = 0; i < n; ++i) out << (i ? "," : "") << "glm_fp64_clamp(a[" << i << "],lo[" << i << "],hi[" << i << "])";
            out << "); }\n";
            out << "inline ulong glm_fp64_dot(" << u << " a," << u << " b) { ulong sum=glm_fp64_mul(a[0],b[0]); ";
            for (int i = 1; i < n; ++i)
                out << "sum=glm_fp64_add(sum,glm_fp64_mul(a[" << i << "],b[" << i << "])); ";
            out << "return sum; }\n";
            out << "inline " << u << " glm_fp64_mix(" << u << " x," << u << " y," << u << " a) { return " << u << "(";
            for (int i = 0; i < n; ++i)
                out << (i ? "," : "") << "glm_fp64_mix(x[" << i << "],y[" << i << "],a[" << i << "])";
            out << "); }\n";
            out << "inline " << u << " glm_fp64_mix(" << u << " x," << u << " y,ulong a) { return glm_fp64_mix(x,y," << u << "(a)); }\n";
            out << "inline " << u << " glm_fp64_faceforward(" << u << " n," << u << " i," << u << " nref) { return glm_fp64_lt(glm_fp64_dot(nref,i),0UL) ? n : glm_fp64_neg(n); }\n";
            out << "inline " << u << " glm_fp64_reflect(" << u << " i," << u << " n) { return glm_fp64_sub(i,glm_fp64_mul(" << u << "(glm_fp64_mul(0x4000000000000000UL,glm_fp64_dot(n,i))),n)); }\n";
            out << "inline ulong glm_fp64_length(" << u << " a) { return glm_fp64_sqrt(glm_fp64_dot(a,a)); }\n";
            out << "inline ulong glm_fp64_distance(" << u << " a," << u << " b) { return glm_fp64_length(glm_fp64_sub(a,b)); }\n";
            out << "inline " << u << " glm_fp64_normalize(" << u << " a) { return glm_fp64_div(a," << u << "(glm_fp64_length(a))); }\n";
            out << "inline " << u << " glm_fp64_mod(" << u << " x," << u << " y) { return " << u << "(";
            for (int i = 0; i < n; ++i)
                out << (i ? "," : "") << "glm_fp64_mod(x[" << i << "],y[" << i << "])";
            out << "); }\n";
            out << "inline " << u << " glm_fp64_smoothstep(" << u << " a," << u << " b," << u << " x) { return " << u << "(";
            for (int i = 0; i < n; ++i)
                out << (i ? "," : "") << "glm_fp64_smoothstep(a[" << i << "],b[" << i << "],x[" << i << "])";
            out << "); }\n";
            out << "inline " << u << " glm_fp64_smoothstep(ulong a,ulong b," << u << " x) { return glm_fp64_smoothstep(" << u << "(a)," << u << "(b),x); }\n";
            out << "inline " << u << " glm_fp64_refract(" << u << " i," << u << " n,ulong eta) { ulong d=glm_fp64_dot(n,i); "
                   "ulong k=glm_fp64_sub(0x3ff0000000000000UL,glm_fp64_mul(glm_fp64_mul(eta,eta),glm_fp64_sub(0x3ff0000000000000UL,glm_fp64_mul(d,d)))); "
                   "return glm_fp64_lt(k,0UL) ? " << u << "(0UL) : glm_fp64_sub(glm_fp64_mul(" << u << "(eta),i),glm_fp64_mul(" << u << "(glm_fp64_add(glm_fp64_mul(eta,d),glm_fp64_sqrt(k))),n)); }\n";
        }
        out << R"MSL(
inline ulong3 glm_fp64_cross(ulong3 a, ulong3 b) {
    return ulong3(glm_fp64_sub(glm_fp64_mul(a.y,b.z),glm_fp64_mul(a.z,b.y)),
                  glm_fp64_sub(glm_fp64_mul(a.z,b.x),glm_fp64_mul(a.x,b.z)),
                  glm_fp64_sub(glm_fp64_mul(a.x,b.y),glm_fp64_mul(a.y,b.x)));
}
)MSL";
        return out.str();
    }
};

#endif
