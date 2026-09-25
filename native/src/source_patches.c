#include "source_patches.h"
#include "compat_runtime.h"
#include "crash_trace.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * Portal's current Mac vguimatsurface (UUID below) was rebuilt with only the
 * Linux branch of CFontManager::CreateOrFindWin32Font: on OSX it appends a
 * NULL slot and returns NULL, so no TrueType font is ever created and the next
 * lookup dereferences that slot.
 *
 * The binary still contains the ATSUI COSXFont, but it implements the older
 * 12-slot font interface while CFontManager, CFontAmalgam and
 * CFontTextureCache call the IVGuiFontImpl layout below.  The replacement
 * creates a COSXFont and hands out a small guest adapter whose vtable forwards
 * each IVGuiFontImpl slot to the matching COSXFont slot.
 */
static const unsigned char vguimatsurface_uuid[16] = {
    0x6d,0x28,0x97,0x0c,0xa3,0x20,0x3a,0xa6,0xbe,0x35,0xab,0x4e,0x8f,0x3c,0x3a,0xcc};
enum {
    kCreateOrFindWin32Font = 0xb1270,
    kFontVectorGrow = 0xb1a90,       /* CUtlMemory<IVGuiFontImpl *, int>::Grow(int) */
    kOSXFontConstructor = 0xb1c80,   /* COSXFont::COSXFont() */
    kOSXFontDestructor = 0xb1e00,    /* COSXFont::~COSXFont() (non-deleting) */
    kOSXFontCreate = 0xb2030,        /* COSXFont::Create(name, tall, weight, blur, scanlines, flags) */
    kOSXFontAllocation = 0xac,       /* sizeof(CBitmapFont), a COSXFont subclass */
};

/* COSXFont vtable offsets. */
enum {
    kOSXCreate = 0x00, kOSXGetCharRGBA = 0x04, kOSXIsEqualTo = 0x08, kOSXIsValid = 0x0c,
    kOSXGetCharABCWidths = 0x10, kOSXGetHeight = 0x14,
};

/* IVGuiFontImpl vtable offsets, recovered from this binary's call sites. */
enum {
    kFontDestructor = 0x00, kFontDeletingDestructor = 0x04, kFontCreate = 0x08,
    kFontGetCharRGBA = 0x0c, kFontIsEqualTo = 0x10, kFontIsValid = 0x14,
    kFontGetCharABCWidths = 0x18, kFontGetHeight = 0x20, kFontGetUnderlined = 0x34,
    kFontGetName = 0x38, kFontGetFamilyName = 0x3c, kFontGetKernedCharWidth = 0x44,
    kFontHasValidFace = 0x48,
    kFontSlots = 0x4c / 4,
};

static const unsigned char prologue[6] = {0x55,0x89,0xe5,0x53,0x57,0x56};
/* CFontManager keeps CUtlVector<IVGuiFontImpl *> m_Win32Fonts at +0x14. */
struct guest_font_vector { uint32_t memory; int32_t allocated, grow, size; uint32_t elements; };
struct guest_font_adapter { uint32_t vtable, osx_font, name; };

static uint32_t vgui_slide;
static uint32_t adapter_vtable;

static uint32_t osx_slot(uint32_t font, uint32_t offset)
{
    uint32_t vtable = *(uint32_t *)(uintptr_t)font;
    return *(uint32_t *)(uintptr_t)(vtable + offset);
}

static uint64_t forward(const uint32_t *args, uint32_t offset, unsigned count)
{
    const struct guest_font_adapter *font = (const void *)(uintptr_t)args[0];
    uint32_t call[8] = {font->osx_font};
    memcpy(call + 1, args + 1, (count - 1) * sizeof(uint32_t));
    return compat_runtime32_call(osx_slot(font->osx_font, offset), call, count);
}

static void destroy_osx_font(uint32_t font)
{
    compat_runtime32_call(vgui_slide + kOSXFontDestructor, &font, 1);
    compat_runtime32_deallocate(font);
}

