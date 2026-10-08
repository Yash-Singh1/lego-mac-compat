#define main glm_clip_probe_main
#include "shader_clip_cpu.cpp"
#undef main

int main(int argc, char **argv)
{
    try {
        require(argc == 2, "Expected artifact directory");
        initialize();
        std::ofstream manifest(std::string(argv[1]) + "/manifest.txt");
        int written = 0;
        auto write = [&](const std::string &name, const char *source) {
            require(source != nullptr, "Missing generated Metal stage");
            std::ofstream(std::string(argv[1]) + "/" + name + ".metal") << source;
            manifest << name << ".metal\n";
            ++written;
        };
        for (int lanes = 2; lanes <= 4; ++lanes) {
            std::string type = "vec" + std::to_string(lanes);
            for (int style = 0; style < 4; ++style) {
                std::string body;
                if (style == 0) body = "result=" + type + "(normalize(input_value));";
                if (style == 1) body = "result=norm(input_value);";
                if (style == 2) body = "result=gl_VertexID==0?normalize(input_value):normalize(-input_value);";
                if (style == 3) body = "result=normalize(next_value());";
                std::string source = "#version 410 core\nuniform " + type + " input_value;"
                    "int calls=0;" + type + " norm(" + type + " x){return normalize(x);}"
                    + type + " next_value(){calls++;return input_value;}"
                    "out " + type + " result;flat out int count_value;"
                    "void main(){" + body + "count_value=calls;gl_Position=vec4(result.x,0,0,1);}";
                const char *varyings[] = {"result", "count_value"};
                glm_compile_request request = {};
                request.sources[GLM_STAGE_VERTEX] = source.c_str();
                std::string fragment_body = body;
                size_t vertex_id = fragment_body.find("gl_VertexID");
                if (vertex_id != std::string::npos)
                    fragment_body.replace(vertex_id, strlen("gl_VertexID"), "int(gl_FragCoord.x)");
                std::string fragment_source = "#version 410 core\nuniform " + type + " input_value;"
                    "int calls=0;" + type + " norm(" + type + " x){return normalize(x);}"
                    + type + " next_value(){calls++;return input_value;}"
                    "out " + type + " result;out int count_value;"
                    "void main(){" + fragment_body + "count_value=calls;}";
                request.sources[GLM_STAGE_FRAGMENT] = fragment_source.c_str();
                request.feedback_varyings = varyings;
                request.feedback_count = 2;
                request.feedback_interleaved = true;
                Result result;
                compile_uncached(&request, &result.value);
                require(result.value.ok, result.value.log ? result.value.log : "Normalize compile failed");
                for (const char *stage : {result.value.msl[GLM_STAGE_VERTEX], result.value.msl_capture}) {
                    require(stage && strstr(stage, "fast::normalize("), "Vertex native normalization changed");
                    require(!strstr(stage, " = glm_fp32_normalize(") && !strstr(stage, "return glm_fp32_normalize("),
                            "Fragment normalization leaked into vertex/capture");
                }
                const char *fragment_stage = result.value.msl[GLM_STAGE_FRAGMENT];
                require(fragment_stage && !strstr(fragment_stage, "fast::normalize("), "Fragment fast normalization survived");
                require(strstr(fragment_stage, " = glm_fp32_normalize(") || strstr(fragment_stage, "return glm_fp32_normalize("),
                        "Fragment normalization helper missing");
                require(strstr(fragment_stage, "rsqrt(length2)"), "Ordered binary32 sum/rsqrt helper missing");
                if (style == 3) {
                    const char *stage = fragment_stage;
                    const char *call = strstr(stage, "next_value(");
                    require(call && strstr(call + 1, "next_value(") &&
                            !strstr(strstr(call + 1, "next_value(") + 1, "next_value("),
                            "Side-effect operand call was duplicated");
                }
                std::string name = "normalize_" + std::to_string(lanes) + "_" + std::to_string(style);
                write(name + "_normal", result.value.msl[GLM_STAGE_VERTEX]);
                write(name + "_capture", result.value.msl_capture);
                write(name + "_fragment", fragment_stage);
                if (style == 0) {
                    std::string metal = fragment_stage;
                    std::string vector = "float" + std::to_string(lanes);
                    size_t begin = metal.find("inline " + vector + " glm_fp32_normalize(");
                    size_t end = metal.find("\n}", begin);
                    require(begin != std::string::npos && end != std::string::npos, "Missing normalization helper definition");
                    std::string kernel = "#include <metal_stdlib>\nusing namespace metal;\n" +
                        metal.substr(begin, end + 2 - begin) +
                        "\nkernel void normalize_ir(device const " + vector + "* input [[buffer(0)]], device " + vector +
                        "* output [[buffer(1)]], uint index [[thread_position_in_grid]]) { output[index] = glm_fp32_normalize(input[index]); }\n";
                    write("normalize_ir_" + std::to_string(lanes), kernel.c_str());
                }
                check_cache(result.value);
            }
        }
        glm_compile_request scalar = {};
        scalar.sources[GLM_STAGE_VERTEX] = "#version 410 core\nuniform float x;out float result;void main(){result=normalize(x);gl_Position=vec4(result);}";
        Result result;
        compile_uncached(&scalar, &result.value);
        require(result.value.ok && strstr(result.value.msl[GLM_STAGE_VERTEX], "sign("), "Scalar normalize lowering changed");
        write("scalar", result.value.msl[GLM_STAGE_VERTEX]);
        std::cout << "Twelve float-vector fixtures plus scalar unchanged; " << written << " offline Metal stages\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
