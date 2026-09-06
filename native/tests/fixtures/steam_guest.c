#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#define METHOD(obj, slot, type) ((type)((*(void ***)(obj))[slot]))
static int checked_callback;
static bool registered_callback(int callback) { checked_callback = callback; return false; }
int check_steam(void) {
    const char *number = "0x123456789abcdef0!";
    struct { char *end; uint32_t guard; } parsed = {0, 0xaabbccdd};
    if (strtoull(number, &parsed.end, 0) != UINT64_C(0x123456789abcdef0) ||
        *parsed.end != '!' || parsed.guard != 0xaabbccdd ||
        strtoll("-4294967297", 0, 10) != -INT64_C(4294967297)) return -80;
    void *library = dlopen(getenv("LP32_STEAM_FIXTURE"), RTLD_NOW);
    void *(*create)(const char *, int *) = dlsym(library, "CreateInterface");
    if (!library || !create) return -70;
    int status = -1;
    void *client = create("SteamClient020", &status);
    if (!client || status) return -71;
    typedef void *(*get_fn)(void *, int, int, const char *);
    get_fn get = METHOD(client, 12, get_fn);
    void *user = get(client, 17, 23, "SteamUser021");
    void *utils = get(client, 17, 23, "SteamUtils010");
    void *stats = get(client, 17, 23, "STEAMUSERSTATS_INTERFACE_VERSION012");
    typedef uint64_t (*id_fn)(void *);
    typedef unsigned (*app_fn)(void *);
    typedef const char *(*string_fn)(void *);
    typedef bool (*rate_fn)(void *, const char *, float, double);
    if (!user || !utils || !stats || get(client, 17, 23, "SteamUser021") != user) return -72;
    if (METHOD(user, 2, id_fn)(user) != UINT64_C(0x123456789abcdef0)) return -73;
    /* AppID follows a private virtual slot omitted from the JSON metadata. */
    if (METHOD(utils, 9, app_fn)(utils) != 620 ||
        strcmp(METHOD(utils, 4, string_fn)(utils), "fixture-country")) return -74;
    if (!METHOD(stats, 5, rate_fn)(stats, "rate", 1.25f, 2.5)) return -75;
    void *legacy = create("SteamClient017", &status);
    typedef void (*frame_fn)(void *);
    if (!legacy || status) return -81;
    METHOD(legacy, 19, frame_fn)(legacy);
    if (METHOD(legacy, 20, app_fn)(legacy) != 1) return -82;
    typedef void (*check_fn)(void *, bool (*)(int));
    METHOD(client, 34, check_fn)(client, registered_callback);
    checked_callback = 0;
    struct callback { int user, id; uint32_t *data; int size; } cb;
    bool (*next)(int, struct callback *) = dlsym(library, "Steam_BGetCallback");
    void (*release)(int) = dlsym(library, "Steam_FreeLastCallback");
    bool (*get_result)(int, uint64_t, void *, int, int, bool *) = dlsym(library, "Steam_GetAPICallResult");
    if (!next || !release || !get_result || !next(23, &cb) || cb.user != 17 ||
        cb.id != 102 || cb.size != 8 || cb.data[0] != 0x11223344 || cb.data[1] != 0xaabbccdd) return -76;
    if (checked_callback != 1040044) return -83;
    release(23); if (next(23, &cb)) return -77;
    uint32_t data[2]; bool failed = true;
    if (!get_result(23, UINT64_C(0xfedcba9876543210), data, sizeof(data), 102, &failed) ||
        failed || data[0] != 0x11223344 || data[1] != 0xaabbccdd) return -78;
    return dlclose(library) ? -79 : 0;
}
