/* Exercise the actual i386 hook and x87 stack without launching the game. */
static int subtitle_panel_selftest(void)
{
    const struct lp32_subtitle_panel_patch *patch = lp32_profile()->subtitle_panel_patch;
    if (!patch) return install_subtitle_panel_patch();
#define PANEL_CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "subtitle panel selftest failed at line %d: %s\n", __LINE__, #condition); \
    return -1; } } while (0)
    const uint8_t changed = 0xcc, ret = 0xc3;
    PANEL_CHECK(!write_guest_code(patch->draw_call.address, &changed, 1, "test mismatch"));
    PANEL_CHECK(install_subtitle_panel_patch() == -1);
    PANEL_CHECK(*(uint8_t *)(uintptr_t)patch->draw_call.address == changed);
    PANEL_CHECK(!write_guest_code(patch->draw_call.address, patch->draw_call.expected, 5, "test restore"));
    PANEL_CHECK(!install_subtitle_panel_patch());
    /* Replace only the terminal draw in this disposable test process. */
    PANEL_CHECK(!write_guest_code(patch->draw_function, &ret, 1, "test draw return"));
    uint32_t memory = compat_runtime32_allocate(1024, 16);
    PANEL_CHECK(memory);
    float *camera = (void *)(uintptr_t)memory;
    float *size = (void *)(uintptr_t)(memory + 896);
    uint32_t args[] = {memory, 0, 0, memory + 896, 0, 0, 0, 127, 0};
    int32_t displacement;
    memcpy(&displacement, (void *)(uintptr_t)(patch->draw_call.address + 1), 4);
    PANEL_CHECK(patch->draw_call.address + 5 + displacement == kSubtitlePanelStub);
    const float fovs[] = {0.75f, 1.024765133857727f, 0.9f, 1.2f, 0.6f, 0.75f};
    for (unsigned repeat = 0; repeat < 32; ++repeat) {
        for (unsigned i = 0; i < sizeof(fovs) / sizeof(fovs[0]); ++i) {
            camera[patch->camera_fov_offset / 4] = fovs[i];
            size[0] = 0.93870002f; size[1] = 0.08640001f;
            size[2] = 0.0f; size[3] = 1.0f;
            compat_runtime32_call(kSubtitlePanelStub, args, 9);
            PANEL_CHECK(!compat_runtime32_last_call_trapped());
            /* Screen width must equal the reference camera after projection. */
            double projected = size[0] * fovs[i] / tan(fovs[i] * 0.5);
            double expected = 0.93870002f * 0.75 / tan(0.375);
            PANEL_CHECK(isfinite(projected) && fabs(projected - expected) < 0.000002);
            PANEL_CHECK(size[1] == 0.08640001f && size[2] == 0 && size[3] == 1);
            PANEL_CHECK(camera[patch->camera_fov_offset / 4] == fovs[i]);
            if (fovs[i] == 0.75f) PANEL_CHECK(size[0] == 0.93870002f);
        }
    }
    puts("subtitle panel PASS (signature, camera projection, unchanged normal scenes, x87 balance)");
    return 0;
#undef PANEL_CHECK
}
