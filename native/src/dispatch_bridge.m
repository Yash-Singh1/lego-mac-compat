#include "dispatch_bridge.h"
#include "blocks_bridge.h"
#include "compat_runtime.h"
#include "objc_bridge.h"
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
static dispatch_queue_t queue(uint32_t token) {
    if (compat_runtime32_pointer_import_matches(token, "__dispatch_main_q")) return dispatch_get_main_queue();
    return (dispatch_queue_t)objc_bridge32_host_object(token);
}
static uint64_t wide(const uint32_t *a) {return (uint64_t)a[0] | ((uint64_t)a[1] << 32);}
static void invoke_function(void *opaque) {
    uint32_t *a = opaque;
    compat_runtime32_call(a[1], a, 1);
    free(a);
}
struct once_entry {uint32_t address; dispatch_once_t predicate; struct once_entry *next;};
static struct once_entry *once_entries;
static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
int dispatch_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *out) {
    if (strncmp(name, "_dispatch_", 10)) return 0;
    *out = 0;
    if (!strcmp(name, "_dispatch_get_global_queue")) {
        *out = objc_bridge32_guest_object(dispatch_get_global_queue((long)(int32_t)a[0], a[1])); return 1;
    }
    if (!strcmp(name, "_dispatch_queue_create")) {
        dispatch_queue_t q = dispatch_queue_create((void *)(uintptr_t)a[0], (dispatch_queue_attr_t)objc_bridge32_host_object(a[1]));
        *out = objc_bridge32_guest_object(q); if (q) dispatch_release(q); return 1;
    }
    if (!strcmp(name, "_dispatch_group_create")) {
        dispatch_group_t g = dispatch_group_create();
        *out = objc_bridge32_guest_object(g); if (g) dispatch_release(g); return 1;
    }
    if (!strcmp(name, "_dispatch_group_wait")) {
        *out = (uint32_t)dispatch_group_wait((dispatch_group_t)objc_bridge32_host_object(a[0]), wide(a + 1)); return 1;
    }
    if (!strcmp(name, "_dispatch_once")) {
        uint32_t *guest = (void *)(uintptr_t)a[0];
        if (!guest) return 0;
        if (__atomic_load_n(guest, __ATOMIC_ACQUIRE) == UINT32_MAX) return 1;
        pthread_mutex_lock(&once_lock);
        struct once_entry *e = once_entries;
        while (e && e->address != a[0]) e = e->next;
        if (!e) {e = calloc(1, sizeof(*e)); if (e) {e->address = a[0]; e->next = once_entries; once_entries = e;}}
        if (e && !*guest) {e->predicate = 0; __atomic_store_n(guest, 1, __ATOMIC_RELEASE);}
        pthread_mutex_unlock(&once_lock);
        if (!e) return 0;
        dispatch_once(&e->predicate, ^{blocks_bridge32_invoke(a[1]);});
        __atomic_store_n(guest, UINT32_MAX, __ATOMIC_RELEASE); return 1;
    }
    if (!strcmp(name, "_dispatch_async_f") || !strcmp(name, "_dispatch_sync_f")) {
        dispatch_queue_t q = queue(a[0]); if (!q) return 0;
        uint32_t *context = malloc(8); if (!context) return 0;
        context[0] = a[1]; context[1] = a[2];
        if (!strcmp(name, "_dispatch_async_f")) dispatch_async_f(q, context, invoke_function);
        else dispatch_sync_f(q, context, invoke_function);
        return 1;
    }
    if (!strcmp(name, "_dispatch_apply_f")) {
        dispatch_queue_t q = queue(a[1]); if (!q) return 0;
        uint32_t function = a[3], context = a[2];
        dispatch_apply(a[0], q, ^(size_t index) {
            uint32_t args[] = {context, (uint32_t)index}; compat_runtime32_call(function, args, 2);
        }); return 1;
    }
    bool after = !strcmp(name, "_dispatch_after"), group = !strcmp(name, "_dispatch_group_async");
    bool async = !strcmp(name, "_dispatch_async"), sync = !strcmp(name, "_dispatch_sync");
    if (!after && !group && !async && !sync) return 0;
    dispatch_queue_t q = queue(a[after ? 2 : group ? 1 : 0]); if (!q) return 0;
    uint32_t block = blocks_bridge32_copy(a[after ? 3 : group ? 2 : 1]); if (!block) return 0;
    dispatch_block_t work = ^{blocks_bridge32_invoke(block); blocks_bridge32_release(block);};
    if (after) dispatch_after(wide(a), q, work);
    else if (group) dispatch_group_async((dispatch_group_t)objc_bridge32_host_object(a[0]), q, work);
    else if (sync) dispatch_sync(q, work);
    else dispatch_async(q, work);
    return 1;
}