static uint64_t font_slot(unsigned offset, const uint32_t *args)
{
    struct guest_font_adapter *font = (void *)(uintptr_t)args[0];
    switch (offset) {
    case kFontDestructor:
    case kFontDeletingDestructor:
        destroy_osx_font(font->osx_font);
        compat_runtime32_deallocate(font->name);
        font->osx_font = font->name = 0;
        if (offset == kFontDeletingDestructor) compat_runtime32_deallocate(args[0]);
        return 0;
    case kFontCreate: return forward(args, kOSXCreate, 7);
    case kFontGetCharRGBA: return forward(args, kOSXGetCharRGBA, 5);
    case kFontIsEqualTo: return forward(args, kOSXIsEqualTo, 7);
    case kFontIsValid:
    case kFontHasValidFace: return forward(args, kOSXIsValid, 1);
    case kFontGetCharABCWidths: return forward(args, kOSXGetCharABCWidths, 5);
    case kFontGetName:
    case kFontGetFamilyName: return font->name;
    case kFontGetKernedCharWidth: {
        /* ABC outputs must live in guest memory; one cell set per thread. */
        static _Thread_local uint32_t abc;
        if (!abc) abc = compat_runtime32_allocate(3 * sizeof(int32_t), 1);
        if (!abc) return 0;
        uint32_t call[6] = {font->osx_font, args[1], abc, abc + 4, abc + 8, 0};
        compat_runtime32_call(osx_slot(font->osx_font, kOSXGetCharABCWidths), call, 6);
        const int32_t *widths = (const int32_t *)(uintptr_t)abc;
        float values[3] = {(float)(widths[0] + widths[1] + widths[2]),
                           (float)widths[0], (float)widths[2]};
        for (unsigned i = 0; i < 3; ++i)
            if (args[4 + i]) memcpy((void *)(uintptr_t)args[4 + i], &values[i], sizeof(float));
        return 0;
    }
    default:
        /* GetHeight through GetUnderlined keep COSXFont's relative order. */
        if (offset >= kFontGetHeight && offset <= kFontGetUnderlined)
            return forward(args, offset - kFontGetHeight + kOSXGetHeight, 1);
        {
            static _Atomic uint32_t reported;
            if (!(atomic_fetch_or(&reported, 1u << (offset / 4)) & (1u << (offset / 4))))
                fprintf(stderr, "compat32: unimplemented IVGuiFontImpl slot 0x%x\n", offset);
        }
        return 0;
    }
}

#define FONT_SLOT(n) \
    static uint64_t font_slot_##n(const uint32_t *args, uint32_t caller) \
    { (void)caller; return font_slot(n * 4, args); }
FONT_SLOT(0) FONT_SLOT(1) FONT_SLOT(2) FONT_SLOT(3) FONT_SLOT(4) FONT_SLOT(5)
FONT_SLOT(6) FONT_SLOT(7) FONT_SLOT(8) FONT_SLOT(9) FONT_SLOT(10) FONT_SLOT(11)
FONT_SLOT(12) FONT_SLOT(13) FONT_SLOT(14) FONT_SLOT(15) FONT_SLOT(16) FONT_SLOT(17)
FONT_SLOT(18)
#undef FONT_SLOT

static const lp32_fast_import_fn font_slot_handlers[kFontSlots] = {
    font_slot_0, font_slot_1, font_slot_2, font_slot_3, font_slot_4, font_slot_5,
    font_slot_6, font_slot_7, font_slot_8, font_slot_9, font_slot_10, font_slot_11,
    font_slot_12, font_slot_13, font_slot_14, font_slot_15, font_slot_16, font_slot_17,
    font_slot_18,
};
static const char font_slot_prefix[] = "_lp32_vgui_font_slot_";

static uint32_t build_adapter_vtable(void)
{
    uint32_t vtable = compat_runtime32_allocate(kFontSlots * sizeof(uint32_t), 1);
    if (!vtable) return 0;
    for (unsigned i = 0; i < kFontSlots; ++i) {
        char name[sizeof(font_slot_prefix) + 4];
        snprintf(name, sizeof(name), "%s%u", font_slot_prefix, i);
        uint32_t thunk = compat_runtime32_guest_callback(name);
        if (!thunk) return 0;
        ((uint32_t *)(uintptr_t)vtable)[i] = thunk;
    }
    return vtable;
}

