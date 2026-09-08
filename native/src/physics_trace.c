#include "physics_trace.h"
#include "crash_trace.h"
#include "guest_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static uint32_t anim_original, tick_original, touch_original, destroy_original, manager;
static unsigned tick_depth;

/* These are the two deferred CUtlVectors in the UUID-validated server. Keep
   entries in place during callbacks: the guest caches a count and an index.
   Removing entries there would skip successors or overrun the shortened list. */
struct pending_vector { uint32_t data; int32_t capacity, grow, count; uint32_t elements; };
static struct pending_vector *pending(uint32_t object, unsigned offset, unsigned stride)
{
    struct pending_vector *v = (void *)(uintptr_t)(object+offset);
    if (v->count < 0 || v->capacity < v->count ||
        (v->count && (!v->data || (uint64_t)v->data+(uint64_t)v->count*stride > UINT32_MAX))) {
        fprintf(stderr, "compat32: invalid deferred physics vector at 0x%08x\n", object+offset);
        abort();
    }
    return v;
}
static void invalidate_pending(uint32_t object, uint32_t deleted, uint32_t caller)
{
    unsigned removed = 0;
    for (unsigned n = 0; n < 2; ++n) {
        unsigned stride = n ? 4 : 16;
        struct pending_vector *v = pending(object, n ? 0x68 : 0x54, stride);
        for (int32_t i = 0; i < v->count; ++i) {
            uint32_t *entry = (void *)(uintptr_t)(v->data+(uint32_t)i*stride);
            if (*entry == deleted) { *entry = 0; ++removed; }
        }
    }
    if (removed) lp32_trace_record(LP32_TRACE_QUEUE_INVALIDATE, deleted, removed, caller, tick_depth);
}
static void compact_pending(uint32_t object)
{
    for (unsigned n = 0; n < 2; ++n) {
        unsigned stride = n ? 4 : 16;
        struct pending_vector *v = pending(object, n ? 0x68 : 0x54, stride);
        int32_t count = 0, old_count = v->count;
        for (int32_t i = 0; i < old_count; ++i) {
            unsigned char *entry = (void *)(uintptr_t)(v->data+(uint32_t)i*stride);
            if (!*(uint32_t *)entry) continue;
            if (count != i) memmove((void *)(uintptr_t)(v->data+(uint32_t)count*stride), entry, stride);
            ++count;
        }
        v->count = count;
        if (count != old_count) lp32_trace_record(LP32_TRACE_QUEUE_COMPACT, object,
            (uint32_t)(old_count-count), n ? 0x68 : 0x54, count);
    }
}
static uint32_t word(uint32_t address)
{ uint32_t value = 0; lp32_crash_read(address, &value, 4); return value; }
static void entity(unsigned stage, uint32_t object, uint32_t caller)
{ lp32_trace_record(stage, object, 0, caller, word(object)); }
static void queue(uint32_t object, unsigned stage, uint32_t first, uint32_t second)
{
    /* Store counts even when damaged; cap reads, never trust a guest count. */
    const unsigned offsets[] = {0x54, 0x68};
    for (unsigned v = 0; v < 2; ++v) {
        uint32_t data = word(object + offsets[v]);
        uint32_t count = word(object + offsets[v] + 12);
        lp32_trace_record(LP32_TRACE_QUEUE, object, count, stage, data);
        uint32_t start = v ? second : first;
        if (start > count) start = 0;
        for (unsigned i = start; i < count && i-start < 256; ++i) {
            uint32_t address = data + i*(v ? 4 : 16);
            if (address < data) break;
            entity(LP32_TRACE_ENTITY, word(address), stage);
        }
    }
}
static uint64_t anim(const uint32_t *args, uint32_t caller)
{
    entity(LP32_TRACE_ANIM_BEFORE, args[0], caller);
    uint32_t first = word(manager+0x60), second = word(manager+0x74);
    uint32_t result = compat_runtime32_call(anim_original, args, 1);
    entity(LP32_TRACE_ANIM_AFTER, args[0], caller);
    queue(manager, LP32_TRACE_ANIM_AFTER, first, second);
    return result;
}
static uint64_t tick(const uint32_t *args, uint32_t caller)
{
    if (!tick_depth) compact_pending(args[0]);
    ++tick_depth;
    entity(LP32_TRACE_TICK_BEFORE, args[0], caller);
    queue(args[0], LP32_TRACE_TICK_BEFORE, 0, 0);
    uint32_t result = compat_runtime32_call(tick_original, args, 1);
    queue(args[0], LP32_TRACE_TICK_AFTER, 0, 0);
    --tick_depth;
    return result;
}
static uint64_t touch(const uint32_t *args, uint32_t caller)
{
    if (!args[0]) return 0; /* A destructor invalidated this queued entry. */
    entity(LP32_TRACE_TOUCH_BEFORE, args[0], caller);
    uint32_t result = compat_runtime32_call(touch_original, args, 2);
    entity(LP32_TRACE_TOUCH_AFTER, args[0], caller);
    return result;
}
static uint64_t destroy(const uint32_t *args, uint32_t caller)
{
    entity(LP32_TRACE_DESTROY_BEFORE, args[0], caller);
    invalidate_pending(manager, args[0], caller);
    uint32_t result = compat_runtime32_call(destroy_original, args, 1);
    entity(LP32_TRACE_DESTROY_AFTER, args[0], caller);
    return result;
}
lp32_fast_import_fn lp32_physics_trace_handler(const char *name)
{
    if (!strcmp(name, "_lp32_trace_physics_destroy")) return destroy;
    if (!strcmp(name, "_lp32_trace_physics_anim")) return anim;
    if (!strcmp(name, "_lp32_trace_physics_tick")) return tick;
    if (!strcmp(name, "_lp32_trace_physics_touch")) return touch;
    return NULL;
}
static void branch(unsigned char *code, uint32_t from, uint32_t to, unsigned opcode)
{ code[0] = opcode; uint32_t relative = to-from-5; memcpy(code+1, &relative, 4); }

