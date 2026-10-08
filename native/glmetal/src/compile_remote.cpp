// Shader compiles in a native helper process (tools/glmetal_compiler.cpp).
// In a process running under Rosetta (the compatibility loaders are x86_64),
// glslang and SPIRV-Cross run translated and about six times slower than
// natively; the helper, an arm64 executable next to libGLMetal.dylib, does
// the same work natively and several requests at once. Frames on its stdin /
// stdout: uint32 payload size, uint64 id, uint8 kind, payload. Without the
// helper (or when it dies) callers compile locally.
#include "shader_compiler.h"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <mutex>
#include <spawn.h>
#include <string>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" char **environ;
extern "C" void glm_remote_encode_request(const glm_compile_request *req, std::vector<unsigned char> *out);
extern "C" bool glm_remote_decode_result(const unsigned char *data, size_t size, glm_compile_result *res);

namespace {

enum { KIND_PROGRAM = 1, KIND_CHECK = 2 };

struct Reply {
    bool done = false, failed = false;
    std::vector<unsigned char> data;
};

std::mutex lock;             // state below
std::condition_variable replied;
std::map<uint64_t, Reply *> waiting;
uint64_t next_id = 1;
int to_helper = -1, from_helper = -1;
bool started = false, broken = false;
std::mutex write_lock;

bool translated()
{
    int value = 0;
    size_t size = sizeof value;
    return sysctlbyname("sysctl.proc_translated", &value, &size, nullptr, 0) == 0 && value == 1;
}

bool read_all(int fd, void *buffer, size_t size)
{
    auto *p = static_cast<unsigned char *>(buffer);
    while (size) {
        ssize_t n = read(fd, p, size);
        if (n <= 0) return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

bool write_all(int fd, const void *buffer, size_t size)
{
    auto *p = static_cast<const unsigned char *>(buffer);
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n <= 0) return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

void fail_all()
{
    std::lock_guard<std::mutex> guard(lock);
    broken = true;
    for (auto &w : waiting) {
        w.second->done = true;
        w.second->failed = true;
    }
    waiting.clear();
    replied.notify_all();
}

void reader()
{
    for (;;) {
        uint32_t size;
        uint64_t id;
        if (!read_all(from_helper, &size, 4) || !read_all(from_helper, &id, 8)) break;
        std::vector<unsigned char> data(size);
        if (size && !read_all(from_helper, data.data(), size)) break;
        std::lock_guard<std::mutex> guard(lock);
        auto it = waiting.find(id);
        if (it == waiting.end()) continue;
        it->second->data = std::move(data);
        it->second->done = true;
        waiting.erase(it);
        replied.notify_all();
    }
    fail_all();
}

// Starts the helper once (under `lock`). False when there is none.
bool start()
{
    if (started) return !broken;
    started = true;
    broken = true;
    if (getenv("GLMETAL_NO_COMPILER_HELPER") || (!translated() && !getenv("GLMETAL_COMPILER_HELPER"))) return false;
    Dl_info info;
    if (!dladdr(reinterpret_cast<void *>(&glm_program_compile), &info) || !info.dli_fname) return false;
    std::string path = info.dli_fname;
    path = path.substr(0, path.rfind('/') + 1) + "glmetal-compiler";
    if (access(path.c_str(), X_OK) != 0) return false;
    int in[2], out[2];
    if (pipe(in) || pipe(out)) return false;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    posix_spawn_file_actions_addclose(&actions, in[1]);
    posix_spawn_file_actions_addclose(&actions, out[0]);
    // Native arm64 even though this process runs translated.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    cpu_type_t arm64 = CPU_TYPE_ARM64;
    size_t set = 0;
    posix_spawnattr_setbinpref_np(&attributes, 1, &arm64, &set);
    const char *argv[] = {path.c_str(), nullptr};
    pid_t pid;
    int status = posix_spawn(&pid, path.c_str(), &actions, &attributes, const_cast<char **>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(in[0]);
    close(out[1]);
    if (status != 0) {
        close(in[1]);
        close(out[0]);
        return false;
    }
    to_helper = in[1];
    from_helper = out[0];
    broken = false;
    std::thread(reader).detach();
    // Reap it when it exits.
    std::thread([pid] {
        int s;
        waitpid(pid, &s, 0);
    }).detach();
    return true;
}

// Sends one request and waits for its reply; false when the helper is unavailable.
bool call(uint8_t kind, const std::vector<unsigned char> &payload, std::vector<unsigned char> *out)
{
    Reply reply;
    uint64_t id;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (!start()) return false;
        id = next_id++;
        waiting[id] = &reply;
    }
    uint32_t size = static_cast<uint32_t>(payload.size());
    bool sent;
    {
        std::lock_guard<std::mutex> guard(write_lock);
        sent = write_all(to_helper, &size, 4) && write_all(to_helper, &id, 8) && write_all(to_helper, &kind, 1) &&
               write_all(to_helper, payload.data(), payload.size());
    }
    if (!sent) {
        fail_all();
        return false;
    }
    std::unique_lock<std::mutex> guard(lock);
    replied.wait(guard, [&] { return reply.done; });
    if (reply.failed) return false;
    *out = std::move(reply.data);
    return true;
}

} // namespace

extern "C" bool glm_remote_compile(const glm_compile_request *req, glm_compile_result *res)
{
    std::vector<unsigned char> payload, reply;
    glm_remote_encode_request(req, &payload);
    if (!call(KIND_PROGRAM, payload, &reply)) return false;
    return glm_remote_decode_result(reply.data(), reply.size(), res);
}

extern "C" bool glm_remote_check(glm_stage stage, const char *source, bool *ok, char **log)
{
    std::vector<unsigned char> payload(4);
    int32_t s = stage;
    memcpy(payload.data(), &s, 4);
    size_t n = source ? strlen(source) : 0;
    payload.insert(payload.end(), source, source + n);
    std::vector<unsigned char> reply;
    if (!call(KIND_CHECK, payload, &reply) || reply.empty()) return false;
    *ok = reply[0] != 0;
    if (log) {
        *log = static_cast<char *>(malloc(reply.size()));
        memcpy(*log, reply.data() + 1, reply.size() - 1);
        (*log)[reply.size() - 1] = 0;
    }
    return true;
}
