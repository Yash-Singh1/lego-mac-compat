#ifndef LP32_AUDIO_QUEUE_BRIDGE_H
#define LP32_AUDIO_QUEUE_BRIDGE_H
#include <stdint.h>
int audio_queue_bridge32_dispatch(const char *, const uint32_t *, uint64_t *);
/* Bink feeds audio from the game loop. Do not throttle background presents
   while one of its output queues is playing. Safe to query without locking. */
int audio_queue_bridge32_movie_playing(void);
/* From the crash handler: report native audio buffers near the given host
   addresses (e.g. a corrupted allocator block) and the recent queue
   operations, to file descriptors fd and extra_fd (-1 to skip). */
void audio_queue_bridge32_crash_report(int fd, int extra_fd, const uint64_t *addresses, unsigned count);
#endif