void lp32_physics_trace_install(const char *path, uint32_t slide, const unsigned char uuid[16])
{
    static const unsigned char supported[16] = {
        0x04,0x20,0xdb,0xf7,0x9c,0xd0,0x31,0xe2,0x84,0xdd,0x61,0xdd,0xc6,0x92,0xb7,0x04};
    const char *name = strrchr(path, '/');
    if (strcmp(name ? name+1 : path, "server.dylib")) return;
    if (!uuid || memcmp(uuid, supported, 16)) {
        fprintf(stderr, "compat32: physics history unavailable for this server UUID; general crash capture remains enabled\n");
        return;
    }
    if (anim_original) return;
    const uint32_t offsets[] = {0x6131b0, 0x4bfb10, 0x4bfb52, 0x4bfb81, 0x2e4f50};
    const unsigned char signatures[5][6] = {
        {0x55,0x89,0xe5,0x53,0x57,0x56}, {0x55,0x89,0xe5,0x53,0x57,0x56},
        {0xe8,0x09,0xdd,0xb5,0xff,0}, {0xe8,0xda,0xdc,0xb5,0xff,0},
        {0x55,0x89,0xe5,0x53,0x57,0x56}};
    /* Guard BEFORE the original vtable load, not merely the indirect call.
       The zero-entry path rejoins the existing loop increment. Live entries
       execute the original bytes and keep the original guest call/ABI. */
    const uint32_t guard_offsets[] = {0x4bfbb6, 0x4bfbf6};
    const uint32_t guard_skips[] = {0x4bfbd1, 0x4bfc0e};
    const unsigned guard_lengths[] = {7, 9};
    const unsigned char guard_signatures[2][9] = {
        {0x8b,0x08,0x8b,0x55,0xf0,0x8b,0x12},
        {0x8b,0x08,0x8b,0x17,0xf3,0x0f,0x10,0x42,0x10}};
    uint32_t targets[5] = {
        compat_runtime32_guest_callback("_lp32_trace_physics_anim"),
        compat_runtime32_guest_callback("_lp32_trace_physics_tick"),
        compat_runtime32_guest_callback("_lp32_trace_physics_touch"), 0,
        compat_runtime32_guest_callback("_lp32_trace_physics_destroy")};
    targets[3] = targets[2];
    for (unsigned i = 0; i < 5; ++i) {
        unsigned char bytes[6];
        unsigned length = (i < 2 || i == 4) ? 6 : 5;
        if (!targets[i] || !lp32_crash_read(slide+offsets[i], bytes, length) ||
            memcmp(bytes, signatures[i], length)) {
            fprintf(stderr, "compat32: physics history signature mismatch; no hooks installed\n");
            return;
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        unsigned char bytes[9];
        if (!lp32_crash_read(slide+guard_offsets[i], bytes, guard_lengths[i]) ||
            memcmp(bytes, guard_signatures[i], guard_lengths[i])) {
            fprintf(stderr, "compat32: physics lifetime guard signature mismatch; no hooks installed\n");
            return;
        }
    }
    /* The guest mapping allocator owns and pads the page, protecting adjacent
       heap metadata and avoiding collisions with guest thread stacks. */
    uint32_t trampoline = guest_memory32_map(0, 4096, PROT_READ|PROT_WRITE,
                                             MAP_PRIVATE|MAP_ANON, -1, 0);
    if (trampoline == UINT32_MAX) return;
    void *memory = (void *)(uintptr_t)trampoline;
    for (unsigned i = 0; i < 3; ++i) {
        unsigned hook = i == 2 ? 4 : i;
        unsigned char *code = (unsigned char *)memory+i*16;
        memcpy(code, signatures[hook], 6);
        branch(code+6, trampoline+i*16+6, slide+offsets[hook]+6, 0xe9);
    }
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t address = trampoline+64+i*32;
        unsigned char *code = (void *)(uintptr_t)address;
        code[0]=0x85; code[1]=0xc0; /* test eax,eax */
        code[2]=0x0f; code[3]=0x84; /* jz loop increment */
        uint32_t relative = slide+guard_skips[i]-(address+8);
        memcpy(code+4, &relative, 4);
        memcpy(code+8, guard_signatures[i], guard_lengths[i]);
        branch(code+8+guard_lengths[i], address+8+guard_lengths[i],
               slide+guard_offsets[i]+guard_lengths[i], 0xe9);
    }
    if (mprotect(memory, 4096, PROT_READ|PROT_EXEC)) return;
    unsigned page_size = (unsigned)getpagesize();
    uint32_t pages[5];
    for (unsigned i = 0; i < 5; ++i) {
        pages[i] = (slide+offsets[i]) & ~(page_size-1);
        if (mprotect((void *)(uintptr_t)pages[i], page_size, PROT_READ|PROT_WRITE|PROT_EXEC)) {
            for (unsigned j = 0; j < i; ++j)
                mprotect((void *)(uintptr_t)pages[j], page_size, PROT_READ|PROT_EXEC);
            return;
        }
    }
    anim_original = trampoline; tick_original = trampoline+16;
    destroy_original = trampoline+32;
    touch_original = slide+0x1d860; manager = slide+0xc8aed8;
    for (unsigned i = 0; i < 5; ++i) {
        unsigned char code[6] = {0,0,0,0,0,0x90};
        branch(code, slide+offsets[i], targets[i], (i < 2 || i == 4) ? 0xe9 : 0xe8);
        unsigned length = (i < 2 || i == 4) ? 6 : 5;
        memcpy((void *)(uintptr_t)(slide+offsets[i]), code, length);
        __builtin___clear_cache((char *)(uintptr_t)(slide+offsets[i]),
                                (char *)(uintptr_t)(slide+offsets[i]+length));
    }
    for (unsigned i = 0; i < 2; ++i) {
        unsigned char code[9]; memset(code, 0x90, sizeof(code));
        branch(code, slide+guard_offsets[i], trampoline+64+i*32, 0xe9);
        memcpy((void *)(uintptr_t)(slide+guard_offsets[i]), code, guard_lengths[i]);
        __builtin___clear_cache((char *)(uintptr_t)(slide+guard_offsets[i]),
                               (char *)(uintptr_t)(slide+guard_offsets[i]+guard_lengths[i]));
    }
    for (unsigned i = 0; i < 5; ++i)
        mprotect((void *)(uintptr_t)pages[i], page_size, PROT_READ|PROT_EXEC);
    fprintf(stderr, "compat32: physics history enabled (animation, pending entities, trigger callbacks, entity destruction)\n");
    fprintf(stderr, "compat32: deferred physics lifetime fix enabled (destruction invalidation, stable callback iteration)\n");
}

