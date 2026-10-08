/* Validate host resource configuration without opening a GPU device. */
#include "../../src/client.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

static void *allocate(size_t size) { (void)size; abort(); }
static void deallocate(void *memory, size_t size) { (void)memory; (void)size; abort(); }

int main(int argc, char **argv)
{
    assert(argc == 2);
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    bool (*configure)(void *(*)(size_t), void (*)(void *, size_t)) =
        dlsym(library, "glmetal_set_shared_buffer_allocator");
    bool (*prewarm)(unsigned, unsigned) = dlsym(library, "glmetal_prewarm_command_queues");
    assert(configure && prewarm);
    assert(!configure(allocate, NULL));
    assert(!configure(NULL, deallocate));
    assert(configure(allocate, deallocate));
    assert(configure(NULL, NULL));
    assert(!prewarm(0, 6));
    assert(!prewarm(4, 0));
    assert(!prewarm(65, 6));
    assert(!prewarm(4, 65));
    puts("GLMetal host resource configuration PASS");
    return 0;
}
