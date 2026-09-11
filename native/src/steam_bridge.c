#include "steam_bridge.h"
#include "compat_runtime.h"
#include "steam_storage_fix.h"

#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* CCallbackBase has three virtual methods, then flags and the callback ID.
   Keep host pointers entirely outside guest memory. The loaded library
   supplies initialization and callback delivery; a successful Init alone
   does not establish its authenticity or a connection to Valve's servers. */
struct steam_callback {
    const void **vtable;
    uint8_t flags;
    int32_t id;
    uint32_t guest;
    int32_t size;
    struct steam_callback *next;
};
static void *steam_library;
static struct steam_callback *callbacks;

int steam_bridge32_prepare_environment(uint32_t app_id)
{
    if (!app_id) return 0;
    char expected[16];
    snprintf(expected, sizeof(expected), "%u", app_id);
    const char *existing = getenv("SteamAppId");
    if (existing && existing[0]) {
        if (!strcmp(existing, expected)) return 0;
        fprintf(stderr, "compat32: conflicting SteamAppId; expected %s for this title\n", expected);
        return -1;
    }
    /* Feral chdirs into Resources before Steam startup; the SDK looks for
       steam_appid.txt in the CWD, not beside our relocated executable.
       The official bundled SDK also accepts SteamAppId. Supply identity
       before any guest code runs, without changing CWD or faking Init. */
    if (setenv("SteamAppId", expected, 1)) {
        perror("compat32: set SteamAppId");
        return -1;
    }
    fprintf(stderr, "compat32: SteamAppId=%s for relocated bundle\n", expected);
    return 0;
}

static void *steam_symbol(const char *name)
{
    if (!steam_library) {
        char executable[PATH_MAX], path[PATH_MAX];
        uint32_t size = sizeof(executable);
        if (_NSGetExecutablePath(executable, &size) != 0) return NULL;
        char *slash = strrchr(executable, '/');
        if (!slash) return NULL;
        *slash = 0;
        int length = snprintf(path, sizeof(path), "%s/../Resources/libsteam_api.dylib", executable);
        if (length < 0 || (size_t)length >= sizeof(path)) return NULL;
        steam_library = dlopen(path, RTLD_LOCAL | RTLD_NOW);
        if (!steam_library) {
            fprintf(stderr, "compat32: cannot load bundled Steam API: %s\n", dlerror());
            return NULL;
        }
        Dl_info info = {0};
        if (dladdr(dlsym(steam_library, "SteamRemoteStorage"), &info)) {
            int fixed = steam_storage_fix_apply(info.dli_fbase);
            if (fixed < 0) {
                fputs("compat32: cannot repair storage directory traversal\n", stderr);
                dlclose(steam_library);
                steam_library = NULL;
                return NULL;
            }
            if (fixed) fputs("compat32: repaired SDK storage directory traversal\n", stderr);
        }
    }
    return dlsym(steam_library, name);
}

static void deliver_callback(struct steam_callback *callback, void *data,
                             bool failure, uint64_t call, unsigned method)
{
    if (data && callback->size >= 12 &&
        (callback->id == 1101 || callback->id == 1102)) {
        uint64_t game_id;
        int32_t status;
        memcpy(&game_id, data, sizeof(game_id));
        memcpy(&status, (const char *)data + 8, sizeof(status));
        fprintf(stderr, "compat32: Steam %s game=%llu result=%d%s\n",
                callback->id == 1101 ? "UserStatsReceived" : "UserStatsStored",
                (unsigned long long)game_id, status, failure ? " I/O failure" : "");
    }
    uint32_t payload = compat_runtime32_allocate((size_t)callback->size, 1);
    if (!payload) return;
    if (data) {
        /* The Mac Steam SDK explicitly packs callback payloads to four
           bytes on both i386 and x86_64 (VALVE_CALLBACK_PACK_SMALL). */
        memcpy((void *)(uintptr_t)payload, data, (size_t)callback->size);
    }
    const uint32_t *vtable = (void *)(uintptr_t)*(uint32_t *)(uintptr_t)callback->guest;
    const uint32_t args[] = {callback->guest, payload, failure, (uint32_t)call, (uint32_t)(call >> 32)};
    compat_runtime32_call(vtable[method], args, method ? 5 : 2);
    compat_runtime32_deallocate(payload);
}
static void callback_run(struct steam_callback *callback, void *data)
{
    deliver_callback(callback, data, false, 0, 0);
}
static void callback_result(struct steam_callback *callback, void *data, bool failure, uint64_t call)
{
    deliver_callback(callback, data, failure, call, 1);
}
static int callback_size(struct steam_callback *callback)
{
    return callback->size;
}
static const void *callback_vtable[] = {callback_run, callback_result, callback_size};

