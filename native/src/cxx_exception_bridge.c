#include "cxx_exception_bridge.h"
#include "compat_runtime.h"
#include "objc_bridge.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* The guest's libc++abi entry points. Exceptions live in guest memory with a
   private header; guest code only hands them back to these functions.
   Frames are unwound with the image's compact unwind info (MW2 has no
   __eh_frame) and GCC LSDAs, then the guest resumes at the landing pad with
   eax = exception, edx = selector, as __gxx_personality_v0 would arrange. */

static uint32_t read32(uint32_t address) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)address, 4); return v; }
static uint16_t read16(uint32_t address) { uint16_t v; memcpy(&v, (const void *)(uintptr_t)address, 2); return v; }
static uint8_t read8(uint32_t address) { return *(const uint8_t *)(uintptr_t)address; }
static void write32(uint32_t address, uint32_t value) { memcpy((void *)(uintptr_t)address, &value, 4); }

struct unwind_image { uint32_t text_start, text_end, info_start, info_end; };
static struct unwind_image unwind_images[8];
static unsigned unwind_image_count;
static pthread_mutex_t unwind_images_lock = PTHREAD_MUTEX_INITIALIZER;

void cxx_exception_bridge32_register_image(uint32_t text_start, uint32_t text_end,
                                           uint32_t unwind_info_start,
                                           uint32_t unwind_info_end)
{
    if (!unwind_info_start || unwind_info_end <= unwind_info_start) return;
    pthread_mutex_lock(&unwind_images_lock);
    if (unwind_image_count < sizeof(unwind_images) / sizeof(unwind_images[0]))
        unwind_images[unwind_image_count++] = (struct unwind_image){
            text_start, text_end, unwind_info_start, unwind_info_end};
    pthread_mutex_unlock(&unwind_images_lock);
}

struct frame_info { uint32_t start, end, encoding, lsda; };

enum {
    kModeMask = 0x0f000000, kModeEbpFrame = 0x01000000,
    kModeStackImmediate = 0x02000000, kModeStackIndirect = 0x03000000,
    kHasLsda = 0x40000000,
};

/* Compact unwind lookup (mach-o/compact_unwind_encoding.h, version 1). */
static bool find_frame_info(uint32_t pc, struct frame_info *info)
{
    struct unwind_image image = {0};
    pthread_mutex_lock(&unwind_images_lock);
    for (unsigned i = 0; i < unwind_image_count; ++i)
        if (pc >= unwind_images[i].text_start && pc < unwind_images[i].text_end) image = unwind_images[i];
    pthread_mutex_unlock(&unwind_images_lock);
    if (!image.info_start) return false;
    uint32_t u = image.info_start, base = image.text_start, offset = pc - base;
    if (read32(u) != 1) return false;
    uint32_t common_offset = read32(u + 4), common_count = read32(u + 8);
    uint32_t index_offset = read32(u + 20), index_count = read32(u + 24);
    if (index_count < 2) return false;
    uint32_t index = u + index_offset;
    if (offset < read32(index) || offset >= read32(index + 12 * (index_count - 1))) return false;
    uint32_t low = 0, high = index_count - 1;
    while (high - low > 1) {
        uint32_t middle = (low + high) / 2;
        if (read32(index + 12 * middle) <= offset) low = middle; else high = middle;
    }
    uint32_t entry = index + 12 * low, page = read32(entry + 4);
    uint32_t next_function = read32(entry + 12);
    if (!page) return false;
    page += u;
    uint32_t kind = read32(page), count = read16(page + 6), entries = page + read16(page + 4);
    if (!count) return false;
    uint32_t first = 0, last = count;
    uint32_t start = 0, end = next_function, encoding = 0;
    if (kind == 2) {
        while (last - first > 1) {
            uint32_t middle = (first + last) / 2;
            if (read32(entries + 8 * middle) <= offset) first = middle; else last = middle;
        }
        start = read32(entries + 8 * first);
        encoding = read32(entries + 8 * first + 4);
        if (first + 1 < count) end = read32(entries + 8 * (first + 1));
    } else if (kind == 3) {
        uint32_t function_base = read32(entry);
        uint32_t page_encodings = page + read16(page + 8);
        while (last - first > 1) {
            uint32_t middle = (first + last) / 2;
            if (function_base + (read32(entries + 4 * middle) & 0xffffff) <= offset) first = middle;
            else last = middle;
        }
        uint32_t word = read32(entries + 4 * first);
        start = function_base + (word & 0xffffff);
        if (first + 1 < count) end = function_base + (read32(entries + 4 * (first + 1)) & 0xffffff);
        uint32_t which = word >> 24;
        encoding = which < common_count ? read32(u + common_offset + 4 * which) :
                                          read32(page_encodings + 4 * (which - common_count));
    } else {
        return false;
    }
    if (offset < start || offset >= end) return false;
    uint32_t lsda = 0;
    if (encoding & kHasLsda) {
        uint32_t lsda_first = u + read32(entry + 8), lsda_last = u + read32(entry + 20);
        for (uint32_t p = lsda_first; p + 8 <= lsda_last; p += 8)
            if (read32(p) == start) { lsda = base + read32(p + 4); break; }
        if (!lsda) return false;
    }
    *info = (struct frame_info){base + start, base + end, encoding, lsda};
    return true;
}