static uint64_t create_or_find_win32_font(const uint32_t *args, uint32_t caller)
{
    (void)caller;
    struct guest_font_vector *fonts = (void *)(uintptr_t)(args[0] + 0x14);
    for (int32_t i = 0; i < fonts->size; ++i) {
        uint32_t font = ((uint32_t *)(uintptr_t)fonts->memory)[i];
        /* Skip CBitmapFonts, which also use the old interface. */
        if (!font || *(uint32_t *)(uintptr_t)font != adapter_vtable) continue;
        uint32_t call[7] = {font, args[1], args[2], args[3], args[4], args[5], args[6]};
        if (font_slot(kFontIsEqualTo, call) & 0xff) return font;
    }

    uint32_t osx_font = compat_runtime32_allocate(kOSXFontAllocation, 1);
    uint32_t adapter = compat_runtime32_allocate(sizeof(struct guest_font_adapter), 1);
    uint32_t name = compat_runtime32_copy_cstring(args[1] ? (const char *)(uintptr_t)args[1] : "");
    if (!osx_font || !adapter || !name) {
        compat_runtime32_deallocate(osx_font);
        compat_runtime32_deallocate(adapter);
        compat_runtime32_deallocate(name);
        return 0;
    }
    compat_runtime32_call(vgui_slide + kOSXFontConstructor, &osx_font, 1);
    uint32_t call[7] = {osx_font, args[1], args[2], args[3], args[4], args[5], args[6]};
    if (!(compat_runtime32_call(vgui_slide + kOSXFontCreate, call, 7) & 0xff)) {
        if (getenv("LP32_TRACE_FONT"))
            fprintf(stderr, "compat32: COSXFont::Create failed for \"%s\" tall=%d\n",
                    (const char *)(uintptr_t)name, (int32_t)args[2]);
        destroy_osx_font(osx_font);
        compat_runtime32_deallocate(adapter);
        compat_runtime32_deallocate(name);
        return 0;
    }
    *(struct guest_font_adapter *)(uintptr_t)adapter =
        (struct guest_font_adapter){adapter_vtable, osx_font, name};

    if (fonts->size >= fonts->allocated) {
        uint32_t grow[2] = {args[0] + 0x14, (uint32_t)(fonts->size + 1 - fonts->allocated)};
        compat_runtime32_call(vgui_slide + kFontVectorGrow, grow, 2);
        if (fonts->size >= fonts->allocated) return adapter;
    }
    ((uint32_t *)(uintptr_t)fonts->memory)[fonts->size++] = adapter;
    fonts->elements = fonts->memory;
    return adapter;
}

lp32_fast_import_fn lp32_source_patch_handler(const char *name)
{
    if (!strcmp(name, "_lp32_vgui_create_or_find_win32_font")) return create_or_find_win32_font;
    size_t prefix = sizeof(font_slot_prefix) - 1;
    if (!strncmp(name, font_slot_prefix, prefix)) {
        char *end;
        unsigned long slot = strtoul(name + prefix, &end, 10);
        if (end != name + prefix && !*end && slot < kFontSlots) return font_slot_handlers[slot];
    }
    return NULL;
}

static int redirect(uint32_t address, uint32_t target)
{
    unsigned page_size = (unsigned)getpagesize();
    uint32_t page = address & ~(page_size - 1);
    if (mprotect((void *)(uintptr_t)page, page_size * 2, PROT_READ | PROT_WRITE | PROT_EXEC)) return -1;
    unsigned char code[5] = {0xe9};
    uint32_t relative = target - address - 5;
    memcpy(code + 1, &relative, 4);
    memcpy((void *)(uintptr_t)address, code, sizeof(code));
    __builtin___clear_cache((char *)(uintptr_t)address, (char *)(uintptr_t)(address + sizeof(code)));
    return mprotect((void *)(uintptr_t)page, page_size * 2, PROT_READ | PROT_EXEC);
}

void lp32_source_patches_install(const char *path, uint32_t slide, const unsigned char uuid[16])
{
    const char *name = strrchr(path, '/');
    if (strcmp(name ? name + 1 : path, "vguimatsurface.dylib") || !uuid ||
        memcmp(uuid, vguimatsurface_uuid, 16)) return;
    const uint32_t checked[] = {kCreateOrFindWin32Font, kOSXFontCreate};
    for (unsigned i = 0; i < 2; ++i) {
        unsigned char bytes[sizeof(prologue)];
        if (!lp32_crash_read(slide + checked[i], bytes, sizeof(bytes)) ||
            memcmp(bytes, prologue, sizeof(prologue))) {
            fprintf(stderr, "compat32: vguimatsurface font signature mismatch; fonts left unpatched\n");
            return;
        }
    }
    vgui_slide = slide;
    adapter_vtable = build_adapter_vtable();
    uint32_t target = compat_runtime32_guest_callback("_lp32_vgui_create_or_find_win32_font");
    if (!adapter_vtable || !target || redirect(slide + kCreateOrFindWin32Font, target)) {
        fprintf(stderr, "compat32: could not install the vguimatsurface font fix\n");
        return;
    }
    fprintf(stderr, "compat32: restored COSXFont creation in vguimatsurface\n");
}
