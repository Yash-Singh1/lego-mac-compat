#include "tfu_input.h"
#include "compat_runtime.h"
#include "game_profile.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* TFU's keyboard/mouse update tail-calls its binding evaluator. Run
   that evaluator unchanged, then OR DIK_F into ForceGrip only when the
   configured gameplay map is active. Menus use a separate fixed map.
   Input bytes already include the game's normal focus filtering. */
static const char callback_name[] = "_lp32_tfu_evaluate_grip_alias";
static uint32_t callback;
static void *ptr(uint32_t value) { return (void *)(uintptr_t)value; }

int tfu_input32_dispatch(const char *name, const uint32_t *args, uint64_t *result)
{
    if (!callback || strcmp(name, callback_name)) return 0;
    *result = compat_runtime32_call(lp32_profile()->tfu->input_evaluator, args, 1);
    if (!compat_runtime32_last_call_trapped()) {
        uint8_t *manager = ptr(args[0]);
        if (*(uint32_t *)(manager + 0x20) == 2 && manager[0x24])
            manager[0x10 + 15] |= manager[0x74 + 0x21];
    }
    return 1;
}

int tfu_input32_install(void)
{
    if (lp32_profile()->title != LP32_TITLE_TFU || callback) return 0;
    /* Verify the update's final argument setup, register restoration, and
       original evaluator target before changing its five-byte tail jump. */
    const struct lp32_tfu_layout *layout = lp32_profile()->tfu;
    uint8_t tail[] = {0x89,0x5d,0x08,0x8b,0x5d,0xf8,0x8b,0x75,0xfc,0xc9,
        0xe9,0xfc,0xfa,0xff,0xff};
    const uint8_t evaluator[] = {0x55,0x89,0xe5,0x57,0x56,0x53,0x83,0xec,0x1c};
    int32_t original = (int32_t)(layout->input_evaluator - (layout->input_tail + sizeof(tail)));
    memcpy(tail + 11, &original, 4);
    if (memcmp(ptr(layout->input_tail), tail, sizeof(tail)) ||
        memcmp(ptr(layout->input_evaluator), evaluator, sizeof(evaluator))) {
        fprintf(stderr, "compat32: TFU grip alias signature mismatch\n"); return -1;
    }
    uint32_t thunk = compat_runtime32_guest_callback(callback_name);
    if (!thunk) return -1;
    uintptr_t page_size = getpagesize(), page = (layout->input_tail + 10) & ~(page_size - 1);
    size_t span = ((layout->input_tail + 15 + page_size - 1) & ~(page_size - 1)) - page;
    if (mprotect((void *)page, span, PROT_READ | PROT_WRITE | PROT_EXEC)) return -1;
    int32_t relative = (int32_t)(thunk - (layout->input_tail + 15));
    memcpy(ptr(layout->input_tail + 11), &relative, sizeof(relative));
    __builtin___clear_cache(ptr(layout->input_tail + 10), ptr(layout->input_tail + 15));
    if (mprotect((void *)page, span, PROT_READ | PROT_EXEC)) return -1;
    callback = thunk;
    fprintf(stderr, "compat32: TFU F -> ForceGrip alias enabled; existing mouse/controller bindings retained\n");
    return 0;
}

int tfu_input32_self_test(void)
{
    if (!callback) return -1;
    uint32_t memory = compat_runtime32_allocate(2048, 1);
    if (!memory) return -1;
    uint8_t *manager = ptr(memory);
    uint8_t *keys = ptr(memory + 1024), *mouse = ptr(memory + 1280);
    *(uint32_t *)(manager + 0x70) = memory + 1320; /* mouse options */
    uint32_t update_args[] = {memory, memory + 1024, 256, memory + 1280};
    /* Supply the real evaluator's 16 button + 8 axis binding records and
       keyboard/mouse snapshot. This exercises guest code, not a mock OR. */
    struct binding { uint32_t device, action, code; } *bindings = ptr(memory + 512);
    for (unsigned i = 0; i < 24; ++i) bindings[i] = (struct binding){0, i, 0};
    bindings[15] = (struct binding){1, 15, 1}; /* original RMB grip */
    bindings[11] = (struct binding){0, 11, 0x12}; /* E: Force Push */
    *(uint32_t *)(manager + 0x20) = 2;
    manager[0x24] = 1;
    *(uint32_t *)(manager + 0x58) = memory + 512;
    *(uint32_t *)(manager + 0x5c) = memory + 512 + 24 * sizeof(*bindings);
    *(uint32_t *)(manager + 0x60) = *(uint32_t *)(manager + 0x5c);
    const uint8_t sequence[][2] = {
        {0,0}, {0x80,0}, {0x80,0}, {0,0}, /* F press/hold/release */
        {0,0x80}, {0x80,0x80}, {0,0x80}, {0,0}, /* release F first */
        {0x80,0x80}, {0x80,0}, {0,0} /* release RMB first */
    };
    int failed = 0;
    for (unsigned i = 0; i < sizeof(sequence)/sizeof(sequence[0]); ++i) {
        keys[0x21] = sequence[i][0];
        mouse[0xc + 1] = sequence[i][1];
        keys[0x12] = 0x80;
        compat_runtime32_call(lp32_profile()->tfu->input_update, update_args, 4);
        failed |= compat_runtime32_last_call_trapped();
        failed |= manager[0x1f] != (sequence[i][0] | sequence[i][1]);
        failed |= manager[0x1b] != 0x80;
    }
    /* F must not become a menu action. */
    *(uint32_t *)(manager + 0x20) = 0;
    manager[0x24] = 0;
    keys[0x21] = 0x80;
    compat_runtime32_call(lp32_profile()->tfu->input_update, update_args, 4);
    failed |= compat_runtime32_last_call_trapped() || manager[0x1f] != 0;
    compat_runtime32_deallocate(memory);
    fprintf(stderr, "TFU grip alias self-test: %s guest update/evaluator, F/RMB press-hold-release, both release orders, other actions, menu exclusion\n", failed ? "FAIL" : "PASS");
    return failed ? -1 : 0;
}
