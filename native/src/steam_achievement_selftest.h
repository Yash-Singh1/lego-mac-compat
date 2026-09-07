/* Included by game_loader.c to exercise the actual mapped i386 routine,
   without initializing Steam, running the game, or unlocking anything. */
static int steam_achievement_selftest(void)
{
    if (lp32_profile()->title != LP32_TITLE_MARVEL) return -1;
#define ACH_CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "achievement selftest failed at line %d: %s\n", __LINE__, #condition); \
    return -1; } } while (0)
    const struct lp32_steam_achievement_guard *guard = lp32_profile()->steam_achievement_guard;
    uint32_t args[] = {0, 1, 0};
    volatile uint32_t *stats = (void *)(uintptr_t)0x0160b554;
    volatile uint8_t *enabled = (void *)(uintptr_t)0x014351c2;
    volatile uint8_t *ready = (void *)(uintptr_t)0x0160b564;
    *stats = 0;
    *enabled = 1;
    if (!strcmp(getenv("LP32_STEAM_ACHIEVEMENT_SELFTEST"), "unpatched")) {
        /* The negative control must crash at 0x249374, as in the report. */
        compat_runtime32_call(0x00249350, args, 3);
        return -1;
    }
    /* A changed image must be rejected without installing the hook. */
    uint8_t original[7];
    memcpy(original, (void *)(uintptr_t)guard->enabled_check.address, sizeof(original));
    uint8_t changed = original[0] ^ 1;
    ACH_CHECK(!write_guest_code(guard->enabled_check.address, &changed, 1, "selftest mismatch"));
    ACH_CHECK(install_steam_achievement_guard() == -1);
    ACH_CHECK(*(uint8_t *)(uintptr_t)guard->enabled_check.address == changed);
    ACH_CHECK(!write_guest_code(guard->enabled_check.address, original, sizeof(original), "selftest restore"));
    ACH_CHECK(!install_steam_achievement_guard());
    ACH_CHECK(compat_runtime32_call(0x00249350, args, 3) == 0);
    ACH_CHECK(!compat_runtime32_last_call_trapped());

    uint32_t allocation = compat_runtime32_allocate(128, 1);
    ACH_CHECK(allocation);
    volatile uint32_t *cells = (void *)(uintptr_t)allocation;
    /* cells: object, vtable[11], Set/Store counters, captured self/name,
       SetAchievement return value. Fake methods execute in guest mode. */
    cells[0] = allocation + 4;
    cells[8] = 0x7f00f500;
    cells[11] = 0x7f00f540;
    unsigned char code[32];
    size_t offset = 0;
    emit_u8(code, &offset, 0xff); emit_u8(code, &offset, 0x05);
    emit_u32(code, &offset, allocation + 48); /* inc Set counter */
    const unsigned char self_load[] = {0x8b,0x44,0x24,0x04,0xa3};
    memcpy(code + offset, self_load, sizeof(self_load)); offset += sizeof(self_load);
    emit_u32(code, &offset, allocation + 56);
    const unsigned char name_load[] = {0x8b,0x44,0x24,0x08,0xa3};
    memcpy(code + offset, name_load, sizeof(name_load)); offset += sizeof(name_load);
    emit_u32(code, &offset, allocation + 60);
    emit_u8(code, &offset, 0xa1); emit_u32(code, &offset, allocation + 64);
    emit_u8(code, &offset, 0xc3);
    ACH_CHECK(!write_guest_code(0x7f00f500, code, offset, "selftest SetAchievement"));
    offset = 0;
    emit_u8(code, &offset, 0xff); emit_u8(code, &offset, 0x05);
    emit_u32(code, &offset, allocation + 52); /* inc Store counter */
    emit_u8(code, &offset, 0xb8); emit_u32(code, &offset, 1);
    emit_u8(code, &offset, 0xc3);
    ACH_CHECK(!write_guest_code(0x7f00f540, code, offset, "selftest StoreStats"));
    uint32_t name = compat_runtime32_copy_cstring("test-only-achievement");
    ACH_CHECK(name);
    *(uint32_t *)(uintptr_t)0x0160b240 = name; /* achievement index 1 */
    *stats = allocation;
    cells[16] = 1;
    *enabled = 0;
    ACH_CHECK(compat_runtime32_call(0x00249350, args, 3) == 0);
    ACH_CHECK(cells[12] == 0 && cells[13] == 0);
    *enabled = 1;
    *ready = 1;
    ACH_CHECK(compat_runtime32_call(0x00249350, args, 3) == 1);
    ACH_CHECK(cells[12] == 1 && cells[13] == 1);
    ACH_CHECK(cells[14] == allocation && cells[15] == name && *ready == 1);
    cells[16] = 0;
    ACH_CHECK(compat_runtime32_call(0x00249350, args, 3) == 1);
    ACH_CHECK(cells[12] == 2 && cells[13] == 1 && *ready == 0);
    *stats = 0;
    ACH_CHECK(compat_runtime32_call(0x00249350, args, 3) == 0);
    ACH_CHECK(cells[12] == 2 && cells[13] == 1);
    ACH_CHECK(!compat_runtime32_last_call_trapped());
    puts("Steam achievement guard PASS (signature, NULL, disabled, live calls, failure, NULL again)");
    return 0;
#undef ACH_CHECK
}
