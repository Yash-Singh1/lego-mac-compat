// Persistent cache of program compiles (glslang + SPIRV-Cross dominate a
// link, and games link shaders on demand, which shows as hitches). Results
// are stored under ~/Library/Caches/GLMetal by a hash of the whole request
// and this build of the compiler; programs with geometry or tessellation
// stages are not cached.
#include "shader_compiler.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" const char glm_compiler_build[];

namespace {

struct Hash {
    uint64_t h = 1469598103934665603ull;
    void bytes(const void *p, size_t n)
    {
        const unsigned char *b = static_cast<const unsigned char *>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    }
    void str(const char *s)
    {
        uint32_t n = s ? static_cast<uint32_t>(strlen(s)) + 1 : 0;
        bytes(&n, sizeof n);
        if (s) bytes(s, n);
    }
    template <typename T> void value(T v) { bytes(&v, sizeof v); }
};

uint64_t request_hash(const glm_compile_request *req)
{
    Hash h;
    h.str(glm_compiler_build);
    for (auto *s : req->sources) h.str(s);
    h.value(req->attribute_count);
    for (int i = 0; i < req->attribute_count; ++i) {
        h.str(req->attributes[i].name);
        h.value(req->attributes[i].location);
    }
    h.value(req->frag_output_count);
    for (int i = 0; i < req->frag_output_count; ++i) {
        h.str(req->frag_outputs[i].name);
        h.value(req->frag_outputs[i].location);
    }
    h.value(req->feedback_count);
    for (int i = 0; i < req->feedback_count; ++i) h.str(req->feedback_varyings[i]);
    h.value(req->feedback_interleaved);
    h.value(req->tess_output_vertices);
    h.value(req->uint_inputs);
    h.value(req->int_inputs);
    for (auto *name : req->border_samplers) h.str(name);
    return h.h;
}

std::string cache_path(const glm_compile_request *req)
{
    const char *home = getenv("HOME");
    if (!home || getenv("GLMETAL_NO_SHADER_CACHE")) return {};
    uint64_t hash = request_hash(req);
    std::string dir = std::string(home) + "/Library/Caches/GLMetal";
    mkdir(dir.c_str(), 0755);
    char name[32];
    snprintf(name, sizeof name, "/%016llx.prog", static_cast<unsigned long long>(hash));
    return dir + name;
}

struct Writer {
    std::vector<unsigned char> out;
    void bytes(const void *p, size_t n) { out.insert(out.end(), (const unsigned char *)p, (const unsigned char *)p + n); }
    template <typename T> void value(T v) { bytes(&v, sizeof v); }
    void str(const char *s)
    {
        int32_t n = s ? static_cast<int32_t>(strlen(s)) : -1;
        value(n);
        if (s) bytes(s, static_cast<size_t>(n));
    }
};

struct Reader {
    const unsigned char *p, *end;
    bool ok = true;
    void bytes(void *d, size_t n)
    {
        if (static_cast<size_t>(end - p) < n) {
            ok = false;
            memset(d, 0, n);
            return;
        }
        memcpy(d, p, n);
        p += n;
    }
    template <typename T> T value()
    {
        T v;
        bytes(&v, sizeof v);
        return v;
    }
    char *str()
    {
        int32_t n = value<int32_t>();
        if (n < 0 || !ok) return nullptr;
        if (static_cast<size_t>(end - p) < static_cast<size_t>(n)) {
            ok = false;
            return nullptr;
        }
        char *s = static_cast<char *>(malloc(static_cast<size_t>(n) + 1));
        memcpy(s, p, static_cast<size_t>(n));
        s[n] = 0;
        p += n;
        return s;
    }
};

const uint32_t magic = 0x474d5053; // GMPS: exact binary64 inverse3x3 and inverse4x4

void write_uniforms(Writer &w, const glm_uniform_info *u, int n)
{
    w.value(n);
    for (int i = 0; i < n; ++i) {
        w.str(u[i].name);
        w.value(u[i].type);
        w.value(u[i].array_size);
        w.value(u[i].offset);
        w.value(u[i].array_stride);
        w.value(u[i].matrix_stride);
        w.value(u[i].sampler_slot);
        w.value(u[i].legacy);
        w.value(u[i].block);
        w.value(u[i].row_major);
        w.value(u[i].is_array);
    }
}

glm_uniform_info *read_uniforms(Reader &r, int *count)
{
    int n = r.value<int>();
    if (!r.ok || n < 0 || n > 1 << 20) n = 0, r.ok = false;
    *count = n;
    auto *u = static_cast<glm_uniform_info *>(calloc(static_cast<size_t>(n) + 1, sizeof(glm_uniform_info)));
    for (int i = 0; i < n; ++i) {
        u[i].name = r.str();
        u[i].type = r.value<uint32_t>();
        u[i].array_size = r.value<int>();
        u[i].offset = r.value<int>();
        u[i].array_stride = r.value<int>();
        u[i].matrix_stride = r.value<int>();
        u[i].sampler_slot = r.value<int>();
        u[i].legacy = r.value<int>();
        u[i].block = r.value<int>();
        u[i].row_major = r.value<bool>();
        u[i].is_array = r.value<bool>();
    }
    return u;
}

void write_io(Writer &w, const glm_io_info *io, int n)
{
    w.value(n);
    for (int i = 0; i < n; ++i) {
        w.str(io[i].name);
        w.value(io[i].type);
        w.value(io[i].location);
        w.value(io[i].index);
        w.value(io[i].array_size);
        w.value(io[i].integer);
    }
}

glm_io_info *read_io(Reader &r, int *count)
{
    int n = r.value<int>();
    if (!r.ok || n < 0 || n > 1 << 16) n = 0, r.ok = false;
    *count = n;
    auto *io = static_cast<glm_io_info *>(calloc(static_cast<size_t>(n) + 1, sizeof(glm_io_info)));
    for (int i = 0; i < n; ++i) {
        io[i].name = r.str();
        io[i].type = r.value<uint32_t>();
        io[i].location = r.value<int>();
        io[i].index = r.value<int>();
        io[i].array_size = r.value<int>();
        io[i].integer = r.value<bool>();
    }
    return io;
}

} // namespace

