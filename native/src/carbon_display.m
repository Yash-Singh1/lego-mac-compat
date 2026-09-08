#import <Cocoa/Cocoa.h>
#include "carbon_display.h"
#include "compat_runtime.h"
#include "objc_bridge.h"
#include "carbon_native.h"
#include <pthread.h>

/* QuickDraw's public GDevice/PixMap records are dereferenced by the i386
   guest, so opaque host pointers are not sufficient for these APIs. */
struct __attribute__((packed, aligned(2))) device32 {
    int16_t reference, id, type; uint32_t inverse_table; int16_t resolution;
    uint32_t search, complement; uint16_t flags; uint32_t pixmap, reference_value, next;
    int16_t bounds[4]; int32_t mode; int16_t cursor_bytes, cursor_depth;
    uint32_t cursor_data, cursor_mask, extension;
};
struct __attribute__((packed, aligned(2))) pixmap32 {
    uint32_t base; uint16_t row_bytes; int16_t bounds[4], version, pack_type;
    uint32_t pack_size, horizontal_resolution, vertical_resolution;
    int16_t pixel_type, pixel_size, components, component_size;
    uint32_t pixel_format, table, extension;
};
_Static_assert(sizeof(struct device32) == 62, "i386 GDevice");
_Static_assert(sizeof(struct pixmap32) == 50, "i386 PixMap");
static struct { CGDirectDisplayID id; uint32_t handle; } displays[32];
static uint32_t display_count, current_device;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

CGRect carbon_display32_port_bounds(void *value) {
    NSView *view = value;
    if (![view isKindOfClass:[NSView class]]) return CGRectZero;
    CGRect bounds = [view bounds];
    int32_t size[2];
    /* A composited fullscreen port uses the guest's chosen render size.
       Its Cocoa view fits the physical desktop, which can be smaller.
       TFU derives intro/loading viewports from these QuickDraw bounds. */
    if (view == [[view window] contentView] &&
        carbon_native32_surface_size([view window], size))
        bounds.size = CGSizeMake(size[0], size[1]);
    return bounds;
}

/* TFU only queries bounds of these regions. Preserve the public rectangular
   QuickDraw Region layout, including its guest master pointer. */
struct __attribute__((packed, aligned(2))) region32 { int16_t size, bounds[4]; };
static uint32_t desktop_region;
static uint32_t create_region(void) {
    uint32_t memory = compat_runtime32_allocate(4 + sizeof(struct region32), 1);
    if (memory) {
        *(uint32_t *)(uintptr_t)memory = memory + 4;
        ((struct region32 *)(uintptr_t)(memory + 4))->size = sizeof(struct region32);
    }
    return memory;
}
static void region_bounds(uint32_t region, CGRect bounds) {
    if (!region) return;
    struct region32 *p = (void *)(uintptr_t)*(uint32_t *)(uintptr_t)region;
    p->bounds[0] = MAX(INT16_MIN, MIN(INT16_MAX, CGRectGetMinY(bounds)));
    p->bounds[1] = MAX(INT16_MIN, MIN(INT16_MAX, CGRectGetMinX(bounds)));
    p->bounds[2] = MAX(INT16_MIN, MIN(INT16_MAX, CGRectGetMaxY(bounds)));
    p->bounds[3] = MAX(INT16_MIN, MIN(INT16_MAX, CGRectGetMaxX(bounds)));
}
static int region_dispatch(const char *name, const uint32_t *a, uint64_t *result) {
    if (!strcmp(name, "_NewRgn")) { *result = create_region(); return 1; }
    if (!strcmp(name, "_DisposeRgn")) { if (a[0] != desktop_region) compat_runtime32_deallocate(a[0]); *result = 0; return 1; }
    if (!strcmp(name, "_GetRegionBounds")) {
        if (a[0] && a[1]) {
            struct region32 *p = (void *)(uintptr_t)*(uint32_t *)(uintptr_t)a[0];
            memcpy((void *)(uintptr_t)a[1], p->bounds, 8);
        }
        *result = a[1]; return 1;
    }
    if (!strcmp(name, "_GetPortVisibleRegion")) {
        id port = objc_bridge32_host_object(a[0]);
        if ([port isKindOfClass:[NSView class]]) region_bounds(a[1], carbon_display32_port_bounds(port));
        *result = a[1]; return 1;
    }
    if (strcmp(name, "_DMGetDeskRegion") && strcmp(name, "_GetGrayRgn")) return 0;
    pthread_mutex_lock(&lock);
    if (!desktop_region) desktop_region = create_region();
    CGDirectDisplayID ids[32]; uint32_t count = 0; CGRect bounds = CGRectNull;
    if (!CGGetActiveDisplayList(32, ids, &count))
        for (unsigned i = 0; i < count; ++i) bounds = CGRectUnion(bounds, CGDisplayBounds(ids[i]));
    if (CGRectIsNull(bounds)) bounds = CGRectZero;
    region_bounds(desktop_region, bounds);
    *result = desktop_region;
    if (!strcmp(name, "_DMGetDeskRegion")) {
        if (a[0]) *(uint32_t *)(uintptr_t)a[0] = desktop_region;
        *result = !a[0] ? (uint32_t)-50 : !desktop_region ? (uint32_t)-108 : 0;
    }
    pthread_mutex_unlock(&lock); return 1;
}

