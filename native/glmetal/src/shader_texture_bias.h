// Texture-object LOD bias is not part of a Metal sampler descriptor. Add it
// to SPIR-V sampling operations, after reflection has assigned sampler slots.
// The private uniform block uses the fixed-function slot, which is unused by
// programmable stages. Reflection of the application's program is unchanged.
static std::vector<uint32_t> texture_bias_spirv(const std::vector<uint32_t> &words,
                                               const std::map<std::string, int> &slots)
{
    using namespace spv;
    spirv_cross::Compiler reflect(words);
    struct Image { uint32_t slot; uint32_t index = 0; uint32_t parameter = 0; };
    std::map<uint32_t, Image> images;
    for (const auto &resource : reflect.get_shader_resources().sampled_images) {
        auto slot = slots.find(reflect.get_name(resource.id));
        const auto &type = reflect.get_type(resource.type_id);
        if (slot == slots.end() || slot->second >= 64 || type.image.ms || type.image.dim == DimBuffer ||
            type.image.dim == DimRect || type.image.dim == DimSubpassData) continue;
        images[resource.id] = {static_cast<uint32_t>(slot->second), 0};
    }
    if (images.empty()) return words;
    uint32_t next = words[3], float_type = 0, uint_type = 0, glsl = 0;
    std::map<uint32_t, uint32_t> value_types;
    std::set<uint32_t> types;
    for (size_t at = 5; at < words.size();) {
        uint32_t n = words[at] >> 16, op = words[at] & 0xffff;
        if (!n) return words;
        if (op == OpTypeFloat && words[at + 2] == 32) float_type = words[at + 1];
        if (op == OpTypeInt && words[at + 2] == 32 && words[at + 3] == 0) uint_type = words[at + 1];
        if (op == OpExtInstImport && std::string(reinterpret_cast<const char *>(&words[at + 2])) == "GLSL.std.450")
            glsl = words[at + 1];
        bool type_declaration = op >= OpTypeVoid && op <= OpTypeForwardPointer;
        if (type_declaration) types.insert(words[at + 1]);
        if (!type_declaration && n >= 3 && types.count(words[at + 1]))
            value_types[words[at + 2]] = words[at + 1];
        at += n;
    }
    std::vector<uint32_t> declarations, annotations, names, imports;
    auto emit = [](std::vector<uint32_t> &out, Op op, std::initializer_list<uint32_t> operands) {
        out.push_back((static_cast<uint32_t>(operands.size() + 1) << 16) | op);
        out.insert(out.end(), operands.begin(), operands.end());
    };
    auto name = [&](std::vector<uint32_t> &out, Op op, uint32_t id, const char *text) {
        size_t count = (strlen(text) + 4) / 4, start = out.size();
        out.resize(start + 2 + count, 0);
        out[start] = (static_cast<uint32_t>(2 + count) << 16) | op;
        out[start + 1] = id;
        memcpy(out.data() + start + 2, text, strlen(text));
    };
    if (!float_type) { float_type = next++; emit(declarations, OpTypeFloat, {float_type, 32}); }
    if (!uint_type) { uint_type = next++; emit(declarations, OpTypeInt, {uint_type, 32, 0}); }
    if (!glsl) { glsl = next++; name(imports, OpExtInstImport, glsl, "GLSL.std.450"); }
    std::map<uint32_t, uint32_t> constants;
    auto constant = [&](uint32_t value) {
        auto found = constants.find(value);
        if (found != constants.end()) return found->second;
        uint32_t id = next++;
        emit(declarations, OpConstant, {uint_type, id, value});
        constants[value] = id;
        return id;
    };
    uint32_t zero = constant(0), count = constant(GLM_LOD_ROW_COUNT), array = next++, block = next++, ptr_block = next++, ptr_float = next++;
    uint32_t state = next++, min_bias = next++, max_bias = next++;
    emit(declarations, OpTypeArray, {array, float_type, count});
    emit(declarations, OpTypeStruct, {block, array});
    emit(declarations, OpTypePointer, {ptr_block, StorageClassUniform, block});
    emit(declarations, OpTypePointer, {ptr_float, StorageClassUniform, float_type});
    emit(declarations, OpVariable, {ptr_block, state, StorageClassUniform});
    emit(declarations, OpConstant, {float_type, min_bias, 0xc1800000u}); // -16
    emit(declarations, OpConstant, {float_type, max_bias, 0x41800000u}); // +16
    emit(annotations, OpDecorate, {array, DecorationArrayStride, 16});
    emit(annotations, OpDecorate, {block, DecorationBlock});
    emit(annotations, OpMemberDecorate, {block, 0, DecorationOffset, 0});
    emit(annotations, OpDecorate, {state, DecorationDescriptorSet, 1});
    emit(annotations, OpDecorate, {state, DecorationBinding, GLM_SLOT_LOD_BIAS});
    name(names, OpName, block, "GLMLodBias");
    name(names, OpName, state, "glm_lod_bias");

    // Carry the descriptor slot alongside sampler function arguments. This
    // preserves different biases when the same function samples two textures.
    struct SamplerParameter { uint32_t argument, slot; };
    struct Function { uint32_t type = 0; std::vector<SamplerParameter> samplers; };
    std::map<uint32_t, Function> functions;
    uint32_t function_id = 0, argument = 0;
    for (size_t at = 5; at < words.size();) {
        uint32_t n = words[at] >> 16, op = words[at] & 0xffff;
        if (op == OpFunction) {
            function_id = words[at + 2];
            functions[function_id].type = words[at + 4];
            argument = 0;
        } else if (op == OpFunctionParameter) {
            const auto &type = reflect.get_type(words[at + 1]);
            if (type.basetype == spirv_cross::SPIRType::SampledImage && !type.image.ms &&
                type.image.dim != DimBuffer && type.image.dim != DimRect && type.image.dim != DimSubpassData) {
                uint32_t parameter = next++;
                functions[function_id].samplers.push_back({argument, parameter});
                images[words[at + 2]] = {0, 0, parameter};
            }
            ++argument;
        }
        at += n;
    }
    for (auto &[id, function] : functions) {
        if (function.samplers.empty()) continue;
        for (size_t at = 5; at < words.size();) {
            uint32_t n = words[at] >> 16;
            if ((words[at] & 0xffff) == OpTypeFunction && words[at + 1] == function.type) {
                function.type = next++;
                declarations.push_back((static_cast<uint32_t>(n + function.samplers.size()) << 16) | OpTypeFunction);
                declarations.push_back(function.type);
                declarations.insert(declarations.end(), words.begin() + at + 2, words.begin() + at + n);
                for (size_t k = 0; k < function.samplers.size(); ++k) declarations.push_back(uint_type);
                break;
            }
            at += n;
        }
    }
    auto descriptor = [&](const Image &image, std::vector<uint32_t> &prefix) {
        uint32_t slot = image.parameter ? image.parameter : constant(image.slot);
        if (image.index) {
            uint32_t index = image.index;
            if (value_types[index] != uint_type) {
                index = next++;
                emit(prefix, OpBitcast, {uint_type, index, image.index});
            }
            uint32_t indexed = next++;
            emit(prefix, OpIAdd, {uint_type, indexed, slot, index});
            slot = indexed;
        }
        return slot;
    };
    std::map<size_t, std::vector<uint32_t>> replacements;
    bool parameters_pending = false, sampled = false;
    for (size_t at = 5; at < words.size();) {
        uint32_t n = words[at] >> 16, op = words[at] & 0xffff;
        if (op == OpFunction) {
            function_id = words[at + 2];
            const auto &function = functions.at(function_id);
            parameters_pending = !function.samplers.empty();
            if (parameters_pending) {
                auto &replacement = replacements[at];
                replacement.assign(words.begin() + at, words.begin() + at + n);
                replacement[4] = function.type;
            }
        } else if (parameters_pending && op == OpLabel) {
            auto &replacement = replacements[at];
            for (const auto &parameter : functions.at(function_id).samplers)
                emit(replacement, OpFunctionParameter, {uint_type, parameter.slot});
            replacement.insert(replacement.end(), words.begin() + at, words.begin() + at + n);
            parameters_pending = false;
        }
        if ((op == OpLoad || op == OpCopyObject) && n >= 4 && images.count(words[at + 3]))
            images[words[at + 2]] = images[words[at + 3]];
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && n == 5 && images.count(words[at + 3])) {
            Image image = images[words[at + 3]];
            image.index = words[at + 4];
            images[words[at + 2]] = image;
        }
        if (op == OpFunctionCall && functions.count(words[at + 3])) {
            const auto &function = functions.at(words[at + 3]);
            if (!function.samplers.empty()) {
                auto &replacement = replacements[at];
                std::vector<uint32_t> call(words.begin() + at, words.begin() + at + n);
                for (const auto &parameter : function.samplers) {
                    auto image = images.find(words[at + 4 + parameter.argument]);
                    // Unresolved opaque arguments must not change the ABI.
                    if (image == images.end()) return words;
                    call.push_back(descriptor(image->second, replacement));
                }
                call[0] = (static_cast<uint32_t>(call.size()) << 16) | op;
                replacement.insert(replacement.end(), call.begin(), call.end());
            }
        }
        bool implicit = op == OpImageSampleImplicitLod || op == OpImageSampleDrefImplicitLod ||
                        op == OpImageSampleProjImplicitLod || op == OpImageSampleProjDrefImplicitLod;
        bool explicit_lod = op == OpImageSampleExplicitLod || op == OpImageSampleDrefExplicitLod ||
                            op == OpImageSampleProjExplicitLod || op == OpImageSampleProjDrefExplicitLod;
        bool query = op == OpImageQueryLod;
        if ((!implicit && !explicit_lod && !query) || !images.count(words[at + 3])) { at += n; continue; }
        bool dref = op == OpImageSampleDrefImplicitLod || op == OpImageSampleProjDrefImplicitLod ||
                    op == OpImageSampleDrefExplicitLod || op == OpImageSampleProjDrefExplicitLod;
        size_t mask_at = dref ? 6 : 5;
        uint32_t mask = n > mask_at ? words[at + mask_at] : 0;
        Image image = images[words[at + 3]];
        std::vector<uint32_t> prefix;
        uint32_t slot = descriptor(image, prefix);
        uint32_t pointer = next++, bias = next++;
        emit(prefix, OpAccessChain, {ptr_float, pointer, state, zero, slot});
        emit(prefix, OpLoad, {float_type, bias, pointer});
        if (mask & ImageOperandsBiasMask) {
            uint32_t sum = next++;
            emit(prefix, OpFAdd, {float_type, sum, bias, words[at + mask_at + 1]});
            bias = sum;
        }
        if (query) {
            prefix.insert(prefix.end(), words.begin() + at, words.begin() + at + n);
            replacements[at] = std::move(prefix); sampled = true; at += n; continue;
        }
        uint32_t clamped = next++;
        emit(prefix, OpExtInst, {float_type, clamped, glsl, GLSLstd450FClamp, bias, min_bias, max_bias});
        uint32_t operand = clamped;
        if (mask & ImageOperandsLodMask) {
            operand = next++;
            emit(prefix, OpFAdd, {float_type, operand, words[at + mask_at + 1], clamped});
        }
        std::vector<uint32_t> sample(words.begin() + at, words.begin() + at + n);
        if (mask & ImageOperandsGradMask) {
            uint32_t scale = next++;
            emit(prefix, OpExtInst, {float_type, scale, glsl, GLSLstd450Exp2, clamped});
            for (size_t axis = 1; axis <= 2; ++axis) {
                uint32_t gradient = sample[mask_at + axis], type = value_types.at(gradient), scaled = next++;
                emit(prefix, reflect.get_type(type).vecsize > 1 ? OpVectorTimesScalar : OpFMul,
                     {type, scaled, gradient, scale});
                sample[mask_at + axis] = scaled;
            }
        } else if (mask & (ImageOperandsBiasMask | ImageOperandsLodMask)) sample[mask_at + 1] = operand;
        else {
            if (sample.size() == mask_at) sample.push_back(0);
            sample[mask_at] |= ImageOperandsBiasMask;
            sample.insert(sample.begin() + mask_at + 1, operand);
        }
        sample[0] = (static_cast<uint32_t>(sample.size()) << 16) | op;
        prefix.insert(prefix.end(), sample.begin(), sample.end());
        replacements[at] = std::move(prefix);
        sampled = true;
        at += n;
    }
    if (!sampled) return words;
    std::vector<uint32_t> result(words.begin(), words.begin() + 5);
    bool metadata = false, declared = false;
    for (size_t at = 5; at < words.size();) {
        uint32_t n = words[at] >> 16, op = words[at] & 0xffff;
        if (op == OpMemoryModel) result.insert(result.end(), imports.begin(), imports.end());
        if (!metadata && (op == OpDecorate || op == OpMemberDecorate || (op >= OpTypeVoid && op <= OpTypeForwardPointer))) {
            result.insert(result.end(), names.begin(), names.end());
            result.insert(result.end(), annotations.begin(), annotations.end());
            metadata = true;
        }
        if (!declared && op == OpFunction) {
            result.insert(result.end(), declarations.begin(), declarations.end());
            declared = true;
        }
        auto replacement = replacements.find(at);
        if (replacement == replacements.end()) result.insert(result.end(), words.begin() + at, words.begin() + at + n);
        else result.insert(result.end(), replacement->second.begin(), replacement->second.end());
        at += n;
    }
    result[3] = next;
    return result;
}