enum { STEAM_USER, STEAM_STATS, STEAM_APPS, STEAM_UTILS, STEAM_STORAGE, STEAM_SCREENSHOTS };
static struct {
    const char *name;
    void *host;
    uint32_t guest;
} interfaces[] = {
    {"_SteamUser", NULL, 0}, {"_SteamUserStats", NULL, 0},
    {"_SteamApps", NULL, 0}, {"_SteamUtils", NULL, 0},
    {"_SteamRemoteStorage", NULL, 0},
    {"_SteamScreenshots", NULL, 0},
};

int steam_bridge32_call_uses_sret(const char *name,const uint32_t *args) {
    return !strcmp(name,"_lp32_steam_0_2") && interfaces[STEAM_USER].guest &&
        args[0] != interfaces[STEAM_USER].guest && args[1] == interfaces[STEAM_USER].guest;
}

/* Only known signatures are callable. Unimplemented slots trap through the
   normal diagnostic path rather than guessing a C++ ABI or returning success. */
static int interface_call(unsigned which, unsigned slot, const uint32_t *a, uint64_t *result)
{
    if (which >= sizeof(interfaces) / sizeof(interfaces[0]) || slot >= 64 ||
        !interfaces[which].host) return 0;
    void *object = interfaces[which].host;
    void *function = (*(void ***)object)[slot];
#define PTR(n) ((void *)(uintptr_t)a[n])
#define CALL0(type) ((type (*)(void *))function)(object)
#define CALL1(type, t1, v1) ((type (*)(void *, t1))function)(object, v1)
#define CALL2(type, t1, v1, t2, v2) ((type (*)(void *, t1, t2))function)(object, v1, v2)
#define CALL3(type, t1, v1, t2, v2, t3, v3) ((type (*)(void *, t1, t2, t3))function)(object, v1, v2, v3)
#define CALL4(type, t1, v1, t2, v2, t3, v3, t4, v4) ((type (*)(void *, t1, t2, t3, t4))function)(object, v1, v2, v3, v4)
    if (which == STEAM_USER) {
        if (slot == 0) { *result = CALL0(int32_t); return 1; }
        if (slot == 1) { *result = CALL0(bool); return 1; }
        if (slot == 6) {
            bool success=CALL2(bool,char *,PTR(1),int32_t,(int32_t)a[2]);
            const char *test=getenv("LP32_STEAM_TEST_DATA_DIR");
            if(success && test && test[0]) {
                if(strlen(test)+1>a[2]) success=false;
                else memcpy(PTR(1),test,strlen(test)+1);
            }
            *result=success;return 1;
        }
        /* Clang returns the trivial eight-byte CSteamID in EDX:EAX; older
           GCC clients pass a hidden result pointer before this. Identify
           the layout from the interface token, not from the game title. */
        if (slot == 2) {
            if(a[0] != interfaces[which].guest && a[1] != interfaces[which].guest) return 0;
            uint64_t id = CALL0(uint64_t);
            if(a[0] == interfaces[which].guest) *result = id;
            else {memcpy(PTR(0), &id, sizeof(id));*result = a[0];}
            return 1;
        }
    } else if (which == STEAM_APPS) {
        if (slot <= 3) { *result = CALL0(bool); return 1; }
        if (slot == 4 || slot == 5) {
            *result = compat_runtime32_copy_cstring(CALL0(const char *)); return 1;
        }
        if (slot == 6 || slot == 7) { *result = CALL1(bool, uint32_t, a[1]); return 1; }
        /* ISteamApps::RequestAppProofOfPurchaseKey(AppId_t). COD4 asks the
           original SDK to deliver its license through callback 1013. */
        if (slot == 14) { CALL1(void, uint32_t, a[1]); *result = 0; return 1; }
    } else if (which == STEAM_UTILS) {
        if (slot == 9) { *result = CALL0(uint32_t); return 1; } /* GetAppID */
    } else if (which == STEAM_STATS) {
        switch (slot) {
        case 0: case 10:
            *result = CALL0(bool);
            fprintf(stderr, "compat32: Steam %s %s\n",
                    slot == 0 ? "RequestCurrentStats" : "StoreStats",
                    *result ? "accepted" : "failed");
            return 1;
        case 1: case 2: case 6:
            *result = CALL2(bool, const char *, PTR(1), void *, PTR(2)); return 1;
        case 3: *result = CALL2(bool, const char *, PTR(1), int32_t, (int32_t)a[2]); return 1;
        case 4: {
            float value; memcpy(&value, &a[2], sizeof(value));
            *result = CALL2(bool, const char *, PTR(1), float, value); return 1;
        }
        case 7: case 8:
            *result = CALL1(bool, const char *, PTR(1));
            fprintf(stderr, "compat32: Steam %s(%s) %s\n",
                    slot == 7 ? "SetAchievement" : "ClearAchievement",
                    a[1] ? (const char *)PTR(1) : "(null)",
                    *result ? "accepted" : "failed");
            return 1;
        case 9: *result = CALL3(bool, const char *, PTR(1), bool *, PTR(2), uint32_t *, PTR(3)); return 1;
        case 11: *result = (uint32_t)CALL1(int, const char *, PTR(1)); return 1;
        case 12: *result = compat_runtime32_copy_cstring(CALL2(const char *, const char *, PTR(1), const char *, PTR(2))); return 1;
        case 14: *result = CALL0(uint32_t); return 1;
        case 15: *result = compat_runtime32_copy_cstring(CALL1(const char *, uint32_t, a[1])); return 1;
        }
    } else if (which == STEAM_SCREENSHOTS) {
        if (slot == 0) { *result = CALL4(uint32_t,const void *,PTR(1),uint32_t,a[2],int32_t,(int32_t)a[3],int32_t,(int32_t)a[4]); return 1; }
        if (slot == 1) { *result = CALL4(uint32_t,const char *,PTR(1),const char *,PTR(2),int32_t,(int32_t)a[3],int32_t,(int32_t)a[4]); return 1; }
        if (slot == 2) { CALL0(void); *result=0; return 1; }
        if (slot == 3) { CALL1(void,bool,a[1]!=0); *result=0; return 1; }
        if (slot == 4) { *result=CALL2(bool,uint32_t,a[1],const char *,PTR(2)); return 1; }
        if (slot == 5 || slot == 6) { *result=CALL2(bool,uint32_t,a[1],uint64_t,(uint64_t)a[2]|((uint64_t)a[3]<<32)); return 1; }
    } else if (which == STEAM_STORAGE) {
        /* RemoteStorage013 inserts three async methods after FileRead.
           Keep the earlier interface's slot mapping for older bundled SDKs. */
        if (steam_symbol("SteamAPI_ISteamRemoteStorage_FileReadAsync")) {
            if (slot == 2) { *result=CALL3(uint64_t,const char *,PTR(1),const void *,PTR(2),uint32_t,a[3]);return 1; }
            if (slot == 3) { *result=CALL3(uint64_t,const char *,PTR(1),uint32_t,a[2],uint32_t,a[3]);return 1; }
            if (slot == 4) { *result=CALL3(bool,uint64_t,(uint64_t)a[1]|((uint64_t)a[2]<<32),void *,PTR(3),uint32_t,a[4]);return 1; }
            if (slot >= 5) slot -= 3;
        }
        switch (slot) {
        case 0: *result = CALL3(bool, const char *, PTR(1), const void *, PTR(2), int32_t, (int32_t)a[3]); return 1;
        case 1: *result = (uint32_t)CALL3(int32_t, const char *, PTR(1), void *, PTR(2), int32_t, (int32_t)a[3]); return 1;
        case 2: case 3: case 10: case 11: *result = CALL1(bool, const char *, PTR(1)); return 1;
        case 4: case 6: *result = CALL1(uint64_t, const char *, PTR(1)); return 1;
        case 5: *result = CALL2(bool, const char *, PTR(1), int, (int)a[2]); return 1;
        case 7: *result = CALL3(bool, uint64_t, (uint64_t)a[1] | ((uint64_t)a[2] << 32), const void *, PTR(3), int32_t, (int32_t)a[4]); return 1;
        case 8: case 9: *result = CALL1(bool, uint64_t, (uint64_t)a[1] | ((uint64_t)a[2] << 32)); return 1;
        case 12: case 14: *result = (uint32_t)CALL1(int32_t, const char *, PTR(1)); return 1;
        case 13: *result = CALL1(int64_t, const char *, PTR(1)); return 1;
        case 15: *result = (uint32_t)CALL0(int32_t); return 1;
        case 16: *result = compat_runtime32_copy_cstring(CALL2(const char *, int, (int)a[1], int32_t *, PTR(2))); return 1;
        case 17: *result = CALL2(bool, int32_t *, PTR(1), int32_t *, PTR(2)); return 1;
        case 18: case 19: *result = CALL0(bool); return 1;
        case 20: CALL1(void, bool, a[1] != 0); *result = 0; return 1;
        }
    }
#undef PTR
#undef CALL0
#undef CALL1
#undef CALL2
#undef CALL3
#undef CALL4
    return 0;
}

