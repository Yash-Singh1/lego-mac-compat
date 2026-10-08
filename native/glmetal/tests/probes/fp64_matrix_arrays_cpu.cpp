#define main glm_clip_probe_main
#include "shader_clip_cpu.cpp"
#undef main

int main(int argc, char **argv)
{
    try {
        require(argc == 2, "Expected artifact directory");
        initialize();
        Artifacts artifacts(argv[1]);
        for (int columns = 2; columns <= 4; ++columns) for (int rows = 2; rows <= 4; ++rows)
            for (int mode = 0; mode < 4; ++mode) {
                std::string type = "dmat" + std::to_string(columns) + "x" + std::to_string(rows);
                std::string source = "#version 410 core\nlayout(std140) uniform Params {float before;" + type +
                    " values[3];float after;};flat out " + type + " result;flat out uint valid;";
                if (mode == 0) source += type + " copyValue(int i){" + type + " local[3]=values;return local[i];}";
                if (mode == 1) {
                    source += type + " copyValue(int i){" + type + " value=values[i];return " + type + "(";
                    for (int c = 0; c < columns; ++c) source += (c ? "," : "") + std::string("value[") + std::to_string(c) + "]";
                    source += ");}";
                }
                source += "void main(){int i=gl_VertexID%3;";
                if (mode < 2) source += "result=copyValue(i);";
                if (mode == 2) source += type + " local[2];local[0]=" + type + "(2.0lf);local[1]=values[i];result=local[i%2];";
                if (mode == 3) {
                    source += "result=" + type + "(";
                    for (int element = 0; element < columns * rows; ++element)
                        source += (element ? "," : "") + std::string("i*") + std::to_string(columns * rows) + "+" + std::to_string(element + 1);
                    source += ");";
                }
                source += "valid=(before==0.25 && after==0.75 && result==values[i])?1u:0u;gl_Position=vec4(0);}";
                const char *varyings[] = {"result", "valid", "gl_SkipComponents1"};
                glm_compile_request request = {};
                request.sources[GLM_STAGE_VERTEX] = source.c_str(); request.feedback_varyings = varyings;
                request.feedback_count = 3; request.feedback_interleaved = true;
                Result result;
                compile_uncached(&request, &result.value);
                require(result.value.ok, result.value.log ? result.value.log : "Matrix array compilation failed");
                const uint32_t types[3][3] = {{0x8F46,0x8F49,0x8F4A},{0x8F4B,0x8F47,0x8F4C},{0x8F4D,0x8F4E,0x8F48}};
                require(result.value.xfb_count > 0 && result.value.xfb[0].type == types[columns-2][rows-2], "Exact matrix feedback type lost");
                require(result.value.xfb_stride[0] == columns * rows * 2 + 2, "Exact matrix feedback stride lost");
                int column_stride = rows == 2 ? 16 : 32, array_stride = columns * column_stride;
                bool found = false;
                for (int i = 0; i < result.value.block_uniform_count; ++i) {
                    const auto &uniform = result.value.block_uniforms[i];
                    if (std::string(uniform.name).find("values") == std::string::npos) continue;
                    require(uniform.type == types[columns-2][rows-2] && uniform.array_stride == array_stride &&
                            uniform.matrix_stride == column_stride && uniform.offset == column_stride && !uniform.row_major,
                            "Exact matrix UBO layout lost");
                    found = true;
                }
                require(found, "Matrix UBO reflection absent");
                std::string name = std::to_string(columns) + "x" + std::to_string(rows) + "_" + std::to_string(mode);
                artifacts.write(name + "_normal", result.value.msl[GLM_STAGE_VERTEX]);
                artifacts.write(name + "_capture", result.value.msl_capture);
                if (mode == 0) check_cache(result.value);
            }
        glm_compile_request request = {};
        request.sources[GLM_STAGE_VERTEX] = "#version 410 core\nlayout(std140,row_major) uniform Params {dmat2 values[3];};flat out dmat2 result;void main(){result=values[gl_VertexID%3];gl_Position=vec4(0);}";
        const char *varying = "result"; request.feedback_varyings = &varying; request.feedback_count = 1; request.feedback_interleaved = true;
        Result fallback; compile_uncached(&request, &fallback.value);
        require(fallback.value.ok && fallback.value.xfb[0].type == 0x8B5A, "Row-major must retain existing fallback");
        std::cout << "36 matrix-array fixtures, " << artifacts.count << " Metal stages, row-major fallback retained\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
