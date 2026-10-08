#define main glm_clip_probe_main
#include "shader_clip_cpu.cpp"
#undef main

int main()
{
    try {
        initialize();
        const char *core = "#version 410 core\nfloat proof[(gl_MaxVaryingVectors==15 && gl_MaxVaryingComponents==60)?1:-1];void main(){gl_Position=vec4(0);}";
        char *log = nullptr;
        require(shader_check_uncached(GLM_STAGE_VERTEX, core, &log), log ? log : "Core constants differ");
        free(log);
        const char *legacy = "#version 120\nfloat proof[(gl_MaxVaryingFloats==64)?1:-1];void main(){gl_Position=gl_Vertex;}";
        log = nullptr;
        require(shader_check_uncached(GLM_STAGE_VERTEX, legacy, &log), log ? log : "Legacy resources changed");
        free(log);
        require(shader_resources("#version 300 es\n") == GetDefaultResources(), "ES resources changed");
        require(shader_resources("#version 410 compatibility\n") == GetDefaultResources(), "Compatibility resources changed");
        const char *varying = "tag";
        glm_compile_request request = {};
        request.sources[GLM_STAGE_VERTEX] = "#version 410 core\nfloat proof[(gl_MaxVaryingVectors==15 && gl_MaxVaryingComponents==60)?1:-1];uniform int count=gl_MaxVaryingVectors;out vec2 tag;void main(){tag=vec2(count,gl_MaxVaryingComponents);gl_Position=vec4(0);}";
        request.feedback_count = 1;
        request.feedback_varyings = &varying;
        request.feedback_interleaved = true;
        Result result;
        compile_uncached(&request, &result.value);
        require(result.value.ok, result.value.log ? result.value.log : "Core feedback constants compilation failed");
        require(result.value.msl_capture != nullptr, "Core capture variant failed");
        bool initializer_found = false;
        for (int i = 0; i < result.value.uniform_count; ++i) {
            const auto &uniform = result.value.uniforms[i];
            if (std::string(uniform.name) != "count") continue;
            int count = 0;
            require(result.value.initial_globals != nullptr, "Missing initializer storage");
            memcpy(&count, result.value.initial_globals + uniform.offset, sizeof count);
            require(count == 15, "Generated initializer builtin differs");
            initializer_found = true;
        }
        require(initializer_found, "Builtin initializer uniform was not reflected");
        std::cout << "Varying constants: core check/normal/capture passed; legacy and ES resources preserved\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