/* Save diagnostics log metadata and SDK results, never save contents. Keep
 * forwarding behavior unchanged, including failure returns. Begin/end pairs
 * distinguish a blocked SDK call from a rejected or missing save request. */
static void trace_storage_call(unsigned slot, const uint32_t *args,
                               uint64_t result, const char *phase)
{
    static unsigned lines;
    unsigned line = __atomic_fetch_add(&lines, 1, __ATOMIC_RELAXED);
    if (line > 1024) return;
    if (line == 1024) {
        fputs("compat32: save trace reached 1024-line session limit\n", stderr);
        return;
    }
    static const char *names[] = {
        "FileWrite", "FileRead", "FileForget", "FileDelete", "FileShare",
        "SetSyncPlatforms", "FileWriteStreamOpen", "FileWriteStreamWriteChunk",
        "FileWriteStreamClose", "FileWriteStreamCancel", "FileExists",
        "FilePersisted", "GetFileSize", "GetFileTimestamp", "GetSyncPlatforms",
        "GetFileCount", "GetFileNameAndSize", "GetQuota", "CloudEnabledForAccount",
        "CloudEnabledForApp", "SetCloudEnabledForApp"
    };
    const char *filename = NULL;
    if (slot <= 6 || (slot >= 10 && slot <= 14))
        filename = (const char *)(uintptr_t)args[1];
    else if (slot == 16 && !strcmp(phase, "end"))
        filename = (const char *)(uintptr_t)(uint32_t)result;
    int32_t bytes = slot <= 1 ? (int32_t)args[3] : 0;
    if (slot == 16 && args[2] && !strcmp(phase, "end"))
        memcpy(&bytes, (const void *)(uintptr_t)args[2], sizeof(bytes));
    fprintf(stderr, "compat32: save %s t=%.3f operation=%s file=\"%.192s\" "
            "bytes=%d result=%lld\n", phase,
            clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9,
            slot <= 20 ? names[slot] : "unknown", filename ? filename : "",
            bytes, (long long)(int64_t)result);
}

