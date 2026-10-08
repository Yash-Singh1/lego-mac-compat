// CPU-only compiler/cache regression. Including the implementation exposes
// internal compile entry points without linking or executing the GL driver.
#include "../../src/shader_compiler.cpp"
#include "../../src/compile_cache.cpp"

#include <fstream>
#include <iostream>
#include <stdexcept>

extern "C" bool glm_remote_compile(const glm_compile_request *, glm_compile_result *) { return false; }
extern "C" bool glm_remote_check(glm_stage, const char *, bool *, char **) { return false; }

namespace {
struct Result {
    glm_compile_result value = {};
    ~Result() { glm_compile_result_free(&value); }
};

void require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error(message);
}

bool same_text(const char *a, const char *b)
{
    return a && b ? std::strcmp(a, b) == 0 : a == b;
}

void compare_stage_sources(const glm_compile_result &a, const glm_compile_result &b)
{
    require(a.ok == b.ok, "Cache changed compile status");
    for (int stage = 0; stage < GLM_STAGE_COUNT; ++stage)
        require(same_text(a.msl[stage], b.msl[stage]), "Cache changed stage MSL");
    require(same_text(a.msl_capture, b.msl_capture), "Cache changed user TF capture MSL");
}

void check_cache(const glm_compile_result &original)
{
    std::vector<unsigned char> bytes;
    glm_remote_encode_result(&original, &bytes);
    Result restored;
    require(glm_remote_decode_result(bytes.data(), bytes.size(), &restored.value), "Cache decode failed");
    compare_stage_sources(original, restored.value);
    const auto *a = original.clip;
    const auto *b = restored.value.clip;
    if (!a) {
        require(!b, "Cache introduced clipping into a fallback program");
        std::cout << "  cache roundtrip preserves fallback stage MSL, " << bytes.size() << " bytes\n";
        return;
    }
    require(a && b && b->kernel && b->pull, "Cache lost clipping stages");
    require(same_text(a->vs_capture, b->vs_capture), "Cache changed internal capture MSL");
    require(a->vs_stride == b->vs_stride && a->out_stride == b->out_stride &&
            a->clip_count == b->clip_count && a->primitive_id == b->primitive_id &&
            a->flat_outputs == b->flat_outputs && a->varying_count == b->varying_count,
            "Cache changed clipping metadata");
    for (int i = 0; i < a->varying_count; ++i) {
        const auto &x = a->varyings[i];
        const auto &y = b->varyings[i];
        require(same_text(x.name, y.name) && x.components == y.components &&
                x.kind == y.kind && x.interpolation == y.interpolation && x.offset == y.offset,
                "Cache changed varying layout");
    }
    compare_stage_sources(*a->kernel, *b->kernel);
    compare_stage_sources(*a->pull, *b->pull);
    require(!b->kernel->clip && !b->pull->clip, "Cache introduced recursive clipping");
    for (int i = 0; i < 4; ++i)
        require(original.xfb_stride[i] == restored.value.xfb_stride[i], "Cache changed original TF stride");
    std::cout << "  cache roundtrip preserves child MSL and metadata, " << bytes.size() << " bytes\n";
}

class Artifacts {
public:
    explicit Artifacts(const char *directory) : directory(directory), manifest(std::string(directory) + "/manifest.txt")
    {
        require(manifest.good(), "Cannot create artifact manifest");
    }
    void write(const std::string &name, const char *msl)
    {
        require(msl && *msl, "Missing MSL for " + name);
        std::ofstream file(directory + "/" + name + ".metal");
        require(file.good(), "Cannot create MSL artifact");
        file << msl;
        require(file.good(), "Cannot write MSL artifact");
        manifest << name << ".metal\n";
        ++count;
    }
    int count = 0;
private:
    std::string directory;
    std::ofstream manifest;
};

const char *const fragment = R"GLSL(#version 410 core
noperspective in vec4 shade;
flat in int tag;
out vec4 frag;
void main() { frag = shade + float(tag); }
)GLSL";

std::string vertex(bool vertex_id)
{
    return std::string(R"GLSL(#version 410 core
layout(location=0) in vec4 p;
layout(location=1) in vec4 c;
noperspective out vec4 shade;
flat out int tag;
void main() {
    gl_Position = p;
    shade = c;
    tag = )GLSL") + (vertex_id ? "gl_VertexID" : "int(c.w)") + R"GLSL(;
    gl_ClipDistance[0] = p.x + p.y + .2;
    gl_ClipDistance[1] = p.x - .3;
}
)GLSL";
}

enum Fixture { Interpolation, PrimitiveID, OriginalTF, AffineID, Legacy, GlobalID, FixtureCount };

