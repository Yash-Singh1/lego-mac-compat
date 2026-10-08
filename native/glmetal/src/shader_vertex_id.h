#ifndef GLM_SHADER_VERTEX_ID_H
#define GLM_SHADER_VERTEX_ID_H

/* Only the user gl_VertexID expression adds GLMPoint.member3. Capture and
   generated pull indices remain raw. Inject after public GL reflection so
   the hidden stream buffer does not become a user resource. */
static std::vector<uint32_t> vertex_id_spirv(const std::vector<uint32_t> &words)
{
    using namespace spv;
    using namespace spirv_cross;
    Compiler reflect(words);
    uint32_t point = 0;
    for (const auto &block : reflect.get_shader_resources().uniform_buffers)
        if (reflect.get_name(block.base_type_id) == "GLMPoint" &&
            reflect.get_type(block.base_type_id).member_types.size() >= 6) point = block.id;
    if (!point) return words;
    std::map<uint32_t, uint32_t> constants;
    std::set<uint32_t> base_pointers, base_values;
    uint32_t uint_type = 0, bool_type = 0, uniform_uint = 0;
    for (size_t at = 5; at < words.size();) {
        uint32_t count = words[at] >> 16, op = words[at] & 0xffff;
        if (op == OpTypeInt && words[at + 2] == 32 && words[at + 3] == 0) uint_type = words[at + 1];
        if (op == OpTypeBool) bool_type = words[at + 1];
        if (op == OpConstant && count == 4) constants[words[at + 2]] = words[at + 3];
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && words[at + 3] == point &&
            count == 5 && constants.count(words[at + 4]) && constants[words[at + 4]] == 3)
            base_pointers.insert(words[at + 2]);
        if (op == OpLoad && base_pointers.count(words[at + 3])) base_values.insert(words[at + 2]);
        at += count;
    }
    std::map<size_t, uint32_t> additions;
    for (size_t at = 5; at < words.size();) {
        uint32_t count = words[at] >> 16, op = words[at] & 0xffff;
        if (op == OpTypePointer && words[at + 2] == StorageClassUniform && words[at + 3] == uint_type)
            uniform_uint = words[at + 1];
        if (op == OpIAdd && words[at + 1] == uint_type) {
            if (base_values.count(words[at + 3])) additions[at] = words[at + 4];
            else if (base_values.count(words[at + 4])) additions[at] = words[at + 3];
        }
        at += count;
    }
    if (additions.empty() || !uint_type || !uniform_uint) return words;
    uint32_t next = words[3];
    std::vector<uint32_t> declarations, annotations, names, functions;
    auto emit = [](std::vector<uint32_t> &out, Op op, std::initializer_list<uint32_t> arguments) {
        out.push_back((uint32_t(arguments.size() + 1) << 16) | op);
        out.insert(out.end(), arguments.begin(), arguments.end());
    };
    auto name = [&](uint32_t id, const char *text) {
        size_t start = names.size(), length = strlen(text), count = (length + 4) / 4;
        names.resize(start + count + 2, 0);
        names[start] = (uint32_t(count + 2) << 16) | OpName;
        names[start + 1] = id;
        memcpy(names.data() + start + 2, text, length);
    };
    if (!bool_type) {
        bool_type = next++;
        emit(declarations, OpTypeBool, {bool_type});
    }
    uint32_t zero = next++, four = next++, five = next++;
    emit(declarations, OpConstant, {uint_type, zero, 0});
    emit(declarations, OpConstant, {uint_type, four, 4});
    emit(declarations, OpConstant, {uint_type, five, 5});
    uint32_t array = next++, block = next++, block_pointer = next++, stream = next++;
    emit(declarations, OpTypeRuntimeArray, {array, uint_type});
    emit(declarations, OpTypeStruct, {block, array});
    emit(declarations, OpTypePointer, {block_pointer, StorageClassUniform, block});
    emit(declarations, OpVariable, {block_pointer, stream, StorageClassUniform});
    emit(annotations, OpDecorate, {array, DecorationArrayStride, 4});
    emit(annotations, OpDecorate, {block, DecorationBufferBlock});
    emit(annotations, OpMemberDecorate, {block, 0, DecorationOffset, 0});
    emit(annotations, OpMemberDecorate, {block, 0, DecorationNonWritable});
    emit(annotations, OpDecorate, {stream, DecorationDescriptorSet, 1});
    emit(annotations, OpDecorate, {stream, DecorationBinding, GLM_SLOT_STREAM});
    name(block, "GLMVertexIDs");
    name(stream, "glm_vertex_ids");
    uint32_t function_type = next++, function = next++, raw = next++, affine = next++;
    emit(declarations, OpTypeFunction, {function_type, uint_type, uint_type, uint_type});
    name(function, "glm_original_vertex_id");
    uint32_t entry = next++, enabled_pointer = next++, enabled = next++, condition = next++;
    uint32_t mapped = next++, unchanged = next++, merge = next++;
    uint32_t offset_pointer = next++, offset = next++, index = next++, id_pointer = next++, id = next++, result = next++;
    emit(functions, OpFunction, {uint_type, function, FunctionControlMaskNone, function_type});
    emit(functions, OpFunctionParameter, {uint_type, raw});
    emit(functions, OpFunctionParameter, {uint_type, affine});
    emit(functions, OpLabel, {entry});
    emit(functions, OpAccessChain, {uniform_uint, enabled_pointer, point, five});
    emit(functions, OpLoad, {uint_type, enabled, enabled_pointer});
    emit(functions, OpINotEqual, {bool_type, condition, enabled, zero});
    emit(functions, OpSelectionMerge, {merge, SelectionControlMaskNone});
    emit(functions, OpBranchConditional, {condition, mapped, unchanged});
    emit(functions, OpLabel, {mapped});
    emit(functions, OpAccessChain, {uniform_uint, offset_pointer, point, four});
    emit(functions, OpLoad, {uint_type, offset, offset_pointer});
    emit(functions, OpIAdd, {uint_type, index, offset, raw});
    emit(functions, OpAccessChain, {uniform_uint, id_pointer, stream, zero, index});
    emit(functions, OpLoad, {uint_type, id, id_pointer});
    emit(functions, OpBranch, {merge});
    emit(functions, OpLabel, {unchanged});
    emit(functions, OpBranch, {merge});
    emit(functions, OpLabel, {merge});
    emit(functions, OpPhi, {uint_type, result, id, mapped, affine, unchanged});
    emit(functions, OpReturnValue, {result});
    emit(functions, OpFunctionEnd, {});

    std::vector<uint32_t> out(words.begin(), words.begin() + 5);
    bool named = false, annotated = false, declared = false;
    for (size_t at = 5; at < words.size();) {
        uint32_t count = words[at] >> 16, op = words[at] & 0xffff;
        if (!named && (op == OpDecorate || op == OpMemberDecorate || (op >= OpTypeVoid && op <= OpTypeForwardPointer))) {
            out.insert(out.end(), names.begin(), names.end()); named = true;
        }
        if (!annotated && op >= OpTypeVoid && op <= OpTypeForwardPointer) {
            out.insert(out.end(), annotations.begin(), annotations.end()); annotated = true;
        }
        if (!declared && op == OpFunction) {
            out.insert(out.end(), declarations.begin(), declarations.end());
            out.insert(out.end(), functions.begin(), functions.end()); declared = true;
        }
        auto found = additions.find(at);
        if (found != additions.end()) {
            uint32_t temporary = next++;
            emit(out, OpIAdd, {uint_type, temporary, words[at + 3], words[at + 4]});
            emit(out, OpFunctionCall, {uint_type, words[at + 2], function, found->second, temporary});
        } else out.insert(out.end(), words.begin() + at, words.begin() + at + count);
        at += count;
    }
    out[3] = next;
    return out;
}
#endif