int steam_bridge32_dispatch(const char *name, const uint32_t *args, uint64_t *result)
{
    if (strncmp(name, "_lp32_steam_", 11) == 0) {
        unsigned which, slot;
        if (sscanf(name, "_lp32_steam_%u_%u", &which, &slot) != 2) return 0;
        if(getenv("LP32_TRACE_STEAM"))fprintf(stderr,"compat32: Steam interface=%u slot=%u args=%08x,%08x,%08x,%08x\n",which,slot,args[0],args[1],args[2],args[3]);
        unsigned trace_slot=slot;
        bool async_slots=which==STEAM_STORAGE && steam_symbol("SteamAPI_ISteamRemoteStorage_FileReadAsync");
        if(async_slots && slot>=5)trace_slot-=3;
        bool storage=which==STEAM_STORAGE && trace_slot<=20 && !(async_slots && slot>=2 && slot<=4);
        if (storage && trace_slot <= 1) trace_storage_call(trace_slot, args, 0, "begin");
        int handled = interface_call(which, slot, args, result);
        if (storage && handled) trace_storage_call(trace_slot, args, *result, "end");
        return handled;
    }
    if (strncmp(name, "_Steam", 6) != 0) return 0;
    for (unsigned i = 0; i < sizeof(interfaces) / sizeof(interfaces[0]); ++i) {
        if (strcmp(name, interfaces[i].name) != 0) continue;
        if (!interfaces[i].guest) {
            void *(*function)(void) = steam_symbol(name + 1);
            interfaces[i].host = function ? function() : NULL;
            if (!interfaces[i].host) {
                static bool warned[sizeof(interfaces) / sizeof(interfaces[0])];
                if (!warned[i]) {
                    fprintf(stderr, "compat32: %s unavailable; Steam initialization is required\n", name + 1);
                    warned[i] = true;
                }
                *result = 0;
                return 1;
            }
            uint32_t allocation = compat_runtime32_allocate(65 * sizeof(uint32_t), 1);
            if (!allocation) return 0;
            uint32_t *words = (void *)(uintptr_t)allocation;
            words[0] = allocation + sizeof(uint32_t);
            for (unsigned slot = 0; slot < 64; ++slot) {
                char symbol[64];
                snprintf(symbol, sizeof(symbol), "_lp32_steam_%u_%u", i, slot);
                words[slot + 1] = compat_runtime32_guest_callback(symbol);
            }
            interfaces[i].guest = allocation;
        }
        *result = interfaces[i].guest;
        return 1;
    }
    if (strcmp(name, "_SteamAPI_RegisterCallback") == 0) {
        void (*function)(struct steam_callback *, int) = steam_symbol(name + 1);
        if (!function) return 0;
        for (struct steam_callback *p = callbacks; p; p = p->next) {
            if (p->guest == args[0]) { *result = 0; return 1; }
        }
        struct steam_callback *callback = calloc(1, sizeof(*callback));
        if (!callback) return 0;
        callback->vtable = callback_vtable;
        callback->guest = args[0];
        callback->id = (int32_t)args[1];
        callback->flags = *(uint8_t *)(uintptr_t)(args[0] + 4);
        const uint32_t *vtable = (void *)(uintptr_t)*(uint32_t *)(uintptr_t)args[0];
        callback->size = (int32_t)compat_runtime32_call(vtable[2], args, 1);
        if (callback->size <= 0 || callback->size > 65536) { free(callback); return 0; }
        callback->next = callbacks;
        callbacks = callback;
        function(callback, callback->id);
        *(uint8_t *)(uintptr_t)(args[0] + 4) = callback->flags;
        *(int32_t *)(uintptr_t)(args[0] + 8) = callback->id;
        *result = 0;
        return 1;
    }
    if (strcmp(name, "_SteamAPI_UnregisterCallback") == 0) {
        void (*function)(struct steam_callback *) = steam_symbol(name + 1);
        if (!function) return 0;
        for (struct steam_callback **p = &callbacks; *p; p = &(*p)->next) {
            if ((*p)->guest != args[0]) continue;
            struct steam_callback *callback = *p;
            function(callback);
            *(uint8_t *)(uintptr_t)(args[0] + 4) = callback->flags;
            *p = callback->next;
            free(callback);
            break;
        }
        *result = 0;
        return 1;
    }
    if (strcmp(name, "_SteamAPI_IsSteamRunning") == 0) {
        bool (*function)(void)=steam_symbol(name+1);if(!function)return 0;*result=function();return 1;
    }
    if (strcmp(name, "_SteamAPI_Init") == 0) {
        bool (*function)(void) = steam_symbol(name + 1);
        *result = function ? function() : false;
        fprintf(stderr, "compat32: SteamAPI_Init %s\n", *result ? "succeeded" : "failed (Steam must be running with a license for this game)");
        return 1;
    }
    if (strcmp(name, "_SteamAPI_RestartAppIfNecessary") == 0) {
        bool (*function)(uint32_t) = steam_symbol(name + 1);
        if (!function) return 0;
        *result = function(args[0]);
        if (*result)
            fprintf(stderr, "compat32: Steam requested relaunch for app %u; the guest may skip SteamAPI_Init\n", args[0]);
        return 1;
    }
    if (strcmp(name, "_SteamAPI_RunCallbacks") == 0 || strcmp(name, "_SteamAPI_Shutdown") == 0) {
        void (*function)(void) = steam_symbol(name + 1);
        if (!function) return 0;
        function();
        *result = 0;
        return 1;
    }
    return 0;
}

