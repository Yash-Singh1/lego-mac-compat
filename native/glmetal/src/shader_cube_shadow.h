#ifndef GLM_SHADER_CUBE_SHADOW_H
#define GLM_SHADER_CUBE_SHADOW_H

/* A cube has six independently filtered faces when seamless sampling is off.
   The parallel array resource uses the same storage, sampler and mip range. */
struct GLMCubeSample { uint32_t faces, disabled, projected, metadata, bias; };
struct GLMCubeSpirv {
    std::vector<uint32_t> words;
    std::map<uint32_t, GLMCubeSample> samples;
    std::map<std::string, int> resources;
};

static GLMCubeSpirv cube_shadow_spirv(const std::vector<uint32_t> &words,
                                    const std::map<std::string, int> &slots)
{
    using namespace spv;
    using namespace spirv_cross;
    GLMCubeSpirv out; out.words = words;
    Compiler reflect(words);
    std::map<uint32_t, std::vector<uint32_t>> types;
    uint32_t next = words[3], float_type = 0, uint_type = 0, vector_type = 0;
    uint32_t state = 0, state_array = 0, ptr_float = 0;
    for (size_t at = 5; at < words.size();) {
        uint32_t n = words[at] >> 16, op = words[at] & 0xffff;
        if (op >= OpTypeVoid && op <= OpTypeForwardPointer)
            types[words[at + 1]] = {words.begin() + at, words.begin() + at + n};
        if (op == OpTypeFloat && words[at + 2] == 32) float_type = words[at + 1];
        if (op == OpTypeInt && words[at + 2] == 32 && words[at + 3] == 0) uint_type = words[at + 1];
        at += n;
    }
    for (auto &[id, t] : types) {
        if ((t[0] & 0xffff) == OpTypeVector && t[2] == float_type && t[3] == 4) vector_type = id;
        if ((t[0] & 0xffff) == OpTypePointer && t[2] == StorageClassUniform && t[3] == float_type) ptr_float = id;
    }
    for (auto &block : reflect.get_shader_resources().uniform_buffers)
        if (reflect.get_name(block.base_type_id) == "GLMLodBias") {
            state = block.id;
            state_array = reflect.get_type(block.base_type_id).member_types[0];
        }
    if (!state || !ptr_float || !uint_type) return out;
    auto cube_type = [&](uint32_t id) {
        const auto &t = reflect.get_type(id);
        return t.basetype == SPIRType::SampledImage && t.image.dim == DimCube && (!t.image.arrayed || !t.image.depth) &&
               reflect.get_type(t.image.type).basetype == SPIRType::Float;
    };
    std::vector<uint32_t> declarations, annotations, names;
    auto emit = [](std::vector<uint32_t> &v, Op op, std::initializer_list<uint32_t> args) {
        v.push_back((uint32_t(args.size() + 1) << 16) | op); v.insert(v.end(), args.begin(), args.end());
    };
    auto name = [&](uint32_t id, const std::string &text) {
        size_t at = names.size(), count = (text.size() + 4) / 4;
        names.resize(at + count + 2, 0); names[at] = (uint32_t(count + 2) << 16) | OpName;
        names[at + 1] = id; memcpy(names.data() + at + 2, text.c_str(), text.size());
    };
    std::map<uint32_t, uint32_t> cloned;
    auto clone = [&](auto &&self, uint32_t id) -> uint32_t {
        if (cloned.count(id)) return cloned[id];
        auto t = types.at(id); Op op = Op(t[0] & 0xffff);
        if (op == OpTypePointer) t[3] = self(self, t[3]);
        else if (op == OpTypeArray) t[2] = self(self, t[2]);
        else if (op == OpTypeSampledImage) {
            uint32_t result = self(self, t[2]); cloned[id] = result; return result;
        }
        else if (op == OpTypeImage) { t[3] = Dim2D; t[5] = 1; }
        else throw CompilerError("Unsupported cube shadow resource type.");
        uint32_t result = next++; cloned[id] = result; t[1] = result;
        declarations.insert(declarations.end(), t.begin(), t.end()); return result;
    };
    struct Face { uint32_t id, type; };
    std::map<uint32_t, Face> faces;
    for (auto &image : reflect.get_shader_resources().sampled_images) {
        if (!cube_type(image.type_id)) continue;
        std::string original = reflect.get_name(image.id);
        auto slot = slots.find(original); if (slot == slots.end()) continue;
        uint32_t type = clone(clone, image.type_id), id = next++;
        emit(declarations, OpVariable, {type, id, StorageClassUniformConstant});
        emit(annotations, OpDecorate, {id, DecorationDescriptorSet, 8});
        emit(annotations, OpDecorate, {id, DecorationBinding, uint32_t(slot->second)});
        std::string hidden = "glm_cube_faces_" + original; name(id, hidden);
        out.resources[hidden] = slot->second; faces[image.id] = {id, type};
    }
    if (faces.empty()) return out;
    bool declare_vector = !vector_type;
    if (declare_vector) vector_type = next++;
    uint32_t zero = next++, one = next++, two = next++, meta_offset = next++, ptr_vector = next++;
    emit(declarations, OpConstant, {uint_type, zero, 0});
    emit(declarations, OpConstant, {uint_type, one, 1});
    emit(declarations, OpConstant, {uint_type, two, 2});
    emit(declarations, OpConstant, {uint_type, meta_offset, 64});
    emit(declarations, OpTypePointer, {ptr_vector, StorageClassUniform, vector_type});
    struct Parameter { uint32_t argument, id, type; };
    struct Function { uint32_t type; std::vector<Parameter> parameters; };
    std::map<uint32_t, Function> functions;
    uint32_t function = 0, argument = 0;
    for (size_t at = 5; at < words.size();) {
        uint32_t n = words[at] >> 16, op = words[at] & 0xffff;
        if (op == OpFunction) { function = words[at + 2]; argument = 0; functions[function].type = words[at + 4]; }
        if (op == OpFunctionParameter) {
            if (cube_type(words[at + 1])) {
                uint32_t id = next++, type = clone(clone, words[at + 1]);
                faces[words[at + 2]] = {id, type}; functions[function].parameters.push_back({argument, id, type});
            }
            ++argument;
        }
        at += n;
    }
    for (auto &[id, f] : functions) if (!f.parameters.empty()) {
        auto t = types.at(f.type); f.type = next++; t[1] = f.type;
        for (auto &parameter : f.parameters) t.push_back(parameter.type);
        t[0] = (uint32_t(t.size()) << 16) | OpTypeFunction;
        declarations.insert(declarations.end(), t.begin(), t.end());
    }
    std::map<size_t, std::vector<uint32_t>> replacements;
    uint32_t current_slot = 0; bool pending = false;
    for (size_t at = 5; at < words.size();) {
        uint32_t n = words[at] >> 16; Op op = Op(words[at] & 0xffff);
        std::vector<uint32_t> instruction(words.begin() + at, words.begin() + at + n), prefix;
        if (op == OpFunction) {
            function = instruction[2]; current_slot = 0; pending = !functions.at(function).parameters.empty();
            if (pending) instruction[4] = functions.at(function).type;
        } else if (op == OpLabel && pending) {
            for (auto &parameter : functions.at(function).parameters)
                emit(prefix, OpFunctionParameter, {parameter.type, parameter.id});
            pending = false;
        }
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && n == 6 && instruction[3] == state) {
            current_slot = instruction[5];
            instruction.push_back(zero);
        }
        if ((op == OpLoad || op == OpCopyObject || op == OpAccessChain || op == OpInBoundsAccessChain) &&
            n >= 4 && faces.count(words[at + 3])) {
            uint32_t id = next++, type = clone(clone, words[at + 1]);
            auto copy = instruction; copy[1] = type; copy[2] = id; copy[3] = faces.at(words[at + 3]).id;
            copy[0] = (uint32_t(copy.size()) << 16) | op;
            prefix.insert(prefix.end(), copy.begin(), copy.end()); faces[words[at + 2]] = {id, type};
        }
        if (op == OpFunctionCall && functions.count(instruction[3])) {
            for (auto &parameter : functions.at(instruction[3]).parameters) {
                auto face = faces.find(instruction[4 + parameter.argument]);
                if (face == faces.end()) throw CompilerError("Unresolved cube shadow function argument.");
                instruction.push_back(face->second.id);
            }
        }
        if ((op == OpImageSampleDrefImplicitLod || op == OpImageSampleDrefExplicitLod ||
             op == OpImageSampleImplicitLod || op == OpImageSampleExplicitLod || op == OpImageQueryLod) &&
            faces.count(instruction[3]) && current_slot) {
            uint32_t pointer = next++, value = next++;
            emit(prefix, OpAccessChain, {ptr_float, pointer, state, zero, current_slot, one});
            emit(prefix, OpLoad, {float_type, value, pointer});
            uint32_t wrap_pointer = next++, projected = next++;
            emit(prefix, OpAccessChain, {ptr_float, wrap_pointer, state, zero, current_slot, two});
            emit(prefix, OpLoad, {float_type, projected, wrap_pointer});
            uint32_t meta_index = next++, meta_pointer = next++, metadata = next++;
            emit(prefix, OpIAdd, {uint_type, meta_index, current_slot, meta_offset});
            emit(prefix, OpAccessChain, {ptr_vector, meta_pointer, state, zero, meta_index});
            emit(prefix, OpLoad, {vector_type, metadata, meta_pointer});
            uint32_t bias_pointer = next++, object_bias = next++;
            emit(prefix, OpAccessChain, {ptr_float, bias_pointer, state, zero, current_slot, zero});
            emit(prefix, OpLoad, {float_type, object_bias, bias_pointer});
            out.samples[instruction[2]] = {faces.at(instruction[3]).id, value, projected, metadata, object_bias};
        }
        instruction[0] = (uint32_t(instruction.size()) << 16) | op;
        prefix.insert(prefix.end(), instruction.begin(), instruction.end()); replacements[at] = std::move(prefix);
        at += n;
    }
    out.words.assign(words.begin(), words.begin() + 5);
    bool named = false, annotated = false, declared = false;
    for (size_t at = 5; at < words.size();) {
        uint32_t n = words[at] >> 16; Op op = Op(words[at] & 0xffff);
        bool type = op >= OpTypeVoid && op <= OpTypeForwardPointer;
        if (!named && (op == OpDecorate || op == OpMemberDecorate || type)) {
            out.words.insert(out.words.end(), names.begin(), names.end()); named = true;
        }
        if (!annotated && type) { out.words.insert(out.words.end(), annotations.begin(), annotations.end()); annotated = true; }
        if (op == OpTypeArray && words[at + 1] == state_array) {
            if (declare_vector) emit(out.words, OpTypeVector, {vector_type, float_type, 4});
            auto t = types.at(state_array); t[2] = vector_type;
            out.words.insert(out.words.end(), t.begin(), t.end());
        } else {
            if (!declared && op == OpFunction) { out.words.insert(out.words.end(), declarations.begin(), declarations.end()); declared = true; }
            auto found = replacements.find(at);
            if (found != replacements.end()) out.words.insert(out.words.end(), found->second.begin(), found->second.end());
            else out.words.insert(out.words.end(), words.begin() + at, words.begin() + at + n);
        }
        at += n;
    }
    out.words[3] = next;
    return out;
}