/* One frame up, per libunwind's CompactUnwinder_x86. */
static bool step_frame(struct guest_resume_context *r, const struct frame_info *info)
{
    uint32_t encoding = info->encoding;
    uint32_t *const registers[] = {NULL, &r->ebx, NULL, NULL, &r->edi, &r->esi, &r->ebp};
    uint32_t mode = encoding & kModeMask;
    if (mode == kModeEbpFrame) {
        uint32_t saved = r->ebp - 4 * ((encoding >> 16) & 0xff);
        uint32_t locations = encoding & 0x7fff;
        for (int i = 0; i < 5; ++i, saved += 4, locations >>= 3) {
            unsigned reg = locations & 7;
            if (reg > 6) return false;
            if (registers[reg]) *registers[reg] = read32(saved);
        }
        uint32_t frame = r->ebp;
        r->ebp = read32(frame);
        r->eip = read32(frame + 4);
        r->esp = frame + 8;
        return true;
    }
    if (mode != kModeStackImmediate && mode != kModeStackIndirect) return false;
    uint32_t size = (encoding >> 16) & 0xff, adjust = (encoding >> 13) & 7;
    uint32_t count = (encoding >> 10) & 7, permutation = encoding & 0x3ff;
    size = mode == kModeStackIndirect ? read32(info->start + size) + 4 * adjust : size * 4;
    if (count > 6) return false;
    int order[6] = {0};
    static const int divisors[7][6] = {
        {0}, {1}, {5, 1}, {20, 4, 1}, {60, 12, 3, 1}, {120, 24, 6, 2, 1}, {120, 24, 6, 2, 1, 1},
    };
    for (uint32_t i = 0; i < count; ++i) {
        order[i] = (int)(permutation / divisors[count][i]);
        permutation -= (uint32_t)order[i] * divisors[count][i];
    }
    bool used[7] = {false};
    uint32_t saved = r->esp + size - 4 - 4 * count;
    for (uint32_t i = 0; i < count; ++i, saved += 4) {
        int renumbered = 0, reg = 0;
        for (int u = 1; u < 7; ++u) {
            if (used[u]) continue;
            if (renumbered++ == order[i]) { reg = u; used[u] = true; break; }
        }
        if (!reg) return false;
        if (reg == 6) r->ebp = read32(saved);
        else if (registers[reg]) *registers[reg] = read32(saved);
    }
    r->eip = read32(saved);
    r->esp = saved + 4;
    return true;
}

static uint32_t read_uleb(uint32_t *p)
{
    uint32_t result = 0; unsigned shift = 0; uint8_t byte;
    do { byte = read8((*p)++); if (shift < 32) result |= (uint32_t)(byte & 0x7f) << shift; shift += 7; } while (byte & 0x80);
    return result;
}
static int32_t read_sleb(uint32_t *p)
{
    uint32_t result = 0; unsigned shift = 0; uint8_t byte;
    do { byte = read8((*p)++); if (shift < 32) result |= (uint32_t)(byte & 0x7f) << shift; shift += 7; } while (byte & 0x80);
    if (shift < 32 && (byte & 0x40)) result |= ~UINT32_C(0) << shift;
    return (int32_t)result;
}
static bool read_encoded(uint32_t *p, uint8_t encoding, uint32_t *value)
{
    uint32_t start = *p, v;
    switch (encoding & 0x0f) {
        case 0x00: case 0x03: case 0x0b: v = read32(*p); *p += 4; break;
        case 0x01: v = read_uleb(p); break;
        case 0x09: v = (uint32_t)read_sleb(p); break;
        case 0x02: v = read16(*p); *p += 2; break;
        case 0x0a: v = (uint32_t)(int32_t)(int16_t)read16(*p); *p += 2; break;
        default: return false;
    }
    switch (encoding & 0x70) {
        case 0x00: break;
        case 0x10: v += start; break;
        default: return false;
    }
    if ((encoding & 0x80) && v) v = read32(v);
    *value = v;
    return true;
}

