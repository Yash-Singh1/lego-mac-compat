#ifndef LP32_SOUND_MANAGER_BRIDGE_H
#define LP32_SOUND_MANAGER_BRIDGE_H
#include <stdint.h>
int sound_manager_bridge32_dispatch(const char *, const uint32_t *, uint64_t *);
/* Tests: mix without an output device, render on demand, and run completion
   events on the channel workers synchronously. Call before creating channels. */
void sound_manager_bridge32_use_test_output(double rate);
void sound_manager_bridge32_render(float *left, float *right, uint32_t frames);
void sound_manager_bridge32_deliver_events(void);
#endif