static std::string glm_cube_shadow_helpers()
{
    return R"MSL(
struct glm_cube_projection { float2 uv; float2 gx; float2 gy; uint face; };
inline glm_cube_projection glm_cube_project(float3 d, float3 dx, float3 dy) {
    float3 a = abs(d); float3 p, px, py; uint face;
    if (a.x >= a.y && a.x >= a.z) {
        if (d.x >= 0) { p=float3(-d.z,-d.y,d.x); px=float3(-dx.z,-dx.y,dx.x); py=float3(-dy.z,-dy.y,dy.x); face=0; }
        else { p=float3(d.z,-d.y,-d.x); px=float3(dx.z,-dx.y,-dx.x); py=float3(dy.z,-dy.y,-dy.x); face=1; }
    } else if (a.y >= a.z) {
        if (d.y >= 0) { p=float3(d.x,d.z,d.y); px=float3(dx.x,dx.z,dx.y); py=float3(dy.x,dy.z,dy.y); face=2; }
        else { p=float3(d.x,-d.z,-d.y); px=float3(dx.x,-dx.z,-dx.y); py=float3(dy.x,-dy.z,-dy.y); face=3; }
    } else {
        if (d.z >= 0) { p=float3(d.x,-d.y,d.z); px=float3(dx.x,-dx.y,dx.z); py=float3(dy.x,-dy.y,dy.z); face=4; }
        else { p=float3(-d.x,-d.y,-d.z); px=float3(-dx.x,-dx.y,-dx.z); py=float3(-dy.x,-dy.y,-dy.z); face=5; }
    }
    float inv=1.0/p.z;
    glm_cube_projection result;
    result.uv=(p.xy/p.z)*0.5+0.5;
    result.gx=(px.xy-p.xy*(px.z*inv))*inv*0.5;
    result.gy=(py.xy-p.xy*(py.z*inv))*inv*0.5;
    result.face=face; return result;
}
inline gradient2d glm_cube_face_gradient(float3 d, float3 dx, float3 dy) {
    float3 a=abs(d); float2 gx,gy; float major;
    if (a.x >= a.y && a.x >= a.z) {
        gx=float2(d.x >= 0 ? -dx.z : dx.z,-dx.y);
        gy=float2(d.x >= 0 ? -dy.z : dy.z,-dy.y); major=a.x;
    } else if (a.y >= a.z) {
        gx=float2(dx.x,d.y >= 0 ? dx.z : -dx.z);
        gy=float2(dy.x,d.y >= 0 ? dy.z : -dy.z); major=a.y;
    } else {
        gx=float2(d.z >= 0 ? dx.x : -dx.x,-dx.y);
        gy=float2(d.z >= 0 ? dy.x : -dy.x,-dy.y); major=a.z;
    }
    float scale=0.5/major;
    return gradient2d(gx*scale,gy*scale);
}
inline gradient2d glm_cube_agx_gradient(float3 d,float3 dx,float3 dy) {
    float3 a=abs(d);float2 gx,gy;float major;
    if(a.x>=a.y&&a.x>=a.z){
        gx=float2(d.x>=0?-dx.z:dx.z,-dx.x);
        gy=float2(d.x>=0?-dy.z:dy.z,-dy.x);major=a.x;
    }else if(a.y>=a.z){
        gx=float2(dx.x,d.y>=0?dx.y:-dx.y);
        gy=float2(dy.x,d.y>=0?dy.y:-dy.y);major=a.y;
    }else{
        gx=float2(d.z>=0?dx.z:-dx.z,-dx.y);
        gy=float2(d.z>=0?dy.z:-dy.z,-dy.y);major=a.z;
    }
    return gradient2d(gx*(0.5/major),gy*(0.5/major));
}
inline float glm_cube_shadow_level(depth2d_array<float> faces, sampler s, float2 uv,
                                    uint face, float reference, float l, float4 metadata) {
    if (metadata.w == 0) return faces.sample_compare(s,uv,face,reference,level(l));
    float last=float(faces.get_num_mip_levels()-1);
    float lod=clamp(clamp(l,metadata.x,metadata.y),0.0,last);
    if (metadata.w == 1) return faces.sample_compare(s,uv,face,reference,level(floor(lod+0.5)));
    float lower=floor(lod),upper=min(lower+1.0,last);
    // Fractional sampler clamps would move these integer sample levels.
    // Let hardware apply the clamp before filtering in that situation.
    if (lower < metadata.x || upper > metadata.y)
        return faces.sample_compare(s,uv,face,reference,level(l));
    float low=faces.sample_compare(s,uv,face,reference,level(lower));
    float high=faces.sample_compare(s,uv,face,reference,level(upper));
    return mix(low,high,lod-lower);
}
inline bool glm_cube_inside(float2 uv, float coarsest_lod, uint width, uint levels,
                            float3 d, float3 dx, float3 dy, float b, float anisotropy) {
    float l=clamp(ceil(coarsest_lod),0.0,float(levels-1));
    float2 radius=float2(0.5*exp2(l)/float(width));
    if (anisotropy > 1) {
        glm_cube_projection footprint=glm_cube_project(d,dx*exp2(b),dy*exp2(b));
        radius+=0.5*(abs(footprint.gx)+abs(footprint.gy));
    }
    return all(uv > radius) && all(uv < 1.0-radius);
}
inline float glm_cube_shadow_grad(depthcube<float> cube, depth2d_array<float> faces,
                                  sampler s, float3 d, float reference, float disabled, float projected, float4 metadata,
                                  float3 dx, float3 dy) {
    if (disabled == 0) return cube.sample_compare(s,d,reference,gradientcube(dx,dy));
    glm_cube_projection p=glm_cube_project(d,dx,dy);
    if (projected != 0) return faces.sample_compare(s,p.uv,p.face,reference,glm_cube_face_gradient(d,dx,dy));
    // Match the native Apple cube-gradient instruction's component selection.
    // That instruction ignores one gradient component for each major axis.
    float3 a=abs(d); float2 gx,gy; float major;
    if (a.x >= a.y && a.x >= a.z) { gx=dx.xz; gy=dy.xz; major=a.x; }
    else if (a.y >= a.z) { gx=dx.xy; gy=dy.xy; major=a.y; }
    else { gx=dx.yz; gy=dy.yz; major=a.z; }
    if (metadata.z > 1) return faces.sample_compare(s,p.uv,p.face,reference,glm_cube_agx_gradient(d,dx,dy));
    float scale=0.5/major;
    return faces.sample_compare(s,p.uv,p.face,reference,gradient2d(gx*scale,gy*scale));
}
inline float glm_cube_shadow_bias(depthcube<float> cube, depth2d_array<float> faces,
                                  sampler s, float3 d, float reference, float disabled, float projected, float4 metadata, float b) {
    float native=cube.sample_compare(s,d,reference,bias(b));
    float lod=cube.calculate_unclamped_lod(s,d)+b;
    float3 dx=dfdx(d),dy=dfdy(d);
    if (disabled == 0) return native;
    glm_cube_projection p=glm_cube_project(d,float3(0),float3(0));
    if (projected != 0) {
        gradient2d g=glm_cube_face_gradient(d,dx,dy);
        float rho=max(length(g.dPdx),length(g.dPdy))*float(faces.get_width());
        return faces.sample_compare(s,p.uv,p.face,reference,level(log2(rho)+b));
    }
    float coarsest=max(cube.calculate_clamped_lod(s,d),lod);
    if (glm_cube_inside(p.uv,coarsest,faces.get_width(),faces.get_num_mip_levels(),d,dx,dy,b,metadata.z))
        return native;
    if (metadata.z > 1) {
        glm_cube_projection footprint=glm_cube_project(d,dx,dy);
        float scale=exp2(b);
        return faces.sample_compare(s,p.uv,p.face,reference,gradient2d(footprint.gx*scale,footprint.gy*scale));
    }
    return glm_cube_shadow_level(faces,s,p.uv,p.face,reference,lod,metadata);
}
inline float glm_cube_shadow_lod(depthcube<float> cube, depth2d_array<float> faces,
                                 sampler s, float3 d, float reference, float disabled, float projected, float4 metadata, float l) {
    if (disabled == 0) return cube.sample_compare(s,d,reference,level(l));
    glm_cube_projection p=glm_cube_project(d,float3(0),float3(0));
    return glm_cube_shadow_level(faces,s,p.uv,p.face,reference,l,metadata);
}
inline float4 glm_cube_float_level(texture2d_array<float> faces, sampler s, float2 uv,
                                   uint face, float l, float4 metadata) {
    return faces.sample(s,uv,face,level(l));
}
inline float4 glm_cube_float_grad(texturecube<float> cube, texture2d_array<float> faces,
                                  sampler s, float3 d, float disabled, float projected, float4 metadata,
                                  float3 dx, float3 dy) {
    if (disabled == 0) return cube.sample(s,d,gradientcube(dx,dy));
    glm_cube_projection p=glm_cube_project(d,dx,dy);
    if (projected != 0) return faces.sample(s,p.uv,p.face,glm_cube_face_gradient(d,dx,dy));
    // Match the native Apple cube-gradient instruction's component selection.
    // That instruction ignores one gradient component for each major axis.
    float3 a=abs(d); float2 gx,gy; float major;
    if (a.x >= a.y && a.x >= a.z) { gx=dx.xz; gy=dy.xz; major=a.x; }
    else if (a.y >= a.z) { gx=dx.xy; gy=dy.xy; major=a.y; }
    else { gx=dx.yz; gy=dy.yz; major=a.z; }
    if (metadata.z > 1) return faces.sample(s,p.uv,p.face,glm_cube_agx_gradient(d,dx,dy));
    float scale=0.5/major;
    return faces.sample(s,p.uv,p.face,gradient2d(gx*scale,gy*scale));
}
inline float4 glm_cube_float_bias(texturecube<float> cube, texture2d_array<float> faces,
                                  sampler s, float3 d, float disabled, float projected, float4 metadata, float b) {
    float4 native=cube.sample(s,d,bias(b));
    float lod=cube.calculate_unclamped_lod(s,d)+b;
    float3 dx=dfdx(d),dy=dfdy(d);
    if (disabled == 0) return native;
    glm_cube_projection p=glm_cube_project(d,float3(0),float3(0));
    if (projected != 0) {
        gradient2d g=glm_cube_face_gradient(d,dx,dy);
        float rho=max(length(g.dPdx),length(g.dPdy))*float(faces.get_width());
        return faces.sample(s,p.uv,p.face,level(log2(rho)+b));
    }
    float coarsest=max(cube.calculate_clamped_lod(s,d),lod);
    if (glm_cube_inside(p.uv,coarsest,faces.get_width(),faces.get_num_mip_levels(),d,dx,dy,b,metadata.z))
        return native;
    if (metadata.z > 1) {
        glm_cube_projection footprint=glm_cube_project(d,dx,dy);
        float scale=exp2(b);
        return faces.sample(s,p.uv,p.face,gradient2d(footprint.gx*scale,footprint.gy*scale));
    }
    return glm_cube_float_level(faces,s,p.uv,p.face,lod,metadata);
}
inline float4 glm_cube_float_lod(texturecube<float> cube, texture2d_array<float> faces,
                                 sampler s, float3 d, float disabled, float projected, float4 metadata, float l) {
    if (disabled == 0) return cube.sample(s,d,level(l));
    glm_cube_projection p=glm_cube_project(d,float3(0),float3(0));
    return glm_cube_float_level(faces,s,p.uv,p.face,l,metadata);
}

inline float4 glm_cube_array_float_grad(texturecube_array<float> cube, texture2d_array<float> faces,
                                  sampler s, float4 coordinate, float disabled, float projected, float4 metadata,
                                  float3 dx, float3 dy) {
    float3 d=coordinate.xyz;
    uint cube_layer=uint(clamp(floor(coordinate.w+0.5),0.0,float(cube.get_array_size()-1)));
    if (disabled == 0) return cube.sample(s,d,cube_layer,gradientcube(dx,dy));
    glm_cube_projection p=glm_cube_project(d,dx,dy);
    p.face += cube_layer*6u;
    if (projected != 0) return faces.sample(s,p.uv,p.face,glm_cube_face_gradient(d,dx,dy));
    // Match the native Apple cube-gradient instruction's component selection.
    // That instruction ignores one gradient component for each major axis.
    float3 a=abs(d); float2 gx,gy; float major;
    if (a.x >= a.y && a.x >= a.z) { gx=dx.xz; gy=dy.xz; major=a.x; }
    else if (a.y >= a.z) { gx=dx.xy; gy=dy.xy; major=a.y; }
    else { gx=dx.yz; gy=dy.yz; major=a.z; }
    if (metadata.z > 1) return faces.sample(s,p.uv,p.face,glm_cube_agx_gradient(d,dx,dy));
    float scale=0.5/major;
    return faces.sample(s,p.uv,p.face,gradient2d(gx*scale,gy*scale));
}
inline float4 glm_cube_array_float_bias(texturecube_array<float> cube, texture2d_array<float> faces,
                                  sampler s, float4 coordinate, float disabled, float projected, float4 metadata, float b) {
    float3 d=coordinate.xyz;
    uint cube_layer=uint(clamp(floor(coordinate.w+0.5),0.0,float(cube.get_array_size()-1)));
    float4 native=cube.sample(s,d,cube_layer,bias(b));
    float lod=cube.calculate_unclamped_lod(s,d)+b;
    float3 dx=dfdx(d),dy=dfdy(d);
    if (disabled == 0) return native;
    glm_cube_projection p=glm_cube_project(d,float3(0),float3(0));
    p.face += cube_layer*6u;
    if (projected != 0) {
        gradient2d g=glm_cube_face_gradient(d,dx,dy);
        float rho=max(length(g.dPdx),length(g.dPdy))*float(faces.get_width());
        return faces.sample(s,p.uv,p.face,level(log2(rho)+b));
    }
    float coarsest=max(cube.calculate_clamped_lod(s,d),lod);
    if (glm_cube_inside(p.uv,coarsest,faces.get_width(),faces.get_num_mip_levels(),d,dx,dy,b,metadata.z))
        return native;
    if (metadata.z > 1) {
        glm_cube_projection footprint=glm_cube_project(d,dx,dy);
        float scale=exp2(b);
        return faces.sample(s,p.uv,p.face,gradient2d(footprint.gx*scale,footprint.gy*scale));
    }
    return glm_cube_float_level(faces,s,p.uv,p.face,lod,metadata);
}
inline float4 glm_cube_array_float_lod(texturecube_array<float> cube, texture2d_array<float> faces,
                                 sampler s, float4 coordinate, float disabled, float projected, float4 metadata, float l) {
    float3 d=coordinate.xyz;
    uint cube_layer=uint(clamp(floor(coordinate.w+0.5),0.0,float(cube.get_array_size()-1)));
    if (disabled == 0) return cube.sample(s,d,cube_layer,level(l));
    glm_cube_projection p=glm_cube_project(d,float3(0),float3(0));
    p.face += cube_layer*6u;
    return glm_cube_float_level(faces,s,p.uv,p.face,l,metadata);
}

template<typename Cube, typename Faces>
inline float2 glm_cube_query(Cube cube, Faces faces, sampler s, float3 d,
                             float disabled, float projected, float4 metadata, float object_bias) {
    float3 dx=dfdx(d),dy=dfdy(d);
    if (disabled == 0 || projected == 0) {
        if (object_bias == 0) return float2(cube.calculate_clamped_lod(s,d),cube.calculate_unclamped_lod(s,d));
        float l=cube.calculate_unclamped_lod(s,d)+object_bias;
        float selected=clamp(clamp(l,metadata.x,metadata.y),0.0,float(faces.get_num_mip_levels()-1));
        if (metadata.w == 0) selected=0;
        else if (metadata.w == 1) selected=floor(selected+0.5);
        return float2(selected,l);
    }
    gradient2d g=glm_cube_face_gradient(d,dx,dy);
    float rho=max(length(g.dPdx),length(g.dPdy))*float(faces.get_width());
    float l=log2(rho)+object_bias;
    float selected=clamp(clamp(l,metadata.x,metadata.y),0.0,float(faces.get_num_mip_levels()-1));
    if (metadata.w == 0) selected=0;
    else if (metadata.w == 1) selected=floor(selected+0.5);
    return float2(selected,l);
}
)MSL";
}