enum { kNoAction, kCleanup, kHandler };

static bool type_matches(uint32_t catch_type, uint32_t thrown_type)
{
    if (!catch_type || catch_type == thrown_type) return true;
    if (!thrown_type) return false;
    /* Itanium type_info: vtable, then the mangled name. */
    const char *a = (const char *)(uintptr_t)read32(catch_type + 4);
    const char *b = (const char *)(uintptr_t)read32(thrown_type + 4);
    return a && b && (uintptr_t)a >= 0x1000 && (uintptr_t)b >= 0x1000 && !strcmp(a, b);
}

/* __gxx_personality_v0's call-site scan. pc is the return address. */
static int scan_lsda(const struct frame_info *info, uint32_t pc, uint32_t thrown_type,
                     uint32_t *landing_pad, int32_t *selector)
{
    uint32_t p = info->lsda, offset = pc - 1 - info->start, lp_start = info->start;
    uint8_t lp_encoding = read8(p++);
    if (lp_encoding != 0xff && !read_encoded(&p, lp_encoding, &lp_start)) return kNoAction;
    uint8_t type_encoding = read8(p++);
    uint32_t class_info = 0;
    if (type_encoding != 0xff) { uint32_t delta = read_uleb(&p); class_info = p + delta; }
    uint8_t site_encoding = read8(p++);
    uint32_t sites_length = read_uleb(&p), action_table = p + sites_length;
    while (p < action_table) {
        uint32_t start, length, pad;
        if (!read_encoded(&p, site_encoding, &start) || !read_encoded(&p, site_encoding, &length) ||
            !read_encoded(&p, site_encoding, &pad)) return kNoAction;
        uint32_t action = read_uleb(&p);
        if (offset < start) break;
        if (offset >= start + length) continue;
        if (!pad) return kNoAction;
        *landing_pad = lp_start + pad;
        *selector = 0;
        if (!action) return kCleanup;
        bool cleanup = false;
        uint32_t record = action_table + action - 1;
        for (;;) {
            int32_t filter = read_sleb(&record);
            uint32_t displacement_at = record;
            int32_t displacement = read_sleb(&record);
            if (filter > 0 && class_info) {
                uint32_t entry = class_info - (uint32_t)filter * 4, type = 0;
                if (read_encoded(&entry, type_encoding, &type) && type_matches(type, thrown_type)) {
                    *selector = filter;
                    return kHandler;
                }
            } else if (filter == 0) {
                cleanup = true;
            }
            if (!displacement) break;
            record = displacement_at + (uint32_t)displacement;
        }
        return cleanup ? kCleanup : kNoAction;
    }
    return kNoAction;
}

enum { kHeaderSize = 48, kMagic = 0x4c504548 };
struct guest_exception {
    uint32_t magic;
    uint32_t object;         /* a dependent exception's is its primary's */
    int32_t references;      /* primary: live throw/handler + exception_ptrs + dependents */
    uint32_t type_info;
    uint32_t destructor;
    int32_t handler_count;
    uint32_t next_caught;
    uint32_t handler_esp;
    int32_t handler_selector;
    uint32_t dependent;
    uint32_t reserved;
    uint32_t unwind;         /* landing pads receive this field's address */
};
_Static_assert(sizeof(struct guest_exception) == kHeaderSize, "exception header");
enum { kUnwindOffset = offsetof(struct guest_exception, unwind) };

static struct guest_exception *header_at(uint32_t address) { return (void *)(uintptr_t)address; }
static struct guest_exception *primary_of(uint32_t object) { return header_at(object - kHeaderSize); }

static _Thread_local uint32_t caught_exceptions;
static _Thread_local int32_t uncaught_exceptions;

void cxx_exception_bridge32_retain(uint32_t object)
{
    if (object) __atomic_add_fetch(&primary_of(object)->references, 1, __ATOMIC_RELAXED);
}

void cxx_exception_bridge32_release(uint32_t object)
{
    if (!object) return;
    struct guest_exception *primary = primary_of(object);
    if (__atomic_sub_fetch(&primary->references, 1, __ATOMIC_ACQ_REL)) return;
    if (primary->destructor) compat_runtime32_call(primary->destructor, &object, 1);
    compat_runtime32_deallocate(object - kHeaderSize);
}

/* Handler lifetime ended: a dependent header goes away; a primary drops its
   throw reference. */