void run_clip_fixtures(Artifacts &artifacts)
{
    const char *names[] = {"interpolation", "primitive_id", "original_tf", "affine_id", "legacy", "global_id"};
    for (int fixture = 0; fixture < FixtureCount; ++fixture) {
        std::string vs = vertex(fixture == AffineID);
        std::string fs = fragment;
        if (fixture == PrimitiveID) fs.replace(fs.find("shade + float(tag)"), 18, "shade + float(tag) + float(gl_PrimitiveID)");
        if (fixture == Legacy) {
            vs = "#version 120\nvarying vec4 color;void main(){gl_Position=gl_Vertex;gl_ClipVertex=gl_Vertex;color=gl_Color;}";
            fs = "#version 120\nvarying vec4 color;void main(){gl_FragColor=color;}";
        }
        if (fixture == GlobalID) {
            vs = "#version 410 core\nint originalGlobalID=gl_VertexID;void main(){gl_Position=vec4(float(originalGlobalID));}";
            fs = "#version 410 core\nout vec4 color;void main(){color=vec4(1);}";
        }
        glm_compile_request request = {};
        request.sources[GLM_STAGE_VERTEX] = vs.c_str();
        request.sources[GLM_STAGE_FRAGMENT] = fs.c_str();
        const char *feedback = fixture == AffineID ? "tag" : "shade";
        if (fixture == OriginalTF || fixture == AffineID) {
            request.feedback_count = 1;
            request.feedback_varyings = &feedback;
            request.feedback_interleaved = true;
        }
        Result result;
        compile_uncached(&request, &result.value);
        const auto &compiled = result.value;
        require(compiled.ok, std::string(names[fixture]) + ": " + (compiled.log ? compiled.log : "compile failed"));
        std::cout << names[fixture] << '\n';
        if (fixture == AffineID) {
            require(!compiled.clip, "VertexID shader unexpectedly eligible for internal clipping");
            require(compiled.msl_capture && compiled.xfb_stride[0] == 1, "Affine TF layout changed");
            const std::string msl = compiled.msl_capture;
            require(msl.find("glm_vertex_id_base") != std::string::npos, "User VertexID lost original base");
            const auto ordinal = msl.find(".glm_xfb_first");
            const auto end = msl.find(';', ordinal);
            require(ordinal != std::string::npos && end != std::string::npos, "Missing raw TF ordinal");
            const auto expression = msl.substr(ordinal, end - ordinal);
            require(expression.find("gl_VertexIndex") != std::string::npos &&
                    expression.find("glm_vertex_id_base") == std::string::npos, "TF ordinal includes user ID base");
            artifacts.write("affine-id-capture", compiled.msl_capture);
            check_cache(compiled);
            continue;
        }
        if (fixture == GlobalID) {
            require(!compiled.clip, "Global VertexID shader unexpectedly eligible for clipping");
            const std::string msl = compiled.msl[GLM_STAGE_VERTEX];
            const auto start = msl.find("int originalGlobalID =");
            const auto end = msl.find(';', start);
            require(start != std::string::npos && end != std::string::npos &&
                    msl.substr(start, end - start).find("glm_vertex_id_base") != std::string::npos,
                    "Global initializer does not evaluate raw VertexID plus original base");
            artifacts.write("global-id-vertex", compiled.msl[GLM_STAGE_VERTEX]);
            check_cache(compiled);
            continue;
        }
        const auto *clip = compiled.clip;
        require(clip && clip->vs_capture && clip->kernel && clip->kernel->ok && clip->pull && clip->pull->ok,
                "Incomplete clipping compiler result");
        require(clip->vs_stride == (fixture == Legacy ? 16 : 11) && clip->out_stride == clip->vs_stride + 1,
                "Unexpected capture/pull stride");
        require(clip->clip_count == (fixture == Legacy ? 8 : 2), "Unexpected clip-distance count");
        require(clip->primitive_id == (fixture == PrimitiveID), "PrimitiveID reflection changed");
        if (fixture != Legacy) {
            require(clip->varying_count == 2 && clip->flat_outputs, "Missing active varying metadata");
            require(clip->varyings[0].interpolation == 2 && clip->varyings[1].interpolation == 1,
                    "Noperspective/flat interpolation metadata changed");
        }
        if (fixture == OriginalTF) {
            require(compiled.msl_capture && compiled.xfb_stride[0] == 4, "Original user TF layout changed");
            // This additional stage supplements the original eighteen-stage scratch coverage.
            artifacts.write("original-user-tf-capture", compiled.msl_capture);
        }
        artifacts.write(std::string(names[fixture]) + "-capture", clip->vs_capture);
        artifacts.write(std::string(names[fixture]) + "-kernel", clip->kernel->msl[GLM_STAGE_COMPUTE]);
        artifacts.write(std::string(names[fixture]) + "-pull", clip->pull->msl[GLM_STAGE_VERTEX]);
        artifacts.write(std::string(names[fixture]) + "-fragment", clip->pull->msl[GLM_STAGE_FRAGMENT]);
        check_cache(compiled);
    }
}
} // namespace

int main(int argc, char **argv)
{
    try {
        require(argc == 2, "Usage: shader_clip_cpu ARTIFACT_DIRECTORY");
        initialize();
        Artifacts artifacts(argv[1]);
        run_clip_fixtures(artifacts);
        require(artifacts.count == 19, "Unexpected number of generated Metal stages");
        std::cout << "CPU/compiler/cache checks passed; " << artifacts.count << " Metal stages generated\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "shader_clip_cpu: " << error.what() << '\n';
        return 1;
    }
}