static bool decode(const std::vector<unsigned char> &data, glm_compile_result *res, unsigned depth=0)
{
    memset(res,0,sizeof *res);
    if(depth>1)return false;
    Reader r{data.data(), data.data() + data.size()};
    if (r.value<uint32_t>() != magic || !r.ok) return false;
    res->ok = true;
    res->log = r.str();
    for (auto &m : res->msl) m = r.str();
    res->msl_capture = r.str();
    res->tess_inputs = r.str();
    if(r.value<bool>()){
        auto *c=static_cast<glm_clip_result *>(calloc(1,sizeof(glm_clip_result)));res->clip=c;
        c->vs_capture=r.str();c->vs_stride=r.value<int>();c->out_stride=r.value<int>();c->clip_count=r.value<int>();
        c->flat_outputs=r.value<bool>();c->primitive_id=r.value<bool>();c->varying_count=r.value<int>();
        if(c->varying_count<0||c->varying_count>64||c->clip_count<1||c->clip_count>8||c->vs_stride<5||c->vs_stride>256||c->out_stride!=c->vs_stride+1){r.ok=false;c->varying_count=0;}
        c->varyings=static_cast<glm_clip_varying *>(calloc(size_t(c->varying_count)+1,sizeof(glm_clip_varying)));
        for(int i=0;i<c->varying_count;++i){auto &v=c->varyings[i];v.name=r.str();v.components=r.value<int>();v.kind=r.value<int>();v.interpolation=r.value<int>();v.offset=r.value<int>();
            if(!v.name||v.components<1||v.components>4||v.kind<0||v.kind>2||v.interpolation<0||v.interpolation>2||v.offset<4+c->clip_count||v.offset+v.components>c->vs_stride)r.ok=false;}
        for(auto **child:{&c->kernel,&c->pull}){
            uint32_t bytes=r.value<uint32_t>();
            if(!r.ok||bytes>uint32_t(r.end-r.p)){r.ok=false;break;}
            std::vector<unsigned char> data(r.p,r.p+bytes);r.p+=bytes;
            *child=static_cast<glm_compile_result *>(calloc(1,sizeof(glm_compile_result)));
            if(!decode(data,*child,depth+1)||(*child)->clip||(*child)->gs||(*child)->tess)r.ok=false;
        }
    }
    res->xfb_count = r.value<int>();
    if (res->xfb_count < 0 || res->xfb_count > 64) r.ok = false, res->xfb_count = 0;
    res->xfb = static_cast<glm_xfb_varying *>(calloc(static_cast<size_t>(res->xfb_count) + 1, sizeof(glm_xfb_varying)));
    for (int i = 0; i < res->xfb_count; ++i) {
        res->xfb[i].name = r.str();
        res->xfb[i].type = r.value<uint32_t>();
        res->xfb[i].size = r.value<int>();
        res->xfb[i].components = r.value<int>();
        res->xfb[i].buffer = r.value<int>();
        res->xfb[i].offset = r.value<int>();
    }
    res->xfb_buffers = r.value<int>();
    for (int &s : res->xfb_stride) s = r.value<int>();
    res->global_size = r.value<int>();
    if (r.value<uint8_t>()) {
        if (res->global_size <= 0 || res->global_size > (1 << 20)) r.ok = false;
        else {
            res->initial_globals = static_cast<unsigned char *>(malloc(static_cast<size_t>(res->global_size)));
            for (int i = 0; i < res->global_size; ++i) res->initial_globals[i] = r.value<uint8_t>();
        }
    }
    res->uniforms = read_uniforms(r, &res->uniform_count);
    res->block_count = r.value<int>();
    if (res->block_count < 0 || res->block_count > 1024) r.ok = false, res->block_count = 0;
    res->blocks = static_cast<glm_block_info *>(calloc(static_cast<size_t>(res->block_count) + 1, sizeof(glm_block_info)));
    for (int i = 0; i < res->block_count; ++i) {
        res->blocks[i].name = r.str();
        res->blocks[i].size = r.value<int>();
        res->blocks[i].slot = r.value<int>();
        res->blocks[i].stages = r.value<uint32_t>();
    }
    res->block_uniforms = read_uniforms(r, &res->block_uniform_count);
    res->subroutine_count = r.value<int>();
    if (res->subroutine_count < 0 || res->subroutine_count > 4096) r.ok = false, res->subroutine_count = 0;
    res->subroutines = static_cast<glm_subroutine *>(calloc(static_cast<size_t>(res->subroutine_count) + 1, sizeof(glm_subroutine)));
    for (int i = 0; i < res->subroutine_count; ++i) {
        res->subroutines[i].stage = r.value<int>();
        res->subroutines[i].index = r.value<int>();
        res->subroutines[i].name = r.str();
    }
    res->subroutine_uniform_count = r.value<int>();
    if (res->subroutine_uniform_count < 0 || res->subroutine_uniform_count > 4096) r.ok = false, res->subroutine_uniform_count = 0;
    res->subroutine_uniforms = static_cast<glm_subroutine_uniform *>(
        calloc(static_cast<size_t>(res->subroutine_uniform_count) + 1, sizeof(glm_subroutine_uniform)));
    for (int i = 0; i < res->subroutine_uniform_count; ++i) {
        auto &u = res->subroutine_uniforms[i];
        u.stage = r.value<int>();
        u.location = r.value<int>();
        u.array_size = r.value<int>();
        u.name = r.str();
        u.compatible_count = r.value<int>();
        for (int &c : u.compatible) c = r.value<int>();
    }
    res->attributes = read_io(r, &res->attribute_count);
    res->outputs = read_io(r, &res->output_count);
    res->sampler_count = r.value<int>();
    res->writes_frag_color = r.value<bool>();
    res->flat_inputs = r.value<bool>();
    res->uses_legacy = r.value<bool>();
    res->globals_stages = r.value<uint8_t>();
    res->legacy_stages = r.value<uint8_t>();
    res->legacy_parts = r.value<uint32_t>();
    res->legacy_uniforms = r.value<uint32_t>();
    if (r.value<uint32_t>() != magic || !r.ok) {
        glm_compile_result_free(res);
        return false;
    }
    return true;
}