static void release_header(uint32_t address)
{
    struct guest_exception *header = header_at(address);
    uint32_t object = header->object;
    if (header->dependent) compat_runtime32_deallocate(address);
    cxx_exception_bridge32_release(object);
}

static const char *type_name(uint32_t type_info)
{
    if (!type_info) return "<unknown>";
    uint32_t name = read32(type_info + 4);
    return name >= 0x1000 ? (const char *)(uintptr_t)name : "<unnamed>";
}

static void terminate(const char *reason, uint32_t header, const uint32_t *arguments,
                      uint32_t return_address)
{
    fprintf(stderr, "compat32: std::terminate: %s (exception type %s, from 0x%08" PRIx32 ")\n",
            reason, header ? type_name(header_at(header)->type_info) : "none", return_address);
    compat_runtime32_trap_import("std::terminate", arguments, return_address);
}

/* Phase 2 from start: the first frame with a cleanup or the handler found in
   phase 1 gets control. */
static bool install_next_pad(uint32_t address, struct guest_resume_context frame)
{
    struct guest_exception *header = header_at(address);
    for (unsigned depth = 0; depth < 4096; ++depth) {
        struct frame_info info;
        if (!find_frame_info(frame.eip - 1, &info)) return false;
        if (info.lsda) {
            uint32_t pad = 0; int32_t selector = 0;
            int action = scan_lsda(&info, frame.eip, header->type_info, &pad, &selector);
            bool handler_frame = frame.esp == header->handler_esp;
            if (handler_frame && action == kHandler) {
                selector = header->handler_selector;
            } else if (action == kHandler) {
                action = kNoAction;
            }
            if (action != kNoAction) {
                frame.eip = pad;
                frame.eax = address + kUnwindOffset;
                frame.edx = (uint32_t)selector;
                compat_runtime32_resume_guest(&frame);
                return true;
            }
            if (handler_frame) return false;
        }
        if (!step_frame(&frame, &info)) return false;
    }
    return false;
}

static void raise_exception(uint32_t address, const uint32_t *arguments, uint32_t return_address)
{
    struct guest_resume_context start;
    if (!compat_runtime32_import_frame(&start)) {
        terminate("throw outside a guest import frame", address, arguments, return_address);
        return;
    }
    struct guest_exception *header = header_at(address);
    struct guest_resume_context frame = start;
    bool found = false;
    for (unsigned depth = 0; depth < 4096 && !found; ++depth) {
        struct frame_info info;
        if (!find_frame_info(frame.eip - 1, &info)) break;
        if (info.lsda) {
            uint32_t pad = 0; int32_t selector = 0;
            if (scan_lsda(&info, frame.eip, header->type_info, &pad, &selector) == kHandler) {
                header->handler_esp = frame.esp;
                header->handler_selector = selector;
                found = true;
                break;
            }
        }
        if (!step_frame(&frame, &info)) break;
    }
    if (!found) { terminate("uncaught exception", address, arguments, return_address); return; }
    ++uncaught_exceptions;
    if (!install_next_pad(address, start))
        terminate("unwinding failed", address, arguments, return_address);
}

static uint32_t allocate_primary(uint32_t size)
{
    uint32_t block = compat_runtime32_allocate(kHeaderSize + size, 1);
    if (!block) return 0;
    struct guest_exception *header = header_at(block);
    header->magic = kMagic;
    header->object = block + kHeaderSize;
    header->references = 1;
    return header->object;
}

static void throw_object(uint32_t object, uint32_t type_info, uint32_t destructor,
                         const uint32_t *arguments, uint32_t return_address)
{
    struct guest_exception *header = primary_of(object);
    header->type_info = type_info;
    header->destructor = destructor;
    header->handler_count = 0;
    header->unwind = 0;
    raise_exception(object - kHeaderSize, arguments, return_address);
}

void cxx_exception_bridge32_rethrow(uint32_t object, const uint32_t *arguments,
                                    uint32_t return_address)
{
    uint32_t block = compat_runtime32_allocate(kHeaderSize, 1);
    if (!object || !block) { terminate("rethrow of an empty exception_ptr", 0, arguments, return_address); return; }
    struct guest_exception *header = header_at(block);
    *header = (struct guest_exception){.magic = kMagic, .object = object,
        .type_info = primary_of(object)->type_info, .dependent = 1};
    cxx_exception_bridge32_retain(object);
    raise_exception(block, arguments, return_address);
}

static uint32_t std_vtable(const char *symbol)
{
    uint32_t cell = compat_runtime32_resolve_symbol(symbol, 1);
    return cell ? cell + 8 : 0;
}

