#include "steam_bridge.h"
#include "compat_runtime.h"
#include "game_profile.h"
#include <dlfcn.h>
#include <ffi/ffi.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

struct steam_method { const char *name; char result; const char *args; };
struct steam_interface { const char *version; unsigned first, count; };
#include "steam_abi.inc"
static void *client_library;
struct steam_proxy { uint32_t guest; void *host; const struct steam_interface *interface; };
static struct steam_proxy proxies[128];
static unsigned proxy_count;
static uint32_t method_thunks[sizeof(steam_methods) / sizeof(steam_methods[0])];
static _Atomic uint32_t warning_hook, callback_check_hook;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static bool trace;

static uint32_t string_copy_locked(const char *text)
{
    // Native accessors return borrowed strings; retain a stable guest copy.
    static uint32_t strings[1024];
    static unsigned count;
    if (!text) return 0;
    for (unsigned i = 0; i < count; ++i) if (!strcmp((const char *)(uintptr_t)strings[i], text)) return strings[i];
    if (count == 1024) return 0;
    uint32_t result = compat_runtime32_copy_cstring(text);
    if (result) strings[count++] = result;
    return result;
}

static uint32_t proxy_for_locked(void *object, const char *version)
{
    if (trace) fprintf(stderr, "compat32: Steam interface request %s available=%d\n", version ? version : "(null)", object != NULL);
    if (!object || !version) return 0;
    const struct steam_interface *interface = NULL;
    for (unsigned i = 0; i < sizeof(steam_interfaces) / sizeof(steam_interfaces[0]); ++i) {
        if (!strcmp(steam_interfaces[i].version, version)) { interface = &steam_interfaces[i]; break; }
    }
    if (!interface) { fprintf(stderr, "compat32: unsupported native Steam interface %s\n", version); return 0; }
    for (unsigned i = 0; i < proxy_count; ++i) {
        if (proxies[i].host == object && proxies[i].interface == interface) return proxies[i].guest;
    }
    if (proxy_count == 128) return 0;
    uint32_t guest = compat_runtime32_allocate(4 + 4 * interface->count, 1);
    if (!guest) return 0;
    uint32_t *words = (void *)(uintptr_t)guest;
    words[0] = guest + 4;
    for (unsigned i = 0; i < interface->count; ++i) {
        unsigned method = interface->first + i;
        if (!method_thunks[method]) {
            char name[64]; snprintf(name, sizeof(name), "_lp32_steam_method_%u", method);
            method_thunks[method] = compat_runtime32_guest_callback(name);
        }
        if (!(words[i + 1] = method_thunks[method])) return 0;
    }
    proxies[proxy_count++] = (struct steam_proxy){guest, object, interface};
    fprintf(stderr, "compat32: native Steam interface %s\n", version);
    return guest;
}

static uint32_t proxy_for(void *object, const char *version)
{
    pthread_mutex_lock(&cache_lock);
    uint32_t result = proxy_for_locked(object, version);
    pthread_mutex_unlock(&cache_lock);
    return result;
}
static uint32_t string_copy(const char *text)
{
    pthread_mutex_lock(&cache_lock);
    uint32_t result = string_copy_locked(text);
    pthread_mutex_unlock(&cache_lock);
    return result;
}
static bool plain_callback(int id)
{
    for (unsigned i = 0; i < sizeof(steam_plain_callbacks) / sizeof(steam_plain_callbacks[0]); ++i)
        if (steam_plain_callbacks[i] == id) return true;
    return false;
}

uint32_t steam_bridge32_open(const char *path)
{
    if (lp32_profile()->title != LP32_TITLE_PORTAL2 || !path || path[0] != '/') return 0;
    const char *base = strrchr(path, '/');
    if (!base || strcmp(base + 1, "steamclient.dylib")) return 0;
    if (!client_library) {
        client_library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!client_library) return 0;
        trace = getenv("LP32_TRACE_STEAM") != NULL;
        fprintf(stderr, "compat32: using native Steam client through the i386 interface bridge\n");
    }
    return LP32_STEAM_CLIENT_HANDLE;
}

