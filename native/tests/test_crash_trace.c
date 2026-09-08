#include "crash_trace.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void *writer(void *argument)
{
    uint32_t base = (uint32_t)(uintptr_t)argument;
    for (unsigned i = 0; i < 20000; ++i) {
        uint32_t p = base+i;
        lp32_trace_record(LP32_TRACE_ALLOC, p, p^0x55aa, p^0xaabb, p^0xccdd);
    }
    return NULL;
}
int main(void)
{
    uint32_t value = 123, read = 0;
    assert(lp32_crash_read((uintptr_t)&value, &read, 4) && read == value);
    void *page = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE|MAP_ANON, -1, 0);
    assert(page != MAP_FAILED && !lp32_crash_read((uintptr_t)page, &read, 4));
    assert(!lp32_crash_read(UINT64_MAX-2, &read, 4));
    lp32_trace_record(LP32_TRACE_TOUCH_BEFORE, 0x1234, 0, 0, 0x5678);
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_create(&threads[i], NULL, writer, (void *)(uintptr_t)((i+1)*0x100000)));
    /* Capture while writers run too: every accepted record must be coherent. */
    FILE *report = tmpfile(); assert(report);
    lp32_crash_capture(fileno(report), 0, 1, 1, NULL, 0, 1);
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
    lp32_crash_capture(fileno(report), 0, 1, 1, NULL, 0, 1);
    rewind(report);
    char *line = NULL; size_t capacity = 0; unsigned events = 0, ends = 0, physics = 0;
    while (getline(&line, &capacity, report) > 0) {
        unsigned long long seq, time, kind, addr, size, site, detail;
        if (sscanf(line, "capture event seq=%llx time=%llx kind=%llx address=%llx size=%llx site=%llx detail=%llx",
                   &seq, &time, &kind, &addr, &size, &site, &detail) == 7) {
            if (kind == LP32_TRACE_ALLOC) {
                assert(size == (addr^0x55aa) && site == (addr^0xaabb) && detail == (addr^0xccdd));
                ++events;
            } else if (kind == LP32_TRACE_TOUCH_BEFORE) ++physics;
        }
        if (!strcmp(line, "capture end\n")) ++ends;
    }
    assert(events > 0 && events <= 2*65536 && ends == 2 && physics == 2);
    free(line); fclose(report); munmap(page, 4096);
    puts("Crash recorder: PASS (concurrent capture, ring wrap, separate physics history, inaccessible memory)");
}
