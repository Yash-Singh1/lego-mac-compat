#ifndef LP32_CRASH_TRACE_H
#define LP32_CRASH_TRACE_H
#include <stdint.h>
/* Called during startup, never from the signal handler. */
void crash_trace_init(void);
void crash_trace_set_path(const char *path);
/* Captures the faulting context, not the signal handler's own backtrace. */
void crash_trace_capture(int signal_number, uint64_t pc, uint64_t sp,
                         uint64_t fp, uint64_t cs);
#endif
