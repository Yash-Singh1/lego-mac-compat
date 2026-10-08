#define main glm_clip_probe_main
#include "shader_clip_cpu.cpp"
#undef main

static void write_conversion_host(const std::string &directory)
{
    std::string body = glm_fp64_conversion_scalar_helpers();
    body = std::regex_replace(body, std::regex("\\bulong\\b"), "uint64_t");
    body = std::regex_replace(body, std::regex("\\buint\\b"), "uint32_t");
    std::ofstream file(directory + "/conversion_host.cpp");
    file << "#include <cstdint>\n#include <cstring>\n#include <random>\n#include <iostream>\n"
            "inline uint32_t clz(uint32_t v){return __builtin_clz(v);}\n" << body;
    file << R"CPP(
static bool check(uint32_t value) {
    double unsigned_reference = static_cast<double>(value);
    int32_t signed_value;
    memcpy(&signed_value, &value, 4);
    double signed_reference = static_cast<double>(signed_value);
    uint64_t unsigned_bits, signed_bits;
    memcpy(&unsigned_bits, &unsigned_reference, 8);
    memcpy(&signed_bits, &signed_reference, 8);
    if (unsigned_bits != glm_fp64_from_uint(value) || signed_bits != glm_fp64_from_int(signed_value)) {
        std::cerr << "Conversion bits differ for " << value << '\n'; return false;
    }
    return true;
}
int main() {
    for (uint32_t i=0;i<65536;++i) if(!check(i)||!check(0u-i)) return 1;
    for (uint32_t i=0;i<32;++i) {
        uint32_t bit=1u<<i;
        if(!check(bit)||!check(bit-1)||!check(bit+1)||!check(0u-bit)) return 1;
    }
    if(!check(UINT32_MAX)||!check(0x80000000u)) return 1;
    std::mt19937 random(0x64);
    for(int i=0;i<100000;++i) if(!check(random())) return 1;
    std::cout << "Emitted integer conversions match host binary64 bits for 231202 edge/random inputs\n";
}
)CPP";
}

int main(int argc, char **argv)
{
    try {
        require(argc == 2, "Expected artifact directory");
        initialize();
        write_conversion_host(argv[1]);
        Artifacts artifacts(argv[1]);
        for (int lanes = 1; lanes <= 4; ++lanes) for (int mode = 0; mode < 3; ++mode) {
            std::string type = lanes == 1 ? "double" : "dvec" + std::to_string(lanes);
            std::string source = "#version 410 core\n";
            if (mode == 0) source += "layout(std140) uniform Params { float before; " + type + " values[4]; float after; };";
            else {
                std::string integer = lanes == 1 ? (mode == 1 ? "int" : "uint") :
                    std::string(mode == 1 ? "ivec" : "uvec") + std::to_string(lanes);
                source += "uniform " + integer + " values[4];";
            }
            source += "flat out " + type + " result;flat out uint valid;void main(){int i=gl_VertexID;";
            if (mode == 0) {
                source += "result=abs(values[i]);valid=(before==0.25 && after==0.75 && ";
                source += lanes == 1 ? "values[i]==-result" : "all(equal(values[i],-result))";
                source += ")?1u:0u;";
            } else source += "result=" + type + "(values[i]);valid=1u;";
            source += "gl_Position=vec4(0);}";
            const char *varyings[] = {"result", "valid", "gl_SkipComponents1"};
            glm_compile_request request = {};
            request.sources[GLM_STAGE_VERTEX] = source.c_str();
            request.feedback_varyings = varyings; request.feedback_count = 3; request.feedback_interleaved = true;
            Result result;
            compile_uncached(&request, &result.value);
            require(result.value.ok, result.value.log ? result.value.log : "FP64 foundation compile failed");
            require(result.value.xfb_count > 0 && result.value.xfb[0].type ==
                    uint32_t(lanes == 1 ? 0x140A : 0x8FFC + lanes - 2), "Exact feedback type lost");
            require(result.value.xfb_stride[0] == lanes * 2 + 2, "Exact feedback stride lost");
            std::string name = std::to_string(mode) + "_" + std::to_string(lanes);
            artifacts.write(name + "_normal", result.value.msl[GLM_STAGE_VERTEX]);
            artifacts.write(name + "_capture", result.value.msl_capture);
            if (mode) require(strstr(result.value.msl_capture, mode == 1 ? " = glm_fp64_from_int(" :
                        " = glm_fp64_from_uint("), "Dynamic conversion missing");
            check_cache(result.value);
        }
        glm_compile_request unsupported = {};
        unsupported.sources[GLM_STAGE_VERTEX] = "#version 410 core\nuniform float input_value;flat out double result;void main(){result=double(input_value);gl_Position=vec4(0);}";
        const char *varying = "result";
        unsupported.feedback_varyings = &varying; unsupported.feedback_count = 1; unsupported.feedback_interleaved = true;
        Result fallback;
        compile_uncached(&unsupported, &fallback.value);
        require(fallback.value.ok && fallback.value.xfb_count == 1 && fallback.value.xfb[0].type == 0x1406,
                "Unsupported float-to-double conversion must retain existing fallback");
        std::cout << "Twelve actual compiler fixtures preserve FP64 storage and capture; " << artifacts.count << " Metal stages\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
