#ifndef LP32_TFU_TIMING_H
#define LP32_TFU_TIMING_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>

static inline uint64_t tfu_time_ns(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/* Wall time correlates hitches with the system log; duration uses a clock
   unaffected by wall-clock adjustments. Only slow operations emit output. */
static inline void tfu_log_slow(const char *operation, unsigned object,
                                uint64_t start, uint64_t end)
{
    if (end - start < 100000000) return;
    struct timespec wall;
    struct tm local;
    char stamp[32];
    clock_gettime(CLOCK_REALTIME, &wall);
    localtime_r(&wall.tv_sec, &local);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    uint64_t thread = 0;
    pthread_threadid_np(NULL, &thread);
    fprintf(stderr, "compat32: TFU slow %s.%03ld operation=%s object=%u duration=%.3fms tid=%llu\n",
            stamp, wall.tv_nsec / 1000000, operation, object,
            (end - start) / 1e6, (unsigned long long)thread);
}

#endif
