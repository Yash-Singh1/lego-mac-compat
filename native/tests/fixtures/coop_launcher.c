#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
extern int fixture_value(void), fixture_classes(void);
#define METHOD(object, slot, type) ((type)((*(void ***)(object))[slot]))

int LauncherMain(void)
{
    int expected_value = getenv("COOP_EXPECT_VALUE")[0] - '0';
    int expected_classes = getenv("COOP_EXPECT_CLASSES")[0] - '0';
    if (fixture_value() != expected_value || fixture_classes() != expected_classes) return -1;
    void *engine = dlopen("engine.dylib", RTLD_NOW), *server = dlopen("server.dylib", RTLD_NOW);
    if (!engine || !server || dlsym(engine, "sv_sendtables") ||
        dlsym(engine, "_ZN6ConVar8SetValueEPKc") || dlsym(server, "g_pServerClassHead") ||
        dlsym(server, "_ZL23g_CPointSurvey_ClassReg")) return -2;
    void *library = dlopen(getenv("LP32_STEAM_FIXTURE"), RTLD_NOW);
    void *(*create)(const char *, int *) = dlsym(library, "CreateInterface");
    if (!library || !create) return -3;
    void *gs = create("SteamGameServer014", NULL), *utils = create("SteamUtils010", NULL);
    typedef bool (*auth_fn)(void *, unsigned, const void *, unsigned, uint64_t *);
    typedef void (*disconnect_fn)(void *, uint64_t);
    typedef unsigned (*app_fn)(void *);
    if (!gs || !utils || METHOD(utils, 9, app_fn)(utils) != 620) return -4;
    const uint64_t player = UINT64_C(0x0110000101234567);
    struct { uint32_t before; uint64_t id; uint32_t after; } output = {0x11223344, 0, 0xaabbccdd};
    if (!METHOD(gs, 38, auth_fn)(gs, 0x01000000, "fixture-ticket-sensitive", 25, &output.id) ||
        output.id != player || output.before != 0x11223344 || output.after != 0xaabbccdd) return -5;
#pragma pack(push, 1)
    struct ip { unsigned char address[16]; int type; };
#pragma pack(pop)
    typedef struct ip (*ip_fn)(void *);
    struct ip address = METHOD(gs, 33, ip_fn)(gs);
    if (address.address[0] != 127 || address.address[3] != 1 || address.type) return -6;
    struct callback { int user, id; unsigned char *data; int size; } cb;
    bool (*next)(int, struct callback *) = dlsym(library, "Steam_BGetCallback");
    void (*release)(int) = dlsym(library, "Steam_FreeLastCallback");
    if (!next || !release) return -7;
    for (unsigned i = 0; i < 21; ++i) {
        int id = i == 0 || i == 20 ? 201 : i == 16 ? 143 : i == 17 ? 203 : i == 18 ? 1202 : 202;
        int size = i == 0 ? 16 : i == 16 ? 20 : i == 17 ? 12 : i == 18 ? 9 : i == 19 ? 13 : i == 20 ? 7 : 140;
        if (!next(23, &cb) || cb.user != 17 || cb.id != id || cb.size != size) return -8;
        if (memcmp(cb.data, &player, size < 8 ? (unsigned)size : 8)) return -9;
        if (i >= 1 && i <= 15) {
            unsigned reason; memcpy(&reason, cb.data + 8, 4);
            if (reason != i - 1) return -10;
        }
        if (i == 19 && cb.data[12] != 'X') return -11;
        release(23);
    }
    if (next(23, &cb)) return -12;
    METHOD(gs, 40, disconnect_fn)(gs, player);
    return 26;
}
