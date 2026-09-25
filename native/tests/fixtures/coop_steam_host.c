#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct object { void **vtable; };
static void *server_table[44], *utils_table[38];
static struct object server = {server_table}, utils = {utils_table};
static unsigned event_index;
static bool outstanding;
static unsigned char *short_payload;
static const uint64_t player = UINT64_C(0x0110000101234567);

static bool authenticate(void *self, unsigned ip, const void *blob, unsigned size, uint64_t *id)
{
    if (self != &server || ip != 0x01000000 || size != 25 ||
        memcmp(blob, "fixture-ticket-sensitive", size) || !id) abort();
    *id = player;
    return true;
}
static void disconnect(void *self, uint64_t id) { if (self != &server || id != player) abort(); }
static unsigned app_id(void *self) { if (self != &utils) abort(); return 620; }
#pragma pack(push, 1)
struct ip { unsigned char address[16]; int type; };
#pragma pack(pop)
static struct ip public_ip(void *self)
{
    if (self != &server) abort();
    return (struct ip){{127, 0, 0, 1}, 0};
}
void *CreateInterface(const char *version, int *result)
{
    if (result) *result = 0;
    server_table[38] = authenticate;
    server_table[40] = disconnect;
    server_table[33] = public_ip;
    utils_table[9] = app_id;
    if (!strcmp(version, "SteamGameServer014")) {
        event_index = 0; outstanding = false;
        return &server;
    }
    if (!strcmp(version, "SteamUtils010")) return &utils;
    if (result) *result = 1;
    return NULL;
}
struct callback { int user, id; void *data; int size; };
bool Steam_BGetCallback(int pipe, struct callback *cb)
{
    static unsigned char payload[140];
    if (pipe != 23 || outstanding) abort();
    if (event_index == 21) return false;
    memset(payload, 0, sizeof(payload));
    memcpy(payload, &player, 8);
    *cb = (struct callback){17, 202, payload, 140};
    unsigned reason = event_index ? event_index - 1 : 0;
    memcpy(payload + 8, &reason, 4);
    if (event_index == 0) { cb->id = 201; cb->size = 16; memcpy(payload + 8, &player, 8); }
    if (event_index == 16) { cb->id = 143; cb->size = 20; memcpy(payload + 12, &player, 8); }
    if (event_index == 17) { cb->id = 203; cb->size = 12; }
    if (event_index == 18) { cb->id = 1202; cb->size = 9; }
    if (event_index == 19) {
        if (!short_payload) {
            size_t page = (size_t)getpagesize();
            unsigned char *memory = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (memory == MAP_FAILED || mprotect(memory + page, page, PROT_NONE)) abort();
            short_payload = memory + page - 13;
        }
        memcpy(short_payload, payload, 12); short_payload[12] = 'X';
        cb->data = short_payload; cb->size = 13;
    }
    if (event_index == 20) { cb->id = 201; cb->size = 7; }
    outstanding = true;
    return true;
}
void Steam_FreeLastCallback(int pipe)
{
    if (pipe != 23 || !outstanding) abort();
    outstanding = false; ++event_index;
}