/* Fragile Objective-C ABI: LocalData_t is a jmp_buf (72 bytes) followed by
   pointers[0] = exception, pointers[1] = next handler. The guest's _setjmp
   is lp32_context_fast_setjmp: ebx, ebp, esi, edi, esp, eip at 0..20. */
static _Thread_local uint32_t objc_handlers;

static int dispatch_objc(const char *name, const uint32_t *a, uint32_t return_address, uint64_t *result)
{
    if (!strcmp(name, "_objc_exception_try_enter")) {
        write32(a[0] + 76, objc_handlers);
        objc_handlers = a[0];
        return 1;
    }
    if (!strcmp(name, "_objc_exception_try_exit")) {
        if (objc_handlers == a[0]) objc_handlers = read32(a[0] + 76);
        return 1;
    }
    if (!strcmp(name, "_objc_exception_extract")) { *result = read32(a[0] + 72); return 1; }
    if (!strcmp(name, "_objc_exception_match")) {
        *result = objc_bridge32_guest_is_kind_of_class(a[1], a[0]);
        return 1;
    }
    if (!strcmp(name, "_objc_exception_throw")) {
        uint32_t handler = objc_handlers;
        if (!handler) {
            fprintf(stderr, "compat32: uncaught Objective-C exception %s\n",
                    objc_bridge32_guest_class_name(a[0]));
            compat_runtime32_trap_import(name, a, return_address);
            return 1;
        }
        objc_handlers = read32(handler + 76);
        write32(handler + 72, a[0]);
        struct guest_resume_context target = {
            .ebx = read32(handler), .ebp = read32(handler + 4), .esi = read32(handler + 8),
            .edi = read32(handler + 12), .esp = read32(handler + 16), .eip = read32(handler + 20),
            .eax = 1,
        };
        compat_runtime32_resume_guest(&target);
        return 1;
    }
    return 0;
}

