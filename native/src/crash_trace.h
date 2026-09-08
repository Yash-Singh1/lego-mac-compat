#ifndef LP32_CRASH_TRACE_H
#define LP32_CRASH_TRACE_H
#include <stddef.h>
#include <stdint.h>

enum lp32_trace_kind {
    LP32_TRACE_ALLOC = 1, LP32_TRACE_FREE, LP32_TRACE_REALLOC,
    LP32_TRACE_BAD_FREE, LP32_TRACE_ANIM_BEFORE, LP32_TRACE_ANIM_AFTER,
    LP32_TRACE_TICK_BEFORE, LP32_TRACE_TICK_AFTER,
    LP32_TRACE_TOUCH_BEFORE, LP32_TRACE_TOUCH_AFTER,
    LP32_TRACE_QUEUE, LP32_TRACE_ENTITY, LP32_TRACE_DESTROY_BEFORE, LP32_TRACE_DESTROY_AFTER,
    LP32_TRACE_QUEUE_INVALIDATE, LP32_TRACE_QUEUE_COMPACT
};
/* Bounded, allocation-free recorder; concurrent writers never wait. */
void lp32_trace_record(unsigned kind, uint32_t address, uint32_t size,
                       uint32_t site, uint32_t detail);
void lp32_trace_module(const char *path, uint32_t base, uint32_t end,
                       uint32_t slide, const unsigned char uuid[16]);
int lp32_crash_read(uint64_t address, void *out, size_t size);
int lp32_crash_thread_stack(void);
void lp32_crash_capture(int fd, uint64_t pc, uint64_t sp, uint64_t bp,
                       const uint64_t *registers, size_t count, int guest);
void lp32_crash_dump_words(int fd, const char *label, uint64_t address, unsigned words);
#endif