uint32_t steam_bridge32_symbol(const char *symbol)
{
    static const char *names[] = {"CreateInterface", "Steam_BGetCallback", "Steam_FreeLastCallback",
        "Steam_GetAPICallResult", "Steam_ReleaseThreadLocalMemory", "Breakpad_SteamMiniDumpInit",
        "Breakpad_SteamSetAppID", "Breakpad_SteamSetSteamID", "Breakpad_SteamWriteMiniDumpSetComment",
        "Breakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId"};
    if (!client_library || !symbol || !dlsym(client_library, symbol)) return 0;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (!strcmp(symbol, names[i])) {
            char thunk[128]; snprintf(thunk, sizeof(thunk), "_lp32_steam_%s", symbol);
            return compat_runtime32_guest_callback(thunk);
        }
    }
    fprintf(stderr, "compat32: unsupported native Steam export %s\n", symbol);
    return 0;
}

static void native_warning(int severity, const char *text)
{
    uint32_t hook = warning_hook;
    if (!hook) return;
    uint32_t copy = compat_runtime32_copy_cstring(text ? text : "");
    uint32_t args[] = {(uint32_t)severity, copy};
    compat_runtime32_call(hook, args, 2);
    compat_runtime32_deallocate(copy);
}
static bool native_callback_check(int callback)
{
    uint32_t arg = (uint32_t)callback;
    uint32_t hook = callback_check_hook;
    return hook && compat_runtime32_call(hook, &arg, 1) != 0;
}

static ffi_type *abi_type(char c)
{
    switch (c) {
    case 'v': return &ffi_type_void;
    case 'b': return &ffi_type_uint8;
    case 'u': return &ffi_type_uint16;
    case 'i': return &ffi_type_uint32;
    case 'q': return &ffi_type_uint64;
    case 'f': return &ffi_type_float;
    case 'd': return &ffi_type_double;
    case 'p': case 's': case 'o': case 'W': case 'K': return &ffi_type_pointer;
    default: return NULL;
    }
}

static int call_method(unsigned index, const uint32_t *args, uint64_t *result)
{
    if (index >= sizeof(steam_methods) / sizeof(steam_methods[0])) return 0;
    const struct steam_method *method = &steam_methods[index];
    uint32_t object = args[0];
    const struct steam_proxy *proxy = NULL;
    pthread_mutex_lock(&cache_lock);
    for (unsigned i = 0; i < proxy_count; ++i) if (proxies[i].guest == object) { proxy = &proxies[i]; break; }
    pthread_mutex_unlock(&cache_lock);
    if (!proxy || index < proxy->interface->first || index >= proxy->interface->first + proxy->interface->count) return 0;
    if (trace) fprintf(stderr, "compat32: Steam %s::%s\n", proxy->interface->version, method->name);
    unsigned count = (unsigned)strlen(method->args);
    if (count > 14 || !abi_type(method->result)) return 0;
    ffi_type *types[16]; void *values[16]; uint64_t storage[16] = {0};
    types[0] = &ffi_type_pointer; storage[0] = (uintptr_t)proxy->host; values[0] = &storage[0];
    unsigned word = 1;
    const char *interface_version = NULL;
    for (unsigned i = 0; i < count; ++i) {
        char code = method->args[i];
        if (!(types[i + 1] = abi_type(code))) {
            fprintf(stderr, "compat32: unsupported Steam argument ABI %s::%s\n", proxy->interface->version, method->name);
            return 0;
        }
        storage[i + 1] = args[word++];
        if (code == 'q' || code == 'd') storage[i + 1] |= (uint64_t)args[word++] << 32;
        if (code == 'W') {
            warning_hook = (uint32_t)storage[i + 1]; storage[i + 1] = warning_hook ? (uintptr_t)native_warning : 0;
        } else if (code == 'K') {
            callback_check_hook = (uint32_t)storage[i + 1]; storage[i + 1] = callback_check_hook ? (uintptr_t)native_callback_check : 0;
        }
        values[i + 1] = &storage[i + 1];
        if (method->result == 'o' && i + 1 == count) interface_version = (const char *)(uintptr_t)storage[i + 1];
    }
    if (!strcmp(method->name, "GetAPICallResult") && !plain_callback((int)args[5])) return 0;
    ffi_cif cif;
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, count + 1, abi_type(method->result), types) != FFI_OK) return 0;
    void **vtable = *(void ***)proxy->host;
    uint64_t returned = 0;
    ffi_call(&cif, FFI_FN(vtable[index - proxy->interface->first]), &returned, values);
    if (method->result == 's') returned = string_copy((const char *)(uintptr_t)returned);
    else if (method->result == 'o') returned = proxy_for((void *)(uintptr_t)returned, interface_version);
    else if (method->result == 'f') { float value; memcpy(&value, &returned, 4); returned = compat_runtime32_return_float(value); }
    else if (method->result == 'd') { double value; memcpy(&value, &returned, 8); returned = compat_runtime32_return_double(value); }
    *result = returned;
    return 1;
}