int cxx_exception_bridge32_dispatch(const char *name, const uint32_t *a,
                                    uint32_t return_address, uint64_t *result)
{
    *result = 0;
    if (!strncmp(name, "_objc_exception_", 16)) return dispatch_objc(name, a, return_address, result);
    if (!strcmp(name, "___cxa_allocate_exception")) {
        *result = allocate_primary(a[0]);
        if (!*result) terminate("out of memory allocating an exception", 0, a, return_address);
        return 1;
    }
    if (!strcmp(name, "___cxa_free_exception")) {
        if (a[0]) compat_runtime32_deallocate(a[0] - kHeaderSize);
        return 1;
    }
    if (!strcmp(name, "___cxa_throw")) {
        throw_object(a[0], a[1], a[2], a, return_address);
        return 1;
    }
    if (!strcmp(name, "___cxa_begin_catch")) {
        if (!a[0]) return 1;
        uint32_t address = a[0] - kUnwindOffset;
        struct guest_exception *header = header_at(address);
        if (header->magic != kMagic) { terminate("foreign exception", 0, a, return_address); return 1; }
        header->handler_count = header->handler_count < 0 ? -header->handler_count + 1 :
                                                            header->handler_count + 1;
        if (caught_exceptions != address) {
            header->next_caught = caught_exceptions;
            caught_exceptions = address;
        }
        --uncaught_exceptions;
        *result = header->object;
        return 1;
    }
    if (!strcmp(name, "___cxa_end_catch")) {
        uint32_t address = caught_exceptions;
        if (!address) return 1;
        struct guest_exception *header = header_at(address);
        if (header->handler_count < 0) {
            if (++header->handler_count == 0) caught_exceptions = header->next_caught;
        } else if (--header->handler_count == 0) {
            caught_exceptions = header->next_caught;
            release_header(address);
        }
        return 1;
    }
    if (!strcmp(name, "___cxa_rethrow")) {
        uint32_t address = caught_exceptions;
        if (!address) { terminate("rethrow with no active exception", 0, a, return_address); return 1; }
        struct guest_exception *header = header_at(address);
        header->handler_count = -header->handler_count;
        raise_exception(address, a, return_address);
        return 1;
    }
    if (!strcmp(name, "__Unwind_Resume")) {
        struct guest_resume_context frame;
        uint32_t address = a[0] - kUnwindOffset;
        if (!a[0] || header_at(address)->magic != kMagic || !compat_runtime32_import_frame(&frame) ||
            !install_next_pad(address, frame))
            terminate("_Unwind_Resume failed", a[0] ? address : 0, a, return_address);
        return 1;
    }
    if (!strcmp(name, "__ZSt9terminatev")) { terminate("called by the game", caught_exceptions, a, return_address); return 1; }
    if (!strcmp(name, "__ZSt17current_exceptionv")) {
        /* exception_ptr comes back through the hidden sret pointer. */
        uint32_t object = caught_exceptions ? header_at(caught_exceptions)->object : 0;
        cxx_exception_bridge32_retain(object);
        write32(a[0], object);
        *result = a[0];
        return 1;
    }
    if (!strcmp(name, "__ZSt17rethrow_exceptionSt13exception_ptr")) {
        /* A non-trivial class argument is passed by address. */
        cxx_exception_bridge32_rethrow(read32(a[0]), a, return_address);
        return 1;
    }
    if (!strcmp(name, "__ZNSt13exception_ptrC1ERKS_") || !strcmp(name, "__ZNSt13exception_ptrC2ERKS_")) {
        uint32_t object = read32(a[1]);
        cxx_exception_bridge32_retain(object);
        write32(a[0], object);
        return 1;
    }
    if (!strcmp(name, "__ZNSt13exception_ptrD1Ev") || !strcmp(name, "__ZNSt13exception_ptrD2Ev")) {
        cxx_exception_bridge32_release(read32(a[0]));
        write32(a[0], 0);
        return 1;
    }
    if (!strcmp(name, "___cxa_uncaught_exception")) { *result = uncaught_exceptions > 0; return 1; }
    /* Standard exception objects the guest constructs before throwing. Their
       vtables are placeholder cells; catch(...) never calls through them. */
    if (!strcmp(name, "__ZNSt9bad_allocC1Ev") || !strcmp(name, "__ZNSt9bad_allocC2Ev")) {
        write32(a[0], std_vtable("__ZTVSt9bad_alloc"));
        return 1;
    }
    if (!strcmp(name, "__ZNSt11logic_errorC1ERKS_") || !strcmp(name, "__ZNSt11logic_errorC2ERKS_")) {
        write32(a[0], read32(a[1]));
        write32(a[0] + 4, read32(a[1] + 4));
        return 1;
    }
    if (!strcmp(name, "__ZNSt3__112future_errorC1ENS_10error_codeE") ||
        !strcmp(name, "__ZNSt3__112future_errorC2ENS_10error_codeE")) {
        static uint32_t message;
        if (!message) message = compat_runtime32_copy_cstring("future_error");
        write32(a[0], std_vtable("__ZTVNSt3__112future_errorE"));
        write32(a[0] + 4, message);
        write32(a[0] + 8, a[1]);
        write32(a[0] + 12, a[2]);
        return 1;
    }
    if (!strcmp(name, "__ZNSt9bad_allocD1Ev") || !strcmp(name, "__ZNSt9bad_allocD2Ev") ||
        !strcmp(name, "__ZNSt3__112future_errorD1Ev") || !strcmp(name, "__ZNSt3__112future_errorD2Ev"))
        return 1;
    if (!strcmp(name, "__ZNSt3__115future_categoryEv")) {
        static uint32_t category;
        if (!category) {
            category = compat_runtime32_allocate(16, 1);
            if (category) write32(category, std_vtable("__ZTVNSt3__117__future_error_categoryE"));
        }
        *result = category;
        return 1;
    }
    if (!strcmp(name, "__ZNSt3__120__throw_system_errorEiPKc")) {
        fprintf(stderr, "compat32: std::system_error(%d, \"%s\")\n", (int)a[0],
                a[1] ? (const char *)(uintptr_t)a[1] : "");
        uint32_t object = allocate_primary(16);
        if (!object) { terminate("out of memory allocating an exception", 0, a, return_address); return 1; }
        write32(object, std_vtable("__ZTVNSt3__112system_errorE"));
        write32(object + 4, a[1]);
        write32(object + 8, a[0]);
        throw_object(object, compat_runtime32_resolve_symbol("__ZTINSt3__112system_errorE", 1), 0,
                     a, return_address);
        return 1;
    }
    return 0;
}

/* Real i386 code, assembled from tests/guest_exception_test.s with
   `clang -target i386-apple-macosx10.7 -c`: A catches (...) around M; M runs a cleanup and _Unwind_Resume
   around B; B throws a 4-byte object holding 0x1234. O is @try { @throw }.
   0xc0de00NN immediates are patched with thunk and data addresses. */
