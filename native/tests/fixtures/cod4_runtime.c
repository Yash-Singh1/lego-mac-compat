#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/resource.h>
#include <dlfcn.h>
#include <ctype.h>
#include <string.h>
#include <pthread.h>

extern int dependency_value(void);
static int initialized;
__attribute__((constructor)) static void initialize(void) { ++initialized; }
static __attribute__((noinline)) void unwind(jmp_buf state, int value, int depth) {
    volatile uint32_t frame[32];
    frame[depth] = 0x12345678;
    if (depth) unwind(state, value, depth - 1);
    longjmp(state, value + (frame[depth] != 0x12345678));
}
int FixtureMain(void) {
    if (dependency_value() != 22 || initialized != 1) return -1;
    struct { jmp_buf state; uint32_t guard; } saved;
    saved.guard = 0xabc123;
    volatile int count = 0;
    int value = setjmp(saved.state);
    if (!value) { ++count; unwind(saved.state, 0, 10); }
    if (value != 1 || count != 1 || saved.guard != 0xabc123) return -2;
    value = setjmp(saved.state);
    if (!value) unwind(saved.state, 42, 6);
    if (value != 42) return -3;
    sigset_t original, blocked = 1u << (SIGUSR1 - 1), current;
    if (sigprocmask(SIG_BLOCK, &blocked, &original)) return -4;
    sigjmp_buf signal_state;
    value = sigsetjmp(signal_state, 1);
    if (!value) {
        sigprocmask(SIG_UNBLOCK, &blocked, 0);
        siglongjmp(signal_state, 7);
    }
    sigprocmask(SIG_SETMASK, 0, &current);
    sigprocmask(SIG_SETMASK, &original, 0);
    if (value != 7 || !(current & blocked)) return -5;
    volatile double rate = 59.94, negative = -1.5;
    volatile float fraction = 0.5f;
    if (lround(rate) != 60 || lround(negative) != -2 || lroundf(-2.5f) != -3) return -6;
    if (asinf(fraction) < 0.5235f || asinf(fraction) > 0.5237f ||
        acosf(fraction) < 1.0471f || acosf(fraction) > 1.0473f || atof("-1.25") != -1.25) return -7;
    struct { struct rlimit limit; uint32_t guard; } resources = {{0}, 0x12345678};
    if (getrlimit(RLIMIT_NOFILE, &resources.limit) || resources.guard != 0x12345678 ||
        resources.limit.rlim_cur < 16 || setrlimit(RLIMIT_NOFILE, &resources.limit)) return -8;
    struct { char path[4096]; uint32_t guard; } path = {{0}, 0x98765432};
    if (getcwd(path.path, sizeof(path.path)) != path.path || path.guard != 0x98765432 || path.path[0] != '/') return -9;
    char *allocated = getcwd(NULL, 0);
    if (!allocated || allocated[0] != '/') return -10;
    free(allocated);
    /* Exercise imports from guest code, not just direct host loader APIs.
       PunkBuster opens a module, resolves entry points and calls them in i386. */
    void *module = dlopen("bin/libfixture_dep.dylib", RTLD_NOW);
    int (*get_value)(void) = module ? dlsym(module, "dependency_value") : 0;
    if (!get_value || get_value() != 22) return -11;
    if (dlsym(module, "missing_entry") || !dlerror() || dlerror()) return -12;
    if (dlopen("bin/broken.dylib", RTLD_NOW) || !dlerror() || dlerror()) return -13;
    /* Exact non-ASCII byte path taken by LAN_CompareHostname, plus an ASCII
       mask with a nonzero result so an unsupported-import zero cannot pass. */
    if (__maskrune('A', _CTYPE_A) != _CTYPE_A ||
        __maskrune(0xc3, _CTYPE_A) || __maskrune(-1, _CTYPE_A)) return -14;
    if (dlclose(module)) return -15;
    module = dlopen("/usr/lib/libSystem.B.dylib", RTLD_NOW);
    int (*parse_number)(const char *) = module ? dlsym(module, "atoi") : 0;
    if (!parse_number || parse_number("12345") != 12345 || dlclose(module)) return -16;
    /* The first import resolves a handler; later calls use the cached path.
       Check guest pointer returns, signed comparisons, padding and guards. */
    pthread_t thread = pthread_self();
    for (unsigned pass = 0; pass < 3; ++pass) {
        struct { char bytes[40]; uint32_t guard; } destination;
        destination.guard = 0x1234abcd;
        const char *text = "Materials/SHIP/metal";
        if (strcpy(destination.bytes, text) != destination.bytes ||
            strlen(destination.bytes) != 20 || strcmp(destination.bytes, text)) return -17;
        if (strcmp("\xff", "\x7f") <= 0 || strcmp("a", "ab") >= 0 ||
            memcmp("a\0b", "a\0c", 3) >= 0 || memcmp("a", "b", 0)) return -18;
        for (unsigned n = 0; n <= sizeof(destination.bytes); ++n) {
            memset(destination.bytes, 0x55, sizeof(destination.bytes));
            if (strncpy(destination.bytes, "ship", n) != destination.bytes) return -19;
            for (unsigned i = 0; i < sizeof(destination.bytes); ++i) {
                char expected = i >= n ? 0x55 : i < 4 ? "ship"[i] : 0;
                if (destination.bytes[i] != expected) return -20;
            }
        }
        if (destination.guard != 0x1234abcd || !thread || pthread_self() != thread) return -21;
        for (int c = -1; c <= 256; ++c) {
            int expected = c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
            if (__tolower(c) != expected) return -22;
        }
    }
    return 26;
}