class GLMCubeCompilerMSL : public GLMCompilerMSL {
public:
    GLMCubeCompilerMSL(const GLMCubeSpirv &code, bool fp64) : GLMCompilerMSL(code.words, fp64), cube_samples(code.samples) {}
protected:
    std::map<uint32_t, GLMCubeSample> cube_samples;
    void emit_header() override {
        GLMCompilerMSL::emit_header();
        if (!cube_samples.empty()) statement(glm_cube_shadow_helpers());
    }
    void emit_instruction(const spirv_cross::Instruction &instruction) override {
        if (instruction.op != spv::OpImageQueryLod) { GLMCompilerMSL::emit_instruction(instruction); return; }
        const uint32_t *ops=stream(instruction);
        auto found=cube_samples.find(ops[1]);
        if (found != cube_samples.end()) {
            const auto &sample=found->second;
            std::string call="glm_cube_query("+to_expression(ops[2])+", "+to_expression(sample.faces)+", "+
                to_sampler_expression(ops[2])+", ("+to_expression(ops[3])+").xyz, "+to_expression(sample.disabled)+", "+
                to_expression(sample.projected)+", "+to_expression(sample.metadata)+", "+to_expression(sample.bias)+")";
            emit_op(ops[0],ops[1],call,false);
            for (uint32_t id : {ops[2],ops[3],sample.faces,sample.disabled,sample.projected,sample.metadata,sample.bias})
                inherit_expression_dependencies(ops[1],id);
            register_control_dependent_expression(ops[1]); return;
        }
        GLMCompilerMSL::emit_instruction(instruction);
    }
    std::string to_texture_op(const spirv_cross::Instruction &instruction, bool sparse, bool *forward,
                             spirv_cross::SmallVector<uint32_t> &dependencies) override {
        const uint32_t *ops = stream(instruction);
        auto found = cube_samples.find(ops[1]);
        if (sparse || found == cube_samples.end())
            return GLMCompilerMSL::to_texture_op(instruction, sparse, forward, dependencies);
        bool shadow = expression_type(ops[2]).image.depth;
        bool array = expression_type(ops[2]).image.arrayed;
        uint32_t mask_at = shadow ? 5u : 4u;
        uint32_t mask = instruction.length > mask_at ? ops[mask_at] : 0;
        if (mask & ~(spv::ImageOperandsBiasMask | spv::ImageOperandsLodMask | spv::ImageOperandsGradMask))
            throw spirv_cross::CompilerError("Unsupported cube shadow sampling operands.");
        uint32_t at = mask_at + 1, bias = 0, lod = 0, gx = 0, gy = 0;
        if (mask & spv::ImageOperandsBiasMask) bias = ops[at++];
        if (mask & spv::ImageOperandsLodMask) lod = ops[at++];
        if (mask & spv::ImageOperandsGradMask) { gx=ops[at++]; gy=ops[at++]; }
        std::string mode = gx ? "grad" : lod ? "lod" : "bias";
        if (!gx && !lod && get_execution_model() != spv::ExecutionModelFragment) mode = "lod";
        auto expr = [&](uint32_t id) { dependencies.push_back(id); return to_expression(id); };
        std::string call = std::string(shadow ? "glm_cube_shadow_" : array ? "glm_cube_array_float_" : "glm_cube_float_") + mode + "(" + expr(ops[2]) + ", " + expr(found->second.faces) +
            ", " + to_sampler_expression(ops[2]) + ", (" + expr(ops[3]) + (array ? ").xyzw, " : ").xyz, ");
        if (shadow) call += expr(ops[4]) + ", ";
        call += expr(found->second.disabled) + ", " + expr(found->second.projected) + ", " + expr(found->second.metadata);
        if (gx) call += ", " + expr(gx) + ", " + expr(gy);
        else call += ", " + (mode == "lod" ? (lod ? expr(lod) : "0.0") : (bias ? expr(bias) : "0.0"));
        *forward = false; return call + ")";
    }
};

#endif
