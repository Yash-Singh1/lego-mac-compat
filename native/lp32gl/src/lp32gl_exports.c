/* Backing table for the exported gl* symbols generated from Apple's export
 * list (see tools/gen_exports.py).  Until LP32GL has initialized, every slot
 * points at a stub; no GL call can reach Mesa before a context exists. */
#include <stdint.h>
#include <string.h>

#include "lp32gl_internal.h"
#include "lp32gl_export_names.h"

static uintptr_t not_initialized(void) { return 0; }

__attribute__((visibility("hidden"))) void *lp32gl_export_table[LP32GL_EXPORT_COUNT];

__attribute__((constructor)) static void preset_table(void)
{
    for (unsigned i = 0; i < LP32GL_EXPORT_COUNT; ++i) lp32gl_export_table[i] = (void *)not_initialized;
}

void lp32gl_exports_resolve(void)
{
    for (unsigned i = 0; i < LP32GL_EXPORT_COUNT; ++i) {
        void *function = lp32gl_gl_proc_address(lp32gl_export_names[i]);
        if (function) lp32gl_export_table[i] = function;
    }
}