static std::vector<unsigned char> encode(const glm_compile_result *res)
{
    Writer w;
    w.value(magic);
    w.str(res->log);
    for (auto *m : res->msl) w.str(m);
    w.str(res->msl_capture);
    w.str(res->tess_inputs);
    w.value(bool(res->clip));
    if(res->clip){const auto &c=*res->clip;
        w.str(c.vs_capture);w.value(c.vs_stride);w.value(c.out_stride);w.value(c.clip_count);
        w.value(c.flat_outputs);w.value(c.primitive_id);w.value(c.varying_count);
        for(int i=0;i<c.varying_count;++i){const auto &v=c.varyings[i];w.str(v.name);w.value(v.components);w.value(v.kind);w.value(v.interpolation);w.value(v.offset);}
        for(auto *child:{c.kernel,c.pull}){auto data=encode(child);w.value(uint32_t(data.size()));w.bytes(data.data(),data.size());}
    }
    w.value(res->xfb_count);
    for (int i = 0; i < res->xfb_count; ++i) {
        w.str(res->xfb[i].name);
        w.value(res->xfb[i].type);
        w.value(res->xfb[i].size);
        w.value(res->xfb[i].components);
        w.value(res->xfb[i].buffer);
        w.value(res->xfb[i].offset);
    }
    w.value(res->xfb_buffers);
    for (int s : res->xfb_stride) w.value(s);
    w.value(res->global_size);
    w.value(static_cast<uint8_t>(res->initial_globals != nullptr));
    if (res->initial_globals)
        for (int i = 0; i < res->global_size; ++i) w.value(res->initial_globals[i]);
    write_uniforms(w, res->uniforms, res->uniform_count);
    w.value(res->block_count);
    for (int i = 0; i < res->block_count; ++i) {
        w.str(res->blocks[i].name);
        w.value(res->blocks[i].size);
        w.value(res->blocks[i].slot);
        w.value(res->blocks[i].stages);
    }
    write_uniforms(w, res->block_uniforms, res->block_uniform_count);
    w.value(res->subroutine_count);
    for (int i = 0; i < res->subroutine_count; ++i) {
        w.value(res->subroutines[i].stage);
        w.value(res->subroutines[i].index);
        w.str(res->subroutines[i].name);
    }
    w.value(res->subroutine_uniform_count);
    for (int i = 0; i < res->subroutine_uniform_count; ++i) {
        const auto &u = res->subroutine_uniforms[i];
        w.value(u.stage);
        w.value(u.location);
        w.value(u.array_size);
        w.str(u.name);
        w.value(u.compatible_count);
        for (int c : u.compatible) w.value(c);
    }
    write_io(w, res->attributes, res->attribute_count);
    write_io(w, res->outputs, res->output_count);
    w.value(res->sampler_count);
    w.value(res->writes_frag_color);
    w.value(res->flat_inputs);
    w.value(res->uses_legacy);
    w.value(res->globals_stages);
    w.value(res->legacy_stages);
    w.value(res->legacy_parts);
    w.value(res->legacy_uniforms);
    w.value(magic);
    return std::move(w.out);
}