/* Read-only probe: initializes the shipped API, enumerates its
 * catalog, and checks full readback. It never invokes a write/delete method
 * or runs the game. Useful for diagnosing saved slots without a level replay. */
int steam_bridge32_probe_storage(void)
{
    uint32_t args[4] = {0};
    uint64_t result = 0;
    steam_bridge32_dispatch("_SteamAPI_Init", args, &result);
    if (!result) return -1;
    steam_bridge32_dispatch("_SteamRemoteStorage", args, &result);
    args[0] = (uint32_t)result;
    int status = -1;
    uint32_t size_cell = 0;
    if (!args[0]) goto done;
    size_cell = compat_runtime32_allocate(4, 1);
    if (!size_cell) goto done;
    void *host = interfaces[STEAM_STORAGE].host;
    void **methods = *(void ***)host;
    Dl_info info = {0};
    dladdr(methods[15], &info);
    printf("storage probe: native count=%d count_offset=%#llx name_offset=%#llx\n",
           ((int32_t (*)(void *))methods[15])(host),
           (unsigned long long)((uintptr_t)methods[15] - (uintptr_t)info.dli_fbase),
           (unsigned long long)((uintptr_t)methods[16] - (uintptr_t)info.dli_fbase));
    steam_bridge32_dispatch("_lp32_steam_4_15", args, &result);
    unsigned count = (unsigned)result;
    printf("storage probe: catalog contains %u file(s)\n", count);
    status = 0;
    for (unsigned index = 0; index < count; ++index) {
        args[1] = index; args[2] = size_cell;
        steam_bridge32_dispatch("_lp32_steam_4_16", args, &result);
        uint32_t name = (uint32_t)result;
        int32_t size = *(int32_t *)(uintptr_t)size_cell;
        if (!name || size < 0 || size > 16 * 1024 * 1024) { status = -1; break; }
        uint32_t data = compat_runtime32_allocate(size ? (size_t)size : 1, 0);
        if (!data) { compat_runtime32_deallocate(name); status = -1; break; }
        args[1] = name; args[2] = data; args[3] = (uint32_t)size;
        steam_bridge32_dispatch("_lp32_steam_4_1", args, &result);
        uint64_t hash = UINT64_C(14695981039346656037);
        if ((int32_t)result == size) {
            const unsigned char *bytes = (const void *)(uintptr_t)data;
            for (int32_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
        } else status = -1;
        printf("storage probe: %s size=%d read=%d fnv1a=%016llx\n",
               (const char *)(uintptr_t)name, size, (int32_t)result, (unsigned long long)hash);
        compat_runtime32_deallocate(data);
        compat_runtime32_deallocate(name);
    }
done:
    if (size_cell) compat_runtime32_deallocate(size_cell);
    steam_bridge32_dispatch("_SteamAPI_Shutdown", args, &result);
    return status;
}