static void initialize_displays(void) {
    if (display_count) return;
    CGDirectDisplayID ids[32]; uint32_t count = 0;
    if (CGGetActiveDisplayList(32, ids, &count)) return;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t memory = compat_runtime32_allocate(128, 1);
        if (!memory) break;
        displays[display_count].id = ids[i]; displays[display_count++].handle = memory;
        *(uint32_t *)(uintptr_t)memory = memory + 4;
        *(uint32_t *)(uintptr_t)(memory + 68) = memory + 72;
        struct device32 *device = (void *)(uintptr_t)(memory + 4);
        struct pixmap32 *pixmap = (void *)(uintptr_t)(memory + 72);
        CGRect bounds = CGDisplayBounds(ids[i]);
        device->id = i; device->type = 2; device->resolution = 4;
        device->flags = (1u << 0) | (1u << 13) | (1u << 15) | (CGDisplayIsMain(ids[i]) ? (1u << 11) : 0);
        device->pixmap = memory + 68;
        device->bounds[0] = bounds.origin.y; device->bounds[1] = bounds.origin.x;
        device->bounds[2] = bounds.origin.y + bounds.size.height;
        device->bounds[3] = bounds.origin.x + bounds.size.width;
        memcpy(pixmap->bounds, device->bounds, 8);
        pixmap->row_bytes = 0x8000 | (((uint32_t)bounds.size.width * 4) & 0x3fff);
        pixmap->horizontal_resolution = pixmap->vertical_resolution = 72 << 16;
        pixmap->pixel_type = 16; pixmap->pixel_size = 32;
        pixmap->components = 3; pixmap->component_size = 8; pixmap->pixel_format = 'BGRA';
        if (display_count > 1) {
            struct device32 *previous = (void *)(uintptr_t)(displays[display_count - 2].handle + 4);
            previous->next = memory;
        }
        if (CGDisplayIsMain(ids[i])) current_device = memory;
    }
}
static unsigned device_index(uint32_t device) {
    for (unsigned i = 0; i < display_count; ++i) if (displays[i].handle == device) return i;
    return display_count;
}
uint32_t carbon_display32_id(uint32_t device) {
    pthread_mutex_lock(&lock); initialize_displays();
    unsigned i = device_index(device); uint32_t id = i < display_count ? displays[i].id : 0;
    pthread_mutex_unlock(&lock); return id;
}
int carbon_display32_dispatch(const char *name, const uint32_t *a, uint64_t *result) {
    if (region_dispatch(name, a, result)) return 1;
    if (strcmp(name, "_DMGetGDeviceByDisplayID") && strcmp(name, "_DMGetDisplayIDByGDevice") &&
        strcmp(name, "_GetMainDevice") && strcmp(name, "_GetDeviceList") && strcmp(name, "_GetNextDevice") &&
        strcmp(name, "_GetGDevice") && strcmp(name, "_GetGWorldDevice") && strcmp(name, "_SetGDevice") &&
        strcmp(name, "_TestDeviceAttribute")) return 0;
    pthread_mutex_lock(&lock); initialize_displays();
    uint32_t main = 0;
    for (unsigned i = 0; i < display_count; ++i) if (CGDisplayIsMain(displays[i].id)) main = displays[i].handle;
    if (!strcmp(name, "_GetMainDevice")) *result = main;
    else if (!strcmp(name, "_GetDeviceList")) *result = display_count ? displays[0].handle : 0;
    else if (!strcmp(name, "_GetGDevice") || !strcmp(name, "_GetGWorldDevice")) *result = current_device;
    else if (!strcmp(name, "_GetNextDevice")) {
        unsigned i = device_index(a[0]); *result = i + 1 < display_count ? displays[i + 1].handle : 0;
    } else if (!strcmp(name, "_SetGDevice")) {
        if (device_index(a[0]) < display_count) current_device = a[0]; *result = 0;
    } else if (!strcmp(name, "_TestDeviceAttribute")) {
        unsigned i = device_index(a[0]);
        struct device32 *device = i < display_count ? (void *)(uintptr_t)(displays[i].handle + 4) : NULL;
        *result = device && a[1] < 16 && (device->flags & (1u << a[1])) != 0;
    } else {
        uint32_t value = 0;
        bool to_device = !strcmp(name, "_DMGetGDeviceByDisplayID");
        for (unsigned i = 0; i < display_count; ++i)
            if ((to_device ? displays[i].id : displays[i].handle) == a[0]) value = to_device ? displays[i].handle : displays[i].id;
        if (!value && a[2]) value = to_device ? main : CGMainDisplayID();
        if (a[1]) *(uint32_t *)(uintptr_t)a[1] = value;
        *result = value && a[1] ? 0 : (uint32_t)-50;
    }
    pthread_mutex_unlock(&lock); return 1;
}