/* Recent results in memory (a program linked again, or compiled ahead by
   a background job, is decoded without touching the disk), and requests a
   job is compiling now: a compile of the same request waits for it. */
static std::mutex memory_lock;
static std::condition_variable memory_changed;
static std::unordered_map<uint64_t, std::vector<unsigned char>> memory;
static std::unordered_set<uint64_t> pending;

extern "C" void glm_compile_pending_begin(const glm_compile_request *req)
{
    std::lock_guard<std::mutex> guard(memory_lock);
    pending.insert(request_hash(req));
}

extern "C" void glm_compile_pending_end(const glm_compile_request *req)
{
    std::lock_guard<std::mutex> guard(memory_lock);
    pending.erase(request_hash(req));
    memory_changed.notify_all();
}

extern "C" bool glm_compile_cache_load(const glm_compile_request *req, glm_compile_result *res)
{
    uint64_t hash = request_hash(req);
    {
        std::unique_lock<std::mutex> guard(memory_lock);
        memory_changed.wait(guard, [&] { return !pending.count(hash); });
        auto it = memory.find(hash);
        if (it != memory.end()) return decode(it->second, res);
    }
    std::string path = cache_path(req);
    if (path.empty()) return false;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<unsigned char> data;
    unsigned char buffer[65536];
    size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, f)) > 0) data.insert(data.end(), buffer, buffer + n);
    fclose(f);
    if (!decode(data, res)) return false;
    std::lock_guard<std::mutex> guard(memory_lock);
    if (memory.size() > 4096) memory.clear();
    memory[hash] = std::move(data);
    return true;
}