static const uint8_t test_code[] = {
        0x55, 0x89, 0xe5, 0x53, 0x56, 0x57, 0x83, 0xec, 0x1c, 0xbb, 0x11, 0x11, 0x11, 0x11, 0xbe, 0x22,
        0x22, 0x22, 0x22, 0xbf, 0x33, 0x33, 0x33, 0x33, 0xe8, 0x4b, 0x00, 0x00, 0x00, 0x31, 0xc0, 0xeb,
        0x3f, 0x81, 0xfb, 0x11, 0x11, 0x11, 0x11, 0x75, 0x32, 0x81, 0xfe, 0x22, 0x22, 0x22, 0x22, 0x75,
        0x2a, 0x81, 0xff, 0x33, 0x33, 0x33, 0x33, 0x75, 0x22, 0x83, 0xfa, 0x01, 0x75, 0x1d, 0x89, 0x04,
        0x24, 0xb8, 0x01, 0x00, 0xde, 0xc0, 0xff, 0xd0, 0x8b, 0x00, 0x89, 0x44, 0x24, 0x10, 0xb8, 0x02,
        0x00, 0xde, 0xc0, 0xff, 0xd0, 0x8b, 0x44, 0x24, 0x10, 0xeb, 0x05, 0xb8, 0xff, 0xff, 0xff, 0xff,
        0x83, 0xc4, 0x1c, 0x5f, 0x5e, 0x5b, 0x5d, 0xc3, 0x55, 0x89, 0xe5, 0x56, 0x83, 0xec, 0x14, 0xbe,
        0x66, 0x66, 0x66, 0x66, 0xe8, 0x19, 0x00, 0x00, 0x00, 0x83, 0xc4, 0x14, 0x5e, 0x5d, 0xc3, 0x89,
        0x04, 0x24, 0xb9, 0x07, 0x00, 0xde, 0xc0, 0xff, 0x01, 0xb9, 0x03, 0x00, 0xde, 0xc0, 0xff, 0xd1,
        0x0f, 0x0b, 0x55, 0x89, 0xe5, 0x53, 0x57, 0x83, 0xec, 0x10, 0xbb, 0x44, 0x44, 0x44, 0x44, 0xbf,
        0x55, 0x55, 0x55, 0x55, 0xc7, 0x04, 0x24, 0x04, 0x00, 0x00, 0x00, 0xb8, 0x04, 0x00, 0xde, 0xc0,
        0xff, 0xd0, 0xc7, 0x00, 0x34, 0x12, 0x00, 0x00, 0x89, 0x04, 0x24, 0xc7, 0x44, 0x24, 0x04, 0x08,
        0x00, 0xde, 0xc0, 0xc7, 0x44, 0x24, 0x08, 0x00, 0x00, 0x00, 0x00, 0xb8, 0x05, 0x00, 0xde, 0xc0,
        0xff, 0xd0, 0x0f, 0x0b, 0x55, 0x89, 0xe5, 0x53, 0x83, 0xec, 0x74, 0x8d, 0x5c, 0x24, 0x10, 0x89,
        0x1c, 0x24, 0xb8, 0x09, 0x00, 0xde, 0xc0, 0xff, 0xd0, 0x89, 0x1c, 0x24, 0xb8, 0x0a, 0x00, 0xde,
        0xc0, 0xff, 0xd0, 0x85, 0xc0, 0x75, 0x10, 0xc7, 0x04, 0x24, 0x77, 0x07, 0x00, 0x00, 0xb8, 0x0b,
        0x00, 0xde, 0xc0, 0xff, 0xd0, 0x0f, 0x0b, 0x89, 0x1c, 0x24, 0xb8, 0x0c, 0x00, 0xde, 0xc0, 0xff,
        0xd0, 0x83, 0xc4, 0x74, 0x5b, 0x5d, 0xc3,
};
enum {
    kTestA = 0x00, kTestCsA = 0x18, kTestLpA = 0x21, kTestM = 0x68, kTestCsM = 0x74,
    kTestLpM = 0x7f, kTestCsR = 0x8e, kTestB = 0x92, kTestO = 0xd4, kTestEnd = 0x117,
    kTestUnwind = 0x400, kTestLsdaA = 0x600, kTestLsdaM = 0x640, kTestGot = 0x680,
    kTestCounter = 0x684, kTestTypeInfo = 0x688, kTestTypeName = 0x6a0,
};

