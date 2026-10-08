#define main glm_clip_probe_main
#include "shader_clip_cpu.cpp"
#undef main

int main(int argc, char **argv)
{
    try {
        require(argc == 2, "Expected artifact directory");
        initialize();
        for (int feedback = 0; feedback < 2; ++feedback) {
            glm_compile_request request = {};
            request.sources[GLM_STAGE_VERTEX] = "#version 410 core\nuniform double value;flat out double unused_value;void main(){unused_value=abs(value);gl_Position=vec4(0);}";
            request.sources[GLM_STAGE_FRAGMENT] = "#version 410 core\nout vec4 color;void main(){color=vec4(1);}";
            const char *varying = "unused_value";
            if (feedback) { request.feedback_varyings = &varying; request.feedback_count = 1; request.feedback_interleaved = true; }
            Result result;
            compile_uncached(&request, &result.value);
            require(result.value.ok, result.value.log ? result.value.log : "Unused output compile failed");
            require(!strstr(result.value.msl[GLM_STAGE_VERTEX], "glm_fp64_abs("), "Unused double raster output must select whole-program fallback");
            require(!strstr(result.value.msl[GLM_STAGE_VERTEX], "ulong unused_value"), "Invalid UInt64 raster output retained");
            if (feedback) require(result.value.xfb_count == 1 && result.value.xfb[0].type == 0x1406 &&
                                  result.value.xfb_stride[0] == 1, "Original feedback fallback layout differs");
            std::string name = std::string(argv[1]) + "/unused_" + std::to_string(feedback);
            std::ofstream(name + "_vs.metal") << result.value.msl[GLM_STAGE_VERTEX];
            std::ofstream(name + "_fs.metal") << result.value.msl[GLM_STAGE_FRAGMENT];
            if (result.value.msl_capture) std::ofstream(name + "_capture.metal") << result.value.msl_capture;
            std::cout << "feedback=" << feedback << " exact_helper=" << (strstr(result.value.msl[GLM_STAGE_VERTEX], "glm_fp64_abs(") != nullptr)
                      << " xfb_type=" << (result.value.xfb_count ? result.value.xfb[0].type : 0) << '\n';
        }
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
