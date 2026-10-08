// Reuse the actual compiler/cache setup and comparisons from the clipping probe.
#define main glm_clip_probe_main
#include "shader_clip_cpu.cpp"
#undef main

int main(int argc, char **argv)
{
    try {
        require(argc == 2, "Usage: fp32_division_compiler ARTIFACT_DIRECTORY");
        initialize();
        const char *names[] = {"constant", "precise_constant", "dynamic_scalar", "precise_dynamic_vector"};
        const char *bodies[] = {
            "float q=gl_FragCoord.x/510.0;vec2 r=gl_FragCoord.xy/vec2(127,7);color=vec4(q,r,1);",
            "precise float q=gl_FragCoord.x/255.0;color=vec4(q);",
            "float q=gl_FragCoord.x/denominators.x;color=vec4(q);",
            "precise vec2 q=gl_FragCoord.xy/denominators;vec2 r=gl_FragCoord.yx/denominators;color=vec4(q,r);"
        };
        Artifacts artifacts(argv[1]);
        for (int i = 0; i < 4; ++i) {
            const std::string fs = std::string("#version 410 core\nuniform vec2 denominators;out vec4 color;void main(){") + bodies[i] + "}";
            glm_compile_request request = {};
            request.sources[GLM_STAGE_VERTEX] = "#version 410 core\nvoid main(){gl_Position=vec4(float(gl_VertexID),0,0,1);}";
            request.sources[GLM_STAGE_FRAGMENT] = fs.c_str();
            Result result;
            compile_uncached(&request, &result.value);
            require(result.value.ok, result.value.log ? result.value.log : "Division compilation failed");
            const std::string msl = result.value.msl[GLM_STAGE_FRAGMENT];
            const auto body = msl.substr(msl.find("fragment "));
            if (i == 0) require(body.find("0.001960784429684281") != std::string::npos, "Constant reciprocal bits changed");
            if (i == 1) require(body.find("glm_fp32_precise_mul(") != std::string::npos, "Precise constant lost isolated multiplication");
            if (i == 2) require(body.find("glm_fp32_div(") != std::string::npos, "Dynamic division helper missing");
            if (i == 3) require(body.find("glm_fp32_precise_div(") != std::string::npos, "Precise dynamic division helper missing");
            check_cache(result.value);
            artifacts.write(std::string(names[i])+"-vertex", result.value.msl[GLM_STAGE_VERTEX]);
            artifacts.write(std::string(names[i])+"-fragment", result.value.msl[GLM_STAGE_FRAGMENT]);
        }
        require(artifacts.count == 8, "Unexpected division stage count");
        std::cout << "Four division compiler/cache fixtures passed; eight Metal stages generated\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
