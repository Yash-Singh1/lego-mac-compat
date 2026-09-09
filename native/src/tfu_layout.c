#include "tfu_layout.h"
#include <stdio.h>
#include <string.h>

/* Relocation-independent instruction signatures from Aspyr's Mac routines.
   Wildcards cover call/branch displacements and linked addresses, never object
   field offsets. Kept as bytes so the distributed loader needs no disassembler. */
static const char state_pattern[] =
    "55 89 e5 83 ec 28 89 5d f4 89 75 f8 89 7d fc 8b 75 0c 80 3d ?? ?? ?? ?? 00 "
    "75 ?? e8 ?? ?? ?? ?? 8b 45 08 8d 3c 80 b8 8f 04 00 00 8b 14 fd ?? ?? ?? ?? "
    "85 d2 0f 84 ?? ?? ?? ?? 80 3d ?? ?? ?? ?? 00 0f 84 ?? ?? ?? ?? 8b 1d ?? ?? "
    "?? ?? 89 1c 24 e8 ?? ?? ?? ?? c7 44 24 0c 01 00 00 00 c7 44 24 04 00 00 00";
static const char caps_pattern[] =
    "55 89 e5 56 53 83 ec 10 8b 75 08 8b 5d 10 80 3d ?? ?? ?? ?? 00 75 ?? e8 ?? "
    "?? ?? ?? 8d 14 b6 b8 8f 04 00 00 8b 0c d5 ?? ?? ?? ?? 85 c9 0f 84 ?? ?? ?? "
    "?? 0f b6 04 d5 ?? ?? ?? ?? 3c 01 74 ?? 72 ?? 3c 02 74 ?? 3c 03 0f 85 ?? ?? "
    "?? ?? e9 ?? ?? ?? ?? c6 03 01 c6 43 01 01 66 c7 43 02 00 00 66 c7 43 04 ff";
static const char vibration_pattern[] =
    "55 89 e5 56 53 83 ec 10 8b 5d 08 8b 75 0c 80 3d ?? ?? ?? ?? 00 75 ?? e8 ?? "
    "?? ?? ?? 8d 14 9b b8 8f 04 00 00 8b 1c d5 ?? ?? ?? ??";
static const char input_pattern[] =
    "55 89 e5 83 ec 18 89 5d f8 89 75 fc 8b 5d 08 8b 75 14 8d 53 74 8b 45 10 89 "
    "44 24 08 8b 45 0c 89 44 24 04 89 14 24 e8 ?? ?? ?? ?? 89 74 24 04 89 1c 24 "
    "e8 ?? ?? ?? ?? 89 5d 08 8b 5d f8 8b 75 fc c9 e9 ?? ?? ?? ??";
static const char render_pattern[] =
    "55 89 e5 57 56 53 81 ec cc 00 00 00 c7 44 24 20 00 00 00 00 c7 44 24 1c 00 "
    "00 00 00 c7 44 24 18 ?? ?? ?? ?? c7 44 24 14 00 00 00 00 c7 44 24 10 00 00 "
    "00 00 c7 44 24 0c 00 00 00 00 c7 44 24 08 00 00 00 00 c7 44 24 04 02 00 00 "
    "00 8b 35 ?? ?? ?? ?? 8b 06 89 04 24 e8 ?? ?? ?? ?? 8b 45 08 8b 58 04 85 db";
static const char next_pattern[] =
    "55 89 e5 83 ec 18 89 5d f8 89 75 fc 8b 5d 08 85 db 74 ?? 8b 43 10 85 c0 74 "
    "?? 8b 43 0c 40 89 43 0c 3b 43 08 7e ?? 8b 35 ?? ?? ?? ?? 89 34 24 e8 ?? ?? "
    "?? ?? c7 43 0c 00 00 00 00 8b 43 10 89 04 24 e8 ?? ?? ?? ?? eb ?? 89 c3 89 "
    "34 24 e8 ?? ?? ?? ?? 89 1c 24 e8 ?? ?? ?? ?? 89 75 08 8b 5d f8 8b 75 fc c9";
static const char completion_pattern[] =
    "8b 7d 08 80 7f 0d 00 75 12 8b 43 0c 3b 43 08 72 0a";
static const char display_pattern[] =
    "b9 01 00 00 00 c7 85 ?? ?? ?? ?? 00 00 00 00 "
    "c7 85 ?? ?? ?? ?? 00 00 00 00 c7 85 ?? ?? ?? ?? 00 00 00 00";

