#define main glm_clip_probe_main
#include "shader_clip_cpu.cpp"
#undef main

static void exact_block(const glm_compile_result &result, bool vector)
{
    bool found = false;
    for (int i = 0; i < result.block_uniform_count; ++i) {
        const auto &uniform = result.block_uniforms[i];
        if (std::string(uniform.name).find("values") == std::string::npos) continue;
        require(uniform.type == uint32_t(vector ? 0x8FFE : 0x140A) &&
                uniform.array_stride == (vector ? 32 : 16), "Clipping child lost exact UBO type/stride");
        found = true;
    }
    require(found, "Exact clip UBO reflection missing");
}

int main(int argc, char **argv)
{
    try {
        require(argc == 2, "Expected artifact directory");
        initialize();
        Artifacts artifacts(argv[1]);
        for (int mode = 0; mode < 4; ++mode) {
            bool fragment = mode & 1, vector = mode & 2;
            std::string block = vector ? "layout(std140) uniform Values {dvec4 values[2];};" :
                                         "layout(std140) uniform Values {double values[4];};";
            std::string check = vector ? "values[0]==dvec4(1.0000000000000002lf)" :
                                         "values[0]==1.0000000000000002lf";
            std::string vertex = "#version 410 core\nlayout(location=0) in vec2 position;";
            std::string fs = "#version 410 core\nout vec4 color;";
            if (!fragment) {
                vertex += block + "flat out int verdict;void main(){verdict=" + check + "?1:0;";
                fs += "flat in int verdict;void main(){color=verdict==1?vec4(0,1,0,1):vec4(1,0,0,1);}";
            } else {
                vertex += "void main(){";
                fs += block + "void main(){color=" + check + "?vec4(0,1,0,1):vec4(1,0,0,1);}";
            }
            vertex += "gl_Position=vec4(position,0,1);gl_ClipDistance[0]=position.x+0.2;}";
            glm_compile_request request = {};
            request.sources[GLM_STAGE_VERTEX] = vertex.c_str(); request.sources[GLM_STAGE_FRAGMENT] = fs.c_str();
            Result result; compile_uncached(&request, &result.value);
            require(result.value.ok, result.value.log ? result.value.log : "Exact clip program failed");
            exact_block(result.value, vector);
            auto *clip = result.value.clip;
            require(clip && clip->vs_capture && clip->kernel && clip->kernel->ok && clip->pull && clip->pull->ok,
                    "Exact program lost clipping stages");
            if (fragment) exact_block(*clip->pull, vector);
            const char *exact_msl = fragment ? clip->pull->msl[GLM_STAGE_FRAGMENT] : clip->vs_capture;
            require(strstr(exact_msl, " = glm_fp64_eq("), "Clipping child lost exact numeric operation");
            std::vector<std::string> names = {"gl_Position", "gl_ClipDistance[0]"};
            for (int i = 0; i < clip->varying_count; ++i) names.push_back(clip->varyings[i].name);
            std::vector<const char *> pointers;
            for (auto &name : names) pointers.push_back(name.c_str());
            auto capture_request = request;
            capture_request.feedback_varyings = pointers.data(); capture_request.feedback_count = int(pointers.size());
            capture_request.feedback_interleaved = true;
            Result capture; compile_program(&capture_request, &capture.value, true, true);
            require(capture.value.ok, "Exact internal capture reflection fixture failed");
            exact_block(capture.value, vector);
            std::string name = std::to_string(mode);
            artifacts.write(name + "_vs", result.value.msl[GLM_STAGE_VERTEX]);
            artifacts.write(name + "_fs", result.value.msl[GLM_STAGE_FRAGMENT]);
            artifacts.write(name + "_capture", clip->vs_capture);
            artifacts.write(name + "_kernel", clip->kernel->msl[GLM_STAGE_COMPUTE]);
            artifacts.write(name + "_pull_vs", clip->pull->msl[GLM_STAGE_VERTEX]);
            artifacts.write(name + "_pull_fs", clip->pull->msl[GLM_STAGE_FRAGMENT]);
            check_cache(result.value);
        }
        std::cout << "Four exact FP64 clipping fixtures preserve child UBO/capture layout; " << artifacts.count << " Metal stages\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