struct native_callback { int user, id; void *data; int size; };
// Steam dispatches callbacks on the polling thread; retain each pipe's low
// buffer until its next callback without sharing storage across threads.
static _Thread_local struct { int pipe; uint32_t data; size_t capacity; } callback_buffers[16];
int steam_bridge32_dispatch(const char *name, const uint32_t *args, uint64_t *result)
{
    if (strncmp(name, "_lp32_steam_", 12) || !client_library) return 0;
    name += 12;
    if (!strncmp(name, "method_", 7)) return call_method((unsigned)strtoul(name + 7, NULL, 10), args, result);
    if (trace) fprintf(stderr, "compat32: Steam export %s\n", name);
    // Steam's native crash handler cannot unwind the mixed i386/x86_64
    // stacks and would replace the loader's mode-recovery signal handlers.
    // Keep the compatibility crash log instead of installing Breakpad.
    if (!strncmp(name, "Breakpad_", 9)) { *result = 0; return 1; }
    void *function = dlsym(client_library, name);
    if (!function) return 0;
    if (!strcmp(name, "CreateInterface")) {
        void *host = ((void *(*)(const char *, int *))function)((const char *)(uintptr_t)args[0], (int *)(uintptr_t)args[1]);
        *result = proxy_for(host, (const char *)(uintptr_t)args[0]); return 1;
    }
    if (!strcmp(name, "Steam_ReleaseThreadLocalMemory")) { ((void (*)(int))function)(args[0]); *result = 0; return 1; }
    if (!strcmp(name, "Steam_FreeLastCallback")) { ((void (*)(int))function)(args[0]); *result = 0; return 1; }
    if (!strcmp(name, "Steam_BGetCallback")) {
        struct native_callback cb = {0};
        bool ok = false;
        for (unsigned pending = 0; pending < 1024; ++pending) {
            ok = ((bool (*)(int, struct native_callback *))function)(args[0], &cb);
            if (!ok || plain_callback(cb.id)) break;
            // The modern client queues private callbacks absent from the old
            // SDK. Only discard ones no guest has registered to receive.
            if (!callback_check_hook || native_callback_check(cb.id)) {
                fprintf(stderr, "compat32: unsupported registered Steam callback layout %d\n", cb.id);
                return 0;
            }
            if (trace) fprintf(stderr, "compat32: skipped unregistered native Steam callback %d\n", cb.id);
            void (*release)(int) = dlsym(client_library, "Steam_FreeLastCallback");
            if (!release) return 0;
            release(args[0]); ok = false;
        }
        if (ok) {
            if (cb.size < 0 || cb.size > 1024 * 1024 || (cb.size && !cb.data) || !plain_callback(cb.id)) return 0;
            unsigned slot;
            for (slot = 0; slot < 16; ++slot)
                if (!callback_buffers[slot].pipe || callback_buffers[slot].pipe == (int)args[0]) break;
            if (slot == 16) return 0;
            callback_buffers[slot].pipe = (int)args[0];
            if (callback_buffers[slot].capacity < (size_t)cb.size) {
                uint32_t resized = compat_runtime32_reallocate(callback_buffers[slot].data, cb.size);
                if (!resized) return 0;
                callback_buffers[slot].data = resized;
                callback_buffers[slot].capacity = cb.size;
            }
            uint32_t data = callback_buffers[slot].data;
            if (cb.size && !data) return 0;
            if (cb.size) memcpy((void *)(uintptr_t)data, cb.data, cb.size);
            uint32_t words[] = {(uint32_t)cb.user, (uint32_t)cb.id, data, (uint32_t)cb.size};
            memcpy((void *)(uintptr_t)args[1], words, sizeof(words));
        }
        *result = ok; return 1;
    }
    if (!strcmp(name, "Steam_GetAPICallResult")) {
        if (!plain_callback((int)args[5])) return 0;
        *result = ((bool (*)(int, uint64_t, void *, int, int, bool *))function)(args[0],
            (uint64_t)args[1] | (uint64_t)args[2] << 32, (void *)(uintptr_t)args[3], args[4], args[5], (bool *)(uintptr_t)args[6]);
        return 1;
    }
    return 0;
}