/* Regression fixture executes the supported server's end-of-tick instruction
   sequence, including its cached pusher count and both vtable loads. Only its
   dependencies are fixtures. This catches a guard at the wrong instruction or
   list compaction that skips another entity during a callback. */
static void test_queues(uint32_t base, const uint32_t *pushers, unsigned np,
                         const uint32_t *children, unsigned nc)
{
    struct pending_vector *p = (void *)(uintptr_t)(base+0xc8aed8+0x54);
    struct pending_vector *c = (void *)(uintptr_t)(base+0xc8aed8+0x68);
    *p = (struct pending_vector){base+0xc8b800, 8, 0, (int32_t)np, base+0xc8b800};
    *c = (struct pending_vector){base+0xc8b900, 8, 0, (int32_t)nc, base+0xc8b900};
    for (unsigned i=0; i<np; ++i) {
        uint32_t *entry=(void *)(uintptr_t)(p->data+i*16);
        entry[0]=pushers[i]; entry[1]=100+i; entry[2]=200+i; entry[3]=300+i;
    }
    if (nc) memcpy((void *)(uintptr_t)c->data, children, nc*4);
}
static void test_object(uint32_t base, uint32_t object)
{
    memset((void *)(uintptr_t)object, 0, 128);
    *(uint32_t *)(uintptr_t)object = base+0xc8b400;
}
int lp32_physics_trace_selftest(void)
{
    static const unsigned char uuid[16] = {
        0x04,0x20,0xdb,0xf7,0x9c,0xd0,0x31,0xe2,0x84,0xdd,0x61,0xdd,0xc6,0x92,0xb7,0x04};
    uint32_t base = guest_memory32_map(0, 0xc8c000, PROT_READ|PROT_WRITE|PROT_EXEC,
                                      MAP_PRIVATE|MAP_ANON, -1, 0);
    if (base == UINT32_MAX) return 1;
    unsigned char *memory = (void *)(uintptr_t)base;
    const unsigned char body[] = {0x55,0x89,0xe5,0x53,0x57,0x56,
        0x8b,0x45,0x08,0xff,0x40,0x04,0x8b,0x40,0x04,0x5e,0x5f,0x5b,0xc9,0xc3};
    memcpy(memory+0x6131b0, body, sizeof(body));
    memcpy(memory+0x2e4f50, body, sizeof(body));
    /* Trigger counts calls, preserves the old origin, and optionally invokes
       a destructor once, using the callback/object in this+8/this+12. */
    const unsigned char touch_body[] = {
        0x55,0x89,0xe5,0x53,0x57,0x56,0x83,0xec,0x0c,
        0x8b,0x45,0x08,0x81,0x38,0,0,0,0,0x75,0x25,
        0xff,0x40,0x04,0x8b,0x55,0x0c,0x85,0xd2,
        0x74,0x05,0x8b,0x0a,0x89,0x48,0x14,
        0x8b,0x48,0x08,0x85,0xc9,0x74,0x0f,
        0xc7,0x40,0x08,0,0,0,0,0x8b,0x40,0x0c,0x89,0x04,0x24,0xff,0xd1,
        0x8b,0x45,0x08,0x8b,0x40,0x04,0x83,0xc4,0x0c,0x5e,0x5f,0x5b,0xc9,0xc3};
    memcpy(memory+0x1d860, touch_body, sizeof(touch_body));
    /* In the captured failure PhysicsTouchTriggers returned without touching
       the filename allocation; let the negative control reach the same later
       virtual call rather than interpreting text as our test-only callback. */
    *(uint32_t *)(memory+0x1d860+14)=base+0xc8b400;
    const unsigned char original_tick[] = {
        0x55,0x89,0xe5,0x53,0x57,0x56,0x83,0xec,0x1c,0xe8,0x00,0x00,0x00,0x00,0x58,0x89,
        0x45,0xe8,0x8b,0x4d,0x08,0x8b,0x41,0x60,0x89,0x45,0xec,0x85,0xc0,0x7e,0x30,0xbf,
        0x04,0x00,0x00,0x00,0x8b,0x5d,0xec,0x66,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,
        0x8b,0x41,0x54,0x89,0xce,0x8b,0x4c,0x38,0xfc,0x01,0xf8,0x89,0x44,0x24,0x04,0x89,
        0x0c,0x24,0xe8,0x09,0xdd,0xb5,0xff,0x89,0xf1,0x83,0xc7,0x10,0x4b,0x75,0xe1,0x8b,
        0x41,0x74,0x85,0xc0,0x89,0xcb,0x7e,0x26,0x31,0xff,0x66,0x0f,0x1f,0x44,0x00,0x00,
        0x8b,0x43,0x68,0x8b,0x04,0xb8,0x89,0x04,0x24,0xc7,0x44,0x24,0x04,0x00,0x00,0x00,
        0x00,0xe8,0xda,0xdc,0xb5,0xff,0x47,0x8b,0x43,0x74,0x39,0xc7,0x7c,0xe2,0x8b,0x75,
        0xec,0x85,0xf6,0x7e,0x45,0x31,0xff,0x8b,0x45,0xe8,0x8b,0x80,0xfa,0x44,0x59,0x00,
        0x89,0x45,0xf0,0x66,0x66,0x66,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,
        0x8b,0x43,0x54,0x8b,0x04,0x38,0x8b,0x08,0x8b,0x55,0xf0,0x8b,0x12,0xf3,0x0f,0x10,
        0x42,0x10,0xf3,0x0f,0x11,0x44,0x24,0x04,0x89,0x04,0x24,0xff,0x91,0x84,0x02,0x00,
        0x00,0x83,0xc7,0x10,0x4e,0x75,0xd9,0x8b,0x43,0x74,0x85,0xc0,0x7e,0x36,0x31,0xf6,
        0x8b,0x45,0xe8,0x8b,0xb8,0xfa,0x44,0x59,0x00,0x0f,0x1f,0x80,0x00,0x00,0x00,0x00,
        0x8b,0x43,0x68,0x8b,0x04,0xb0,0x8b,0x08,0x8b,0x17,0xf3,0x0f,0x10,0x42,0x10,0xf3,
        0x0f,0x11,0x44,0x24,0x04,0x89,0x04,0x24,0xff,0x91,0x84,0x02,0x00,0x00,0x46,0x3b,
        0x73,0x74,0x7c,0xdc,0xc7,0x43,0x60,0x00,0x00,0x00,0x00,0xc7,0x43,0x74,0x00,0x00,
        0x00,0x00,0x83,0xc4,0x1c,0x5e,0x5f,0x5b,0x5d,0xc3,
    };
    memcpy(memory+0x4bfb10, original_tick, sizeof(original_tick));
    *(uint32_t *)(memory+0xa54018) = base+0xc8b200;
    *(uint32_t *)(memory+0xc8b200) = base+0xc8b300;
    *(uint32_t *)(memory+0xc8b310) = 0x3d088889; /* frame time */
    const unsigned char shadow[] = {0x8b,0x44,0x24,0x04,0xff,0x40,0x10,0xc3};
    memcpy(memory+0xc8b700, shadow, sizeof(shadow));
    *(uint32_t *)(memory+0xc8b684) = base+0xc8b700;
    const unsigned char wrong_uuid[16] = {0};
    lp32_physics_trace_install("fixture/server.dylib", base, wrong_uuid);
    if (anim_original || memory[0x6131b0] != 0x55) return 2;
    memory[0x6131b0] = 0x90;
    lp32_physics_trace_install("fixture/server.dylib", base, uuid);
    if (anim_original || memory[0x4bfb10] != 0x55) return 3;
    memory[0x6131b0] = 0x55;
    memory[0x4bfbb6] = 0x90;
    lp32_physics_trace_install("fixture/server.dylib", base, uuid);
    if (anim_original || memory[0x4bfb10] != 0x55) return 4;
    memory[0x4bfbb6] = 0x8b;
    lp32_physics_trace_install("fixture/server.dylib", base, uuid);
    if (!anim_original) return 5;
    uint32_t dead=base+0xc8b000, live=base+0xc8b080, third=base+0xc8b100;
    uint32_t pushed=base+0xc8aed8;
    test_object(base, dead); test_object(base, live); test_object(base, third);
    if (compat_runtime32_call(base+0x6131b0, &dead, 1) != 1) return 6;

    /* The reported crash: all aliases invalidated, allocation reused as text,
       then another tick. Compaction must retain the live pusher's old origin. */
    uint32_t p1[]={dead, live}, c1[]={dead, live, dead};
    test_queues(base,p1,2,c1,3);
    const char *mode = getenv("LP32_PHYSICS_TRACE_SELFTEST");
    int unfixed = mode && !strcmp(mode,"unfixed");
    /* Negative control bypasses only the fixture's destructor hook. It must
       reproduce SIGSEGV in the original consumer with the filename payload. */
    compat_runtime32_call(unfixed ? destroy_original : base+0x2e4f50,&dead,1);
    if (!unfixed && (word(base+0xc8b800) || word(base+0xc8b900) || word(base+0xc8b908))) return 7;
    const char reused[]="models/player/eggbot/eggbot_skin_burst.mdl";
    memcpy((void *)(uintptr_t)dead,reused,sizeof(reused));
    compat_runtime32_call(base+0x4bfb10,&pushed,1);
    if (word(live+4)!=2 || word(live+16)!=2 || word(live+20)!=101 ||
        memcmp((void *)(uintptr_t)dead,reused,sizeof(reused)) || word(pushed+0x60) || word(pushed+0x74)) return 8;

    /* A trigger deletes a later entry in both lists: no index shift, no skipped
       successor, and both displaced vtable-load guards must actually run. */
    test_object(base,dead); test_object(base,live); test_object(base,third);
    uint32_t p2[]={live,dead,third}, c2[]={dead,third};
    test_queues(base,p2,3,c2,2);
    *(uint32_t *)(uintptr_t)(live+8)=base+0x2e4f50;
    *(uint32_t *)(uintptr_t)(live+12)=dead;
    compat_runtime32_call(base+0x4bfb10,&pushed,1);
    if (word(dead+4)!=1 || word(dead+16) || word(third+4)!=2 ||
        word(third+16)!=2 || word(third+20)!=102 || word(live+16)!=1) return 9;

    /* Deletion of the current trigger object must suppress its later shadow
       update as well as its duplicate in the child vector. */
    test_object(base,dead); test_object(base,live);
    uint32_t p3[]={dead,live}, c3[]={dead,live};
    test_queues(base,p3,2,c3,2);
    *(uint32_t *)(uintptr_t)(dead+8)=base+0x2e4f50;
    *(uint32_t *)(uintptr_t)(dead+12)=dead;
    compat_runtime32_call(base+0x4bfb10,&pushed,1);
    if (word(dead+16) || word(live+4)!=2 || word(live+16)!=2) return 10;

    /* Reusing the address for a NEW queued entity is valid. Invalidating old
       entries must not blacklist that address or discard the new work. */
    test_object(base,dead);
    uint32_t c4[]={dead}; test_queues(base,NULL,0,c4,1);
    compat_runtime32_call(base+0x2e4f50,&dead,1);
    test_object(base,dead);
    *(uint32_t *)(memory+0xc8b904)=dead;
    *(uint32_t *)(uintptr_t)(pushed+0x74)=2;
    compat_runtime32_call(base+0x4bfb10,&pushed,1);
    if (word(dead+4)!=1 || word(dead+16)!=1) return 11;
    lp32_crash_capture(STDERR_FILENO,0,1,1,NULL,0,1);
    fprintf(stderr,"Physics history self-test: PASS (UUID/signature rejection, original results, nested guest calls)\n");
    fprintf(stderr,"Physics lifetime regression: PASS (deleted allocation reused as text, duplicate references, deletion during callbacks, live successors, new entity at reused address)\n");
    return 0;
}
