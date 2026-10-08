/* CPU-only diagnostic. No driver or GPU execution. */
#define main glm_clip_probe_main
#include "shader_clip_cpu.cpp"
#undef main

static std::vector<uint32_t> uniform_spirv(const std::string &source, EShLanguage stage)
{
    glslang::TShader shader(stage);
    const char *text = source.c_str();
    shader.setStrings(&text, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 450);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    require(shader.parse(GetDefaultResources(), 450, false, messages), shader.getInfoLog());
    glslang::TProgram program;
    program.addShader(&shader);
    require(program.link(messages), program.getInfoLog());
    std::vector<uint32_t> words;
    glslang::SpvOptions options;
    options.disableOptimizer = true;
    glslang::GlslangToSpv(*program.getIntermediate(stage), words, &options);
    return words;
}

int main(int argc, char **argv)
{
    try {
        require(argc == 2, "Expected artifact directory");
        initialize();
        const char *types[] = {"double", "dvec4", "dmat2"};
        int accepted = 0, rejected = 0;
        for (int stage = 0; stage < 2; ++stage) for (int shape = 0; shape < 3; ++shape)
            for (int conversion = 0; conversion < 2; ++conversion) {
                std::string type = types[shape];
                std::string value = conversion ? "i+1" : "1.0lf";
                std::string source = "#version 450\nlayout(std140,set=0,binding=0) uniform Params { " +
                    type + " uniform_array[4]; };layout(location=0) flat " +
                    std::string(stage ? "in" : "out") + " int status;";
                if (stage) source += "layout(location=0) out vec4 color;";
                source += "void main(){int result=1;for(int i=0;i<4;++i)if(" + type + "(" + value +
                    ")!=uniform_array[i])result=0;";
                source += stage ? "color=vec4(result*status);}" : "status=result;gl_Position=vec4(0);}";
                auto words = uniform_spirv(source, stage ? EShLangFragment : EShLangVertex);
                bool supported = glm_fp64_supported(words);
                bool expected = true;
                require(supported == expected, "Eligibility changed for " + type);
                spirv_cross::Compiler reflection(words);
                auto resource = reflection.get_shader_resources().uniform_buffers[0];
                auto block = reflection.get_type(resource.base_type_id);
                auto field = reflection.get_type(block.member_types[0]);
                std::string name = std::string(stage ? "fs_" : "vs_") + type + (conversion ? "_int" : "_literal");
                std::ofstream(std::string(argv[1]) + "/" + name + ".glsl") << source;
                std::ofstream binary(std::string(argv[1]) + "/" + name + ".spv", std::ios::binary);
                binary.write(reinterpret_cast<const char *>(words.data()), words.size() * 4);
                std::cout << name << " supported=" << supported << " stride=" <<
                    reflection.type_struct_member_array_stride(block, 0) << " block_size=" <<
                    reflection.get_declared_struct_size(block) << " type=" << field.basetype << '\n';
                if (!supported) { ++rejected; continue; }
                GLMCompilerMSL msl(words);
                auto options = msl.get_msl_options();
                options.set_msl_version(2, 3);
                msl.set_msl_options(options);
                std::ofstream(std::string(argv[1]) + "/" + name + ".metal") << msl.compile();
                ++accepted;
            }
        glm_compile_request request = {};
        request.sources[GLM_STAGE_VERTEX] = "#version 410 core\nvoid main(){gl_Position=vec4(0);}";
        request.sources[GLM_STAGE_FRAGMENT] = "#version 410 core\nlayout(std140) uniform Params {dvec4 uniform_array[4];};out vec4 color;void main(){color=vec4(uniform_array[0]==dvec4(1.0lf));}";
        Result result;
        compile_uncached(&request, &result.value);
        require(result.value.ok, result.value.log ? result.value.log : "Full program failed");
        bool found = false;
        for (int i = 0; i < result.value.block_uniform_count; ++i) {
            const auto &uniform = result.value.block_uniforms[i];
            if (std::string(uniform.name).find("uniform_array") == std::string::npos) continue;
            std::cout << "full VS/FS current reflection type=" << uniform.type << " array_stride=" << uniform.array_stride << '\n';
            require(uniform.type == 0x8FFE && uniform.array_stride == 32, "Exact VS/FS double-vector layout lost");
            found = true;
        }
        require(found, "Uniform reflection missing");
        std::ofstream(std::string(argv[1]) + "/program_vs.metal") << result.value.msl[GLM_STAGE_VERTEX];
        std::ofstream(std::string(argv[1]) + "/program_fs.metal") << result.value.msl[GLM_STAGE_FRAGMENT];
        std::cout << accepted << " accepted controls, " << rejected << " expected unsupported fixtures\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
