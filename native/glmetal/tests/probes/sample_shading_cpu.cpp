#define main glm_clip_probe_main
#include "shader_clip_cpu.cpp"
#undef main
#include "../../src/shader_depth.cpp"

int main(int argc, char **argv)
{
    try {
        require(argc == 2, "Usage: sample_shading_cpu ARTIFACT_DIRECTORY");
        initialize(); Artifacts artifacts(argv[1]);
        const char *names[] = {"sample_id", "sample_position", "forced_inputs", "forced_flat"};
        for (int i = 0; i < 4; ++i) {
            const char *vs = "#version 410 core\nout vec2 smoothValue;noperspective out vec2 linearValue;centroid out vec2 centroidValue;flat out int tag;void main(){gl_Position=vec4(float(gl_VertexID),0,0,1);smoothValue=vec2(.3);linearValue=vec2(.4);centroidValue=vec2(.5);tag=2;}";
            std::string fs = "#version 410 core\nin vec2 smoothValue;noperspective in vec2 linearValue;centroid in vec2 centroidValue;flat in int tag;out vec4 color;void main(){color=";
            fs += i == 3 ? "vec4(float(tag),gl_FragCoord.xy,1);}" :
                std::string("vec4(smoothValue+linearValue+centroidValue,float(tag),gl_FragCoord.x)") +
                (i == 0 ? "+vec4(float(gl_SampleID));}" : i == 1 ? "+vec4(gl_SamplePosition,0,0);}" : ";}");
            glm_compile_request request = {}; request.sources[GLM_STAGE_VERTEX] = vs; request.sources[GLM_STAGE_FRAGMENT] = fs.c_str();
            Result result; compile_uncached(&request, &result.value);
            require(result.value.ok, result.value.log ? result.value.log : "Sample shader compile failed");
            const std::string original = result.value.msl[GLM_STAGE_FRAGMENT];
            if (i < 2) {
                require(original.find("sample_perspective") != std::string::npos, "Implicit smooth sample interpolation missing");
                require(original.find("sample_no_perspective") != std::string::npos, "Implicit linear sample interpolation missing");
            }
            require(original.find("centroid_perspective") != std::string::npos || i == 3, "Centroid qualifier changed");
            char *patched = glm_sample_shading_msl(original.c_str(), "main0");
            require(patched, "State sample patch failed");
            const std::string changed(patched);
            if (i == 2) require(changed.find("sample_no_perspective") != std::string::npos, "Forced linear sample interpolation missing");
            if (i == 3) require(changed.find("glm_forced_sample_id [[sample_id]]") != std::string::npos, "No-input execution forcing missing");
            artifacts.write(std::string(names[i])+"-vertex", result.value.msl[GLM_STAGE_VERTEX]);
            artifacts.write(std::string(names[i])+"-fragment", original.c_str());
            artifacts.write(std::string(names[i])+"-forced", patched); free(patched);
            check_cache(result.value);
        }
        require(!glm_sample_shading_msl("fragment float4 absent(){}", "missing"), "Missing entry accepted");
        require(artifacts.count == 12, "Unexpected sample stage count");
        std::cout << "Four sample interpolation/cache fixtures passed; twelve stages generated\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