int cxx_exception_bridge32_run_self_test(void)
{
    uint8_t *code = mmap((void *)0x76100000, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (code == MAP_FAILED) return -1;
    uint32_t base = (uint32_t)(uintptr_t)code;
    memcpy(code, test_code, sizeof(test_code));
    const uint32_t patches[][2] = {
        {1, compat_runtime32_guest_callback("___cxa_begin_catch")},
        {2, compat_runtime32_guest_callback("___cxa_end_catch")},
        {3, compat_runtime32_guest_callback("__Unwind_Resume")},
        {4, compat_runtime32_guest_callback("___cxa_allocate_exception")},
        {5, compat_runtime32_guest_callback("___cxa_throw")},
        {7, base + kTestCounter}, {8, base + kTestTypeInfo},
        {9, compat_runtime32_guest_callback("_objc_exception_try_enter")},
        {10, compat_runtime32_guest_callback("__setjmp")},
        {11, compat_runtime32_guest_callback("_objc_exception_throw")},
        {12, compat_runtime32_guest_callback("_objc_exception_extract")},
    };
    for (size_t i = 0; i + 4 <= sizeof(test_code); ++i) {
        uint32_t word;
        memcpy(&word, code + i, 4);
        if ((word >> 8) != 0xc0de00) continue;
        for (size_t p = 0; p < sizeof(patches) / sizeof(patches[0]); ++p)
            if ((word & 0xff) == patches[p][0]) { memcpy(code + i, &patches[p][1], 4); i += 3; break; }
    }
    write32(base + kTestTypeInfo + 4, base + kTestTypeName);
    memcpy(code + kTestTypeName, "i", 2);

    /* __unwind_info: one regular second-level page, two LSDAs. */
    const uint32_t header[] = {1, 28, 0, 28, 0, 28, 2,
        kTestA, 68, 52, kTestEnd, 0, 68,
        kTestA, kTestLsdaA, kTestM, kTestLsdaM};
    memcpy(code + kTestUnwind, header, sizeof(header));
    const uint32_t page[] = {2, 8 | (4u << 16),
        kTestA, 0x4103006c, kTestM, 0x41010005, kTestB, 0x0102000c, kTestO, 0x01010001};
    memcpy(code + kTestUnwind + 68, page, sizeof(page));
    uint8_t *lsda = code + kTestLsdaA;
    const uint8_t lsda_a[] = {0xff, 0x9b, 21, 0x03, 13,
        kTestCsA - kTestA, 0, 0, 0, 5, 0, 0, 0, kTestLpA - kTestA, 0, 0, 0, 1, 0x01, 0x00};
    memcpy(lsda, lsda_a, sizeof(lsda_a));
    write32(base + kTestLsdaA + 20, (base + kTestGot) - (base + kTestLsdaA + 20));
    const uint8_t lsda_m[] = {0xff, 0xff, 0x03, 26,
        kTestCsM - kTestM, 0, 0, 0, 5, 0, 0, 0, kTestLpM - kTestM, 0, 0, 0, 0,
        kTestCsR - kTestM, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0};
    memcpy(code + kTestLsdaM, lsda_m, sizeof(lsda_m));
    cxx_exception_bridge32_register_image(base, base + 0x400, base + kTestUnwind,
                                          base + kTestUnwind + 68 + sizeof(page));

    /* LP32_EXCEPTION_LOOKUP=0xpc,...: print the game image's unwind entries. */
    const char *lookups = getenv("LP32_EXCEPTION_LOOKUP");
    for (const char *p = lookups; p && *p;) {
        char *next;
        uint32_t pc = (uint32_t)strtoul(p, &next, 0);
        struct frame_info info = {0};
        bool found = find_frame_info(pc, &info);
        fprintf(stderr, "lookup %#x found=%d start=%#x end=%#x encoding=%#x lsda=%#x\n",
                pc, found, info.start, info.end, info.encoding, info.lsda);
        p = *next == ',' ? next + 1 : "";
    }
    int failures = 0;
    for (int round = 0; round < 3; ++round) {
        write32(base + kTestCounter, 0);
        uint32_t result = compat_runtime32_call(base + kTestA, NULL, 0);
        if (compat_runtime32_last_call_trapped() || result != 0x1234 ||
            read32(base + kTestCounter) != 1 || caught_exceptions || uncaught_exceptions) {
            fprintf(stderr, "C++ exception round %d: result=%#x cleanups=%u caught=%#x uncaught=%d\n",
                    round, result, read32(base + kTestCounter), caught_exceptions, uncaught_exceptions);
            ++failures;
        }
        result = compat_runtime32_call(base + kTestO, NULL, 0);
        if (compat_runtime32_last_call_trapped() || result != 0x777 || objc_handlers) {
            fprintf(stderr, "Objective-C exception round %d: result=%#x handlers=%#x\n",
                    round, result, objc_handlers);
            ++failures;
        }
    }
    munmap(code, 4096);
    if (failures) return -1;
    fprintf(stderr, "Guest exception PASS (throw through cleanup to catch(...), @try/@throw)\n");
    return 0;
}
