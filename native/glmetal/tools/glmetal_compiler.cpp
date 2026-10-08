// glmetal-compiler: GLMetal's shader compiler as a native helper process
// (src/compile_remote.cpp is the client). Requests arrive on stdin and are
// compiled concurrently; replies go to stdout in any order, tagged by id.
#include "../src/shader_compiler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <string>
#include <unistd.h>
#include <vector>

extern "C" void *glm_remote_decode_request(const unsigned char *data, size_t size, glm_compile_request *req);
extern "C" void glm_remote_free_request(void *storage);
extern "C" void glm_remote_encode_result(const glm_compile_result *res, std::vector<unsigned char> *out);

namespace {

std::mutex write_lock;

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

void write_all(const void *buffer, size_t size)
{
    auto *p = static_cast<const unsigned char *>(buffer);
    while (size) {
        ssize_t n = write(1, p, size);
        if (n <= 0) _exit(0); // the client is gone
        p += n;
        size -= static_cast<size_t>(n);
    }
}

void reply(uint64_t id, const std::vector<unsigned char> &data)
{
    uint32_t size = static_cast<uint32_t>(data.size());
    std::lock_guard<std::mutex> guard(write_lock);
    write_all(&size, 4);
    write_all(&id, 8);
    write_all(data.data(), data.size());
}

void serve(uint64_t id, uint8_t kind, std::vector<unsigned char> payload)
{
    std::vector<unsigned char> out;
    if (kind == 1) {
        glm_compile_request request;
        void *storage = glm_remote_decode_request(payload.data(), payload.size(), &request);
        glm_compile_result result;
        glm_program_compile(&request, &result);
        glm_remote_encode_result(&result, &out);
        glm_compile_result_free(&result);
        glm_remote_free_request(storage);
    } else if (kind == 2 && payload.size() >= 4) {
        int32_t stage;
        memcpy(&stage, payload.data(), 4);
        std::string source(payload.begin() + 4, payload.end());
        char *log = nullptr;
        bool ok = glm_shader_check(static_cast<glm_stage>(stage), source.c_str(), &log);
        out.push_back(ok ? 1 : 0);
        if (log) out.insert(out.end(), log, log + strlen(log));
        free(log);
    }
    reply(id, out);
}

struct Job {
    uint64_t id;
    uint8_t kind;
    std::vector<unsigned char> payload;
};

void *run(void *argument)
{
    Job *job = static_cast<Job *>(argument);
    serve(job->id, job->kind, std::move(job->payload));
    delete job;
    return nullptr;
}

} // namespace

int main()
{
    // Detach from the application: a client that waits for all its children
    // would otherwise wait for this process forever. The client reaps the parent; this process keeps the
    // pipes and exits when the client closes them.
    pid_t child = fork();
    if (child > 0) _exit(0);
    // The client keeps the caches; here only memory ones. Never a helper's helper.
    setenv("GLMETAL_NO_SHADER_CACHE", "1", 1);
    setenv("GLMETAL_NO_COMPILER_HELPER", "1", 1);
    unsetenv("GLMETAL_COMPILER_HELPER");
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 16 << 20); // glslang recurses deeply
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    for (;;) {
        uint32_t size;
        uint64_t id;
        uint8_t kind;
        if (!read_all(0, &size, 4) || !read_all(0, &id, 8) || !read_all(0, &kind, 1)) return 0;
        std::vector<unsigned char> payload(size);
        if (size && !read_all(0, payload.data(), size)) return 0;
        pthread_t thread;
        Job *job = new Job{id, kind, std::move(payload)};
        if (pthread_create(&thread, &attributes, run, job) != 0) run(job);
    }
}
