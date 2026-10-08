#define main glm_clip_probe_main
#include "shader_clip_cpu.cpp"
#undef main

static void write_spirv(const std::string &source, bool capture, const std::string &path)
{
    Bindings bindings;
    bindings.capture = capture;
    bindings.feedback.push_back("tag");
    bindings.interleaved = true;
    Prepared prepared = prepare(source.c_str(), GLM_STAGE_VERTEX, &bindings);
    glslang::TShader shader(EShLangVertex);
    const char *text = prepared.text.c_str();
    shader.setStrings(&text, 1);
    configure(shader, EShLangVertex);
    require(shader.parse(shader_resources(source.c_str()), 410, false, messages), shader.getInfoLog());
    glslang::TProgram program;
    program.addShader(&shader);
    require(program.link(messages), program.getInfoLog());
    program.mapIO();
    std::vector<uint32_t> words;
    glslang::SpvOptions options;
    options.disableOptimizer = true;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangVertex), words, &options);
    auto mapped = vertex_id_spirv(words);
    require(mapped.size() > words.size(), "Original-ID helper was not injected");
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(mapped.data()), mapped.size() * sizeof(uint32_t));
    require(output.good(), "SPIR-V artifact write failed");
}

int main(int argc, char **argv)
{
    try {
        require(argc == 2, "Usage: vertex_id_mapping_cpu ARTIFACT_DIRECTORY");
        initialize();
        Artifacts artifacts(argv[1]);
        const char *names[] = {"direct", "helper", "global", "loop", "conditional", "nested"};
        const char *bodies[] = {
            "void main(){tag=gl_VertexID;gl_Position=vec4(tag);}",
            "int id(){return gl_VertexID;}void main(){tag=id();gl_Position=vec4(tag);}",
            "int id=gl_VertexID;void main(){tag=id;gl_Position=vec4(tag);}",
            "void main(){tag=0;for(int i=0;i<3;++i){tag+=gl_VertexID;}gl_Position=vec4(tag);}",
            "void main(){tag=(gl_VertexID==2)?gl_VertexID+1:gl_VertexID-1;gl_Position=vec4(tag);}",
            "int id(){return gl_VertexID;}int other(){int x=id();if(x>1){for(int i=0;i<2;++i)x+=id();}return x;}void main(){tag=other();gl_Position=vec4(tag);}"
        };
        for (int version : {140, 410}) {
            for (int i = 0; i < 6; ++i) {
                std::string source = "#version " + std::to_string(version) + "\nout int tag;\n" + bodies[i];
                const char *varying = "tag";
                glm_compile_request request = {};
                request.sources[GLM_STAGE_VERTEX] = source.c_str();
                request.feedback_count = 1;
                request.feedback_varyings = &varying;
                request.feedback_interleaved = true;
                Result result;
                compile_uncached(&request, &result.value);
                require(result.value.ok, result.value.log ? result.value.log : "ID mapping compilation failed");
                require(result.value.msl_capture != nullptr, "Missing capture variant");
                std::string label = std::string(names[i]) + "-" + std::to_string(version);
                for (int capture = 0; capture < 2; ++capture) {
                    const char *msl = capture ? result.value.msl_capture : result.value.msl[GLM_STAGE_VERTEX];
                    require(msl && std::string(msl).find("[[buffer(30)]]") != std::string::npos, "Missing hidden stream binding");
                    require(std::string(msl).find("glm_original_vertex_id") != std::string::npos, "Missing user-ID mapping function");
                    std::string name = label + (capture ? "-capture" : "-normal");
                    artifacts.write(name, msl);
                    write_spirv(source, capture, std::string(argv[1]) + "/" + name + ".spv");
                }
            }
        }
        glm_compile_request raw = {};
        raw.sources[GLM_STAGE_VERTEX] = "#version 410 core\n#define GLM_RAW_VERTEX_ID\nvoid main(){gl_Position=vec4(float(gl_VertexID));}";
        Result result;
        compile_uncached(&raw, &result.value);
        require(result.value.ok, result.value.log ? result.value.log : "Raw-ID fixture failed");
        require(std::string(result.value.msl[GLM_STAGE_VERTEX]).find("glm_original_vertex_id") == std::string::npos,
                "Generated raw-ID optout changed");
        artifacts.write("raw-optout", result.value.msl[GLM_STAGE_VERTEX]);
        std::cout << "Original-ID mapping: 12 normal/capture fixtures and raw optout passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