extern "C" void glm_compile_cache_store(const glm_compile_request *req, const glm_compile_result *res)
{
    if (!res->ok || res->gs || res->tess) return;
    std::vector<unsigned char> bytes = encode(res);
    {
        std::lock_guard<std::mutex> guard(memory_lock);
        if (memory.size() > 4096) memory.clear();
        memory[request_hash(req)] = bytes;
    }
    std::string path = cache_path(req);
    if (path.empty()) return;
    // Written whole then renamed: concurrent readers never see a partial file.
    static std::atomic<unsigned> serial;
    std::string temp = path + "." + std::to_string(getpid()) + "." + std::to_string(serial++) + ".tmp";
    FILE *f = fopen(temp.c_str(), "wb");
    if (!f) return;
    bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    ok = fclose(f) == 0 && ok;
    if (ok) rename(temp.c_str(), path.c_str());
    else unlink(temp.c_str());
}

/* GLMETAL_DUMP_SHADERS=dir: every linked program's GLSL and MSL, by request hash. */
extern "C" void glm_compile_dump(const glm_compile_request *req, const glm_compile_result *res)
{
    const char *dir = getenv("GLMETAL_DUMP_SHADERS");
    if (!dir) return;
    mkdir(dir, 0755);
    static const char *const stages[] = {"vert", "frag", "geom", "tesc", "tese", "comp"};
    char base[1024];
    snprintf(base, sizeof base, "%s/%016llx", dir, static_cast<unsigned long long>(request_hash(req)));
    for (int s = 0; s < GLM_STAGE_COUNT; ++s) {
        const char *texts[] = {req->sources[s], res->msl[s]};
        for (int k = 0; k < 2; ++k) {
            if (!texts[k]) continue;
            std::string path = std::string(base) + "." + stages[s] + (k ? ".msl" : ".glsl");
            if (FILE *f = fopen(path.c_str(), "w")) {
                fputs(texts[k], f);
                fclose(f);
            }
        }
    }
}

/* Successful glm_shader_check results by (build, stage, source): in memory,
   and as empty marker files next to the program cache. */
static uint64_t check_hash(glm_stage stage, const char *source)
{
    Hash h;
    h.str(glm_compiler_build);
    h.value(static_cast<int>(stage));
    h.str(source);
    return h.h;
}

static std::string check_path(uint64_t hash)
{
    const char *home = getenv("HOME");
    if (!home || getenv("GLMETAL_NO_SHADER_CACHE")) return {};
    char name[40];
    snprintf(name, sizeof name, "/%016llx.ok", static_cast<unsigned long long>(hash));
    return std::string(home) + "/Library/Caches/GLMetal" + name;
}

static std::mutex checks_lock;
static std::unordered_set<uint64_t> checks;

extern "C" bool glm_shader_check_known(glm_stage stage, const char *source)
{
    if (!source) return false;
    uint64_t hash = check_hash(stage, source);
    {
        std::lock_guard<std::mutex> guard(checks_lock);
        if (checks.count(hash)) return true;
    }
    std::string path = check_path(hash);
    if (path.empty() || access(path.c_str(), F_OK) != 0) return false;
    std::lock_guard<std::mutex> guard(checks_lock);
    checks.insert(hash);
    return true;
}

extern "C" void glm_shader_check_remember(glm_stage stage, const char *source)
{
    if (!source) return;
    uint64_t hash = check_hash(stage, source);
    {
        std::lock_guard<std::mutex> guard(checks_lock);
        checks.insert(hash);
    }
    std::string path = check_path(hash);
    if (path.empty()) return;
    std::string dir = path.substr(0, path.rfind('/'));
    mkdir(dir.c_str(), 0755);
    if (FILE *f = fopen(path.c_str(), "w")) fclose(f);
}

/* ---- remote compiles (compile_remote.cpp, tools/glmetal_compiler.cpp) -------
   A request and a result as bytes. Results with geometry or tessellation
   stages do not round-trip (the caller compiles those itself). */

