#define main glm_clip_probe_main
#include "shader_clip_cpu.cpp"
#undef main

int main(int argc, char **argv)
{
    try {
        require(argc == 2, "Expected artifact directory");
        initialize();
        Artifacts artifacts(argv[1]);
        const char *types[] = {"double", "dvec4", "dmat2x3"};
        const uint32_t gltypes[] = {0x140A, 0x8FFE, 0x8F49};
        const int strides[] = {16, 32, 64};
        for (int stage = 0; stage < 3; ++stage) for (int shape = 0; shape < 3; ++shape) {
            std::string vs = "#version 410 core\nlayout(location=0)in vec4 position;void main(){gl_Position=position;}";
            std::string fs = "#version 410 core\nflat in int verdict;out vec4 color;void main(){color=vec4(verdict);}";
            std::string declaration = std::string("layout(std140) uniform Values {") + types[shape] + " values[4];};";
            std::string check = std::string("int i=0;int good=(values[i]==") + types[shape] + "(i+1))?1:0;";
            std::string gs, tc, te;
            if (stage == 0) {
                gs = "#version 410 core\nlayout(triangles)in;layout(triangle_strip,max_vertices=3)out;flat out int verdict;" + declaration +
                    "void main(){" + check + "for(int j=0;j<3;++j){verdict=good;gl_Position=gl_in[j].gl_Position;EmitVertex();}EndPrimitive();}";
            } else {
                tc = "#version 410 core\nlayout(vertices=3)out;out int verdict_cp[];" + (stage == 1 ? declaration : "") +
                    "void main(){" + (stage == 1 ? check : "int good=1;") +
                    "verdict_cp[gl_InvocationID]=good;gl_out[gl_InvocationID].gl_Position=gl_in[gl_InvocationID].gl_Position;"
                    "gl_TessLevelOuter[0]=1;gl_TessLevelOuter[1]=1;gl_TessLevelOuter[2]=1;gl_TessLevelInner[0]=1;}";
                te = "#version 410 core\nlayout(triangles,equal_spacing,ccw)in;in int verdict_cp[];flat out int verdict;" +
                    (stage == 2 ? declaration : "") + "void main(){" + (stage == 2 ? check : "int good=verdict_cp[0];") +
                    "verdict=good;gl_Position=gl_TessCoord.x*gl_in[0].gl_Position+gl_TessCoord.y*gl_in[1].gl_Position+gl_TessCoord.z*gl_in[2].gl_Position;}";
            }
            glm_compile_request request = {};
            request.sources[GLM_STAGE_VERTEX] = vs.c_str(); request.sources[GLM_STAGE_FRAGMENT] = fs.c_str();
            if (stage == 0) request.sources[GLM_STAGE_GEOMETRY] = gs.c_str();
            else { request.sources[GLM_STAGE_TESS_CONTROL] = tc.c_str(); request.sources[GLM_STAGE_TESS_EVALUATION] = te.c_str(); }
            Result result; compile_uncached(&request, &result.value);
            std::string name = std::to_string(stage) + "_" + types[shape];
            require(result.value.ok, name + ": " + (result.value.log ? result.value.log : "Compile failed"));
            const auto *child = stage == 0 ? result.value.gs->kernel : stage == 1 ? result.value.tess->kernel : result.value.tess->eval;
            const glm_compile_result *compiled_results[] = {&result.value, child};
            for (const auto *compiled : compiled_results) {
                bool found = false;
                for (int i = 0; i < compiled->block_uniform_count; ++i) {
                    const auto &u = compiled->block_uniforms[i];
                    if (std::string(u.name).find("values") == std::string::npos) continue;
                    require(u.type == gltypes[shape] && u.array_stride == strides[shape], name + ": exact UBO layout lost");
                    found = true;
                }
                require(found, name + ": uniform metadata missing");
            }
            artifacts.write(name + "_vs", result.value.msl[GLM_STAGE_VERTEX]);
            artifacts.write(name + "_fs", result.value.msl[GLM_STAGE_FRAGMENT]);
            if (stage == 0) {
                artifacts.write(name + "_capture", result.value.gs->vs_capture);
                artifacts.write(name + "_kernel", result.value.gs->kernel->msl[GLM_STAGE_COMPUTE]);
                artifacts.write(name + "_pull", result.value.gs->pull->msl[GLM_STAGE_VERTEX]);
            } else {
                artifacts.write(name + "_capture", result.value.tess->vs_capture);
                artifacts.write(name + "_kernel", result.value.tess->kernel->msl[GLM_STAGE_COMPUTE]);
                artifacts.write(name + "_eval", result.value.tess->eval->msl[GLM_STAGE_VERTEX]);
            }
            std::cout << name << " retains exact storage\n";
        }
        // A float-to-double conversion in the fragment stage still selects
        // fallback for every generated stage, including an otherwise exact GS.
        glm_compile_request fallback_request = {};
        fallback_request.sources[GLM_STAGE_VERTEX] = "#version 410 core\nvoid main(){gl_Position=vec4(0);}";
        fallback_request.sources[GLM_STAGE_GEOMETRY] = "#version 410 core\nlayout(points)in;layout(points,max_vertices=1)out;layout(std140)uniform Values{double values[4];};flat out int verdict;void main(){verdict=values[0]==1.0lf?1:0;gl_Position=gl_in[0].gl_Position;EmitVertex();EndPrimitive();}";
        fallback_request.sources[GLM_STAGE_FRAGMENT] = "#version 410 core\nuniform float source;flat in int verdict;out vec4 color;void main(){double converted=double(source);color=vec4(converted==1.0lf&&verdict==1);}";
        Result fallback; compile_uncached(&fallback_request, &fallback.value);
        require(fallback.value.ok && fallback.value.gs && fallback.value.gs->kernel->ok,
                std::string("Mixed-stage fallback failed: ") + (fallback.value.log ? fallback.value.log : "no log"));
        require(uniform_storage_compatible(fallback.value, *fallback.value.gs->kernel), "Mixed-stage fallback storage differs");
        bool found_fallback = false;
        for (int i = 0; i < fallback.value.block_uniform_count; ++i) {
            const auto &u = fallback.value.block_uniforms[i];
            if (std::string(u.name).find("values") == std::string::npos) continue;
            require(u.type == 0x1406 && u.array_stride == 16, "Unsupported stage retained mixed binary64 storage");
            found_fallback = true;
        }
        require(found_fallback, "Missing fallback UBO");
        artifacts.write("fallback_capture", fallback.value.gs->vs_capture);
        artifacts.write("fallback_kernel", fallback.value.gs->kernel->msl[GLM_STAGE_COMPUTE]);
        artifacts.write("fallback_pull", fallback.value.gs->pull->msl[GLM_STAGE_VERTEX]);
        artifacts.write("fallback_fs", fallback.value.gs->pull->msl[GLM_STAGE_FRAGMENT]);
        std::cout << artifacts.count << " Metal stages; mixed-stage fallback retains matching storage\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