static uint32_t unique(const uint8_t *code, size_t size, uint32_t base,
                       const char *pattern, const char *name)
{
    uint8_t bytes[128], mask[128];
    size_t length = 0;
    while (*pattern && length < sizeof(bytes)) {
        unsigned value = 0;
        mask[length] = pattern[0] != '?';
        if (mask[length] && sscanf(pattern, "%2x", &value) != 1) return 0;
        bytes[length++] = value;
        pattern += 2;
        if (*pattern == ' ') ++pattern;
    }
    if (!length || *pattern || size < length) return 0;
    uint32_t found = 0;
    for (size_t i = 0; i <= size - length; ++i) {
        if (code[i] != bytes[0]) continue;
        size_t j = 1;
        while (j < length && (!mask[j] || code[i+j] == bytes[j])) ++j;
        if (j != length) continue;
        if (found) {
            fprintf(stderr, "TFU compatibility: ambiguous %s routine; refusing to patch\n", name);
            return 0;
        }
        found = base + (uint32_t)i;
    }
    if (!found) fprintf(stderr, "TFU compatibility: cannot identify %s routine in this Mac build\n", name);
    return found;
}

static int contains(size_t size, uint32_t base, uint32_t address, size_t length)
{
    return address >= base && (uint64_t)(address - base) + length <= size;
}
static uint32_t target(const uint8_t *code, uint32_t base, uint32_t address)
{
    int32_t relative;
    memcpy(&relative, code + address - base + 1, 4);
    return address + 5u + (uint32_t)relative;
}

int tfu_layout_find(const uint8_t *code, size_t size, uint32_t base, uint32_t entry,
                    struct lp32_tfu_layout *layout, uint32_t *display, uint32_t *main)
{
    struct lp32_tfu_layout found = {0};
    if (!base || size > UINT32_MAX - base) return -1;
#define FIND(field, pattern) do { \
    found.field = unique(code, size, base, pattern##_pattern, #field); \
    if (!found.field) return -1; \
} while (0)
    FIND(xinput_state, state);
    FIND(xinput_caps, caps);
    FIND(xinput_vibration, vibration);
    FIND(input_update, input);
    FIND(movie_next_frame, next);
    FIND(movie_completion, completion);
#undef FIND
    uint32_t render = unique(code, size, base, render_pattern, "movie renderer");
    uint32_t sentinel = unique(code, size, base, display_pattern, "display sentinel");
    if (!render || !sentinel) return -1;
    /* The update signature covers the entire function including its tail
       jump. Follow that jump and validate the evaluator prologue. */
    found.input_tail = found.input_update + 55;
    found.input_evaluator = target(code, base, found.input_tail + 10);
    const uint8_t evaluator[] = {0x55,0x89,0xe5,0x57,0x56,0x53,0x83,0xec,0x1c};
    if (!contains(size, base, found.input_evaluator, sizeof(evaluator)) ||
        memcmp(code + found.input_evaluator - base, evaluator, sizeof(evaluator))) {
        fprintf(stderr, "TFU compatibility: incompatible input evaluator ABI\n"); return -1;
    }
    /* Locate the verified NextFrame call inside this renderer, rather than
       assuming an offset from its start. The completion path must be nearby. */
    if (found.movie_completion <= render || found.movie_completion - render > 4096) return -1;
    for (uint32_t a = render; a + 20 <= found.movie_completion; ++a) {
        if (memcmp(code + a - base, "\x89\x1c\x24\xe8", 4)) continue;
        /* Only the final advance after the frame upload. The earlier
           catch-up loop also calls NextFrame and must remain untouched. */
        if (memcmp(code + a - base + 8, "\x8b\x0e\x8b\x13\xf3\x0f\x2a\xc2\x85\xd2", 10)) continue;
        if (target(code, base, a + 3) != found.movie_next_frame) continue;
        if (found.movie_hook) { fprintf(stderr, "TFU compatibility: ambiguous movie advance call\n"); return -1; }
        found.movie_hook = a;
    }
    /* Aspyr crt: argc/argv setup then call main; mov [esp],eax; call exit; hlt.
       Restrict this search to the actual entry stub, not the rest of __text. */
    uint32_t main_address = 0;
    const uint8_t crt[] = {0x6a,0x00,0x89,0xe5,0x83,0xe4,0xf0};
    if (!contains(size, base, entry, 80) || memcmp(code + entry - base, crt, sizeof(crt))) return -1;
    for (uint32_t a = entry; a < entry + 64; ++a) {
        const uint8_t *p = code + a - base;
        if (p[0] != 0xe8 || memcmp(p + 5, "\x89\x44\x24\x00\xe8", 5) || p[14] != 0xf4) continue;
        if (main_address) return -1;
        main_address = target(code, base, a);
    }
    if (!found.movie_hook || !contains(size, base, main_address, 1)) {
        fprintf(stderr, "TFU compatibility: unsupported movie/entry control flow\n"); return -1;
    }
    *layout = found; *display = sentinel; *main = main_address;
    fprintf(stderr, "TFU compatibility: verified code patterns; main=%08x input=%08x movie=%08x display=%08x\n",
            main_address, found.input_update, found.movie_hook, sentinel);
    return 0;
}