extern "C" void glm_remote_encode_request(const glm_compile_request *req, std::vector<unsigned char> *out)
{
    Writer w;
    for (auto *s : req->sources) w.str(s);
    w.value(req->attribute_count);
    for (int i = 0; i < req->attribute_count; ++i) {
        w.str(req->attributes[i].name);
        w.value(req->attributes[i].location);
    }
    w.value(req->frag_output_count);
    for (int i = 0; i < req->frag_output_count; ++i) {
        w.str(req->frag_outputs[i].name);
        w.value(req->frag_outputs[i].location);
    }
    w.value(req->feedback_count);
    for (int i = 0; i < req->feedback_count; ++i) w.str(req->feedback_varyings[i]);
    w.value(req->feedback_interleaved);
    w.value(req->tess_output_vertices);
    w.value(req->uint_inputs);
    w.value(req->int_inputs);
    for (auto *name : req->border_samplers) w.str(name);
    *out = std::move(w.out);
}

/* The decoded request's strings live in `storage` (freed by the caller with
   glm_remote_free_request). */
struct RemoteRequestStorage {
    std::vector<char *> strings;
    std::vector<glm_name_location> attributes, outputs;
    std::vector<const char *> feedback;
};

extern "C" void *glm_remote_decode_request(const unsigned char *data, size_t size, glm_compile_request *req)
{
    auto *storage = new RemoteRequestStorage;
    Reader r{data, data + size};
    memset(req, 0, sizeof *req);
    auto keep = [&](char *s) {
        if (s) storage->strings.push_back(s);
        return s;
    };
    for (auto &s : req->sources) s = keep(r.str());
    int n = r.value<int>();
    for (int i = 0; i < n && r.ok && i < 4096; ++i) {
        char *name = keep(r.str());
        storage->attributes.push_back({name, r.value<int>()});
    }
    n = r.value<int>();
    for (int i = 0; i < n && r.ok && i < 4096; ++i) {
        char *name = keep(r.str());
        storage->outputs.push_back({name, r.value<int>()});
    }
    n = r.value<int>();
    for (int i = 0; i < n && r.ok && i < 4096; ++i) storage->feedback.push_back(keep(r.str()));
    req->attributes = storage->attributes.data();
    req->attribute_count = static_cast<int>(storage->attributes.size());
    req->frag_outputs = storage->outputs.data();
    req->frag_output_count = static_cast<int>(storage->outputs.size());
    req->feedback_varyings = storage->feedback.empty() ? nullptr : storage->feedback.data();
    req->feedback_count = static_cast<int>(storage->feedback.size());
    req->feedback_interleaved = r.value<bool>();
    req->tess_output_vertices = r.value<int>();
    req->uint_inputs = r.value<uint32_t>();
    req->int_inputs = r.value<uint32_t>();
    for (auto &name : req->border_samplers) name = keep(r.str());
    return storage;
}

extern "C" void glm_remote_free_request(void *storage)
{
    auto *s = static_cast<RemoteRequestStorage *>(storage);
    for (char *p : s->strings) free(p);
    delete s;
}

/* A result as bytes: status (0 failed with a log, 1 ok, 2 not transferable)
   then the log or the cache encoding. */
extern "C" void glm_remote_encode_result(const glm_compile_result *res, std::vector<unsigned char> *out)
{
    out->clear();
    if (res->gs || res->tess) {
        out->push_back(2);
        return;
    }
    if (!res->ok) {
        Writer w;
        w.value(static_cast<uint8_t>(0));
        w.str(res->log);
        *out = std::move(w.out);
        return;
    }
    out->push_back(1);
    std::vector<unsigned char> body = encode(res);
    out->insert(out->end(), body.begin(), body.end());
}

/* False when the result must be compiled locally. */
extern "C" bool glm_remote_decode_result(const unsigned char *data, size_t size, glm_compile_result *res)
{
    memset(res, 0, sizeof *res);
    if (!size || data[0] == 2) return false;
    if (data[0] == 0) {
        Reader r{data + 1, data + size};
        res->ok = false;
        res->log = r.str();
        return r.ok;
    }
    std::vector<unsigned char> body(data + 1, data + size);
    return decode(body, res);
}
