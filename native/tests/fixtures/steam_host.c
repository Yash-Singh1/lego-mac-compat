/* Original mock client: exercises the real 32-to-64-bit bridge without Steam. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
struct object { void **vtable; };
static void *client17_table[37];
static void *client_table[42], *user_table[32], *utils_table[38], *stats_table[45];
static struct object client17 = {client17_table};
static struct object client = {client_table}, user = {user_table}, utils = {utils_table}, stats = {stats_table};
static uint64_t steam_id(void *self) { if (self != &user) abort(); return UINT64_C(0x123456789abcdef0); }
static unsigned app_id(void *self) { if (self != &utils) abort(); return 620; }
static const char *country(void *self) { if (self != &utils) abort(); return "fixture-country"; }
static bool rate(void *self, const char *name, float count, double seconds) {
    return self == &stats && !strcmp(name, "rate") && count == 1.25f && seconds == 2.5;
}
static unsigned callback_count, frames;
static void set_check(void *self, bool (*check)(int)) { if (self != &client || !check) abort(); }
static void run_frame(void *self) { if (self != &client17) abort(); ++frames; }
static unsigned ipc_count(void *self) { if (self != &client17) abort(); return frames; }
static void *get_interface(void *self, int u, int p, const char *version) {
    if (self != &client || u != 17 || p != 23) abort();
    if (!strcmp(version, "SteamUser021")) return &user;
    if (!strcmp(version, "SteamUtils010")) return &utils;
    if (!strcmp(version, "STEAMUSERSTATS_INTERFACE_VERSION012")) return &stats;
    return 0;
}
void *CreateInterface(const char *version, int *result) {
    callback_count = 0;
    if (!strcmp(version, "SteamClient017")) {
        frames = 0; client17_table[19] = run_frame; client17_table[20] = ipc_count;
        if (result) *result = 0;
        return &client17;
    }
    if (result) *result = strcmp(version, "SteamClient020") != 0;
    client_table[34] = set_check; client_table[12] = get_interface; user_table[2] = steam_id;
    utils_table[4] = country; utils_table[9] = app_id; stats_table[5] = rate;
    return !strcmp(version, "SteamClient020") ? &client : 0;
}
struct callback { int user, id; void *data; int size; };
static const uint32_t payload[] = {0x11223344, 0xaabbccdd};
bool Steam_BGetCallback(int pipe, struct callback *cb) {
    if (pipe != 23 || callback_count >= 2) return false;
    if (!callback_count++) { *cb = (struct callback){17, 1040044, (void *)payload, sizeof(payload)}; return true; }
    *cb = (struct callback){17, 102, (void *)payload, sizeof(payload)}; return true;
}
void Steam_FreeLastCallback(int pipe) { if (pipe != 23) abort(); }
bool Steam_GetAPICallResult(int pipe, uint64_t call, void *data, int size, int id, bool *failed) {
    if (pipe != 23 || call != UINT64_C(0xfedcba9876543210) || size != 8 || id != 102) abort();
    memcpy(data, payload, size); *failed = false; return true;
}
