#ifndef LP32_HITCH_RECORDER_H
#define LP32_HITCH_RECORDER_H
#include <stdint.h>
#include <stdbool.h>

enum hitch_kind { HITCH_UNKNOWN, HITCH_NONE, HITCH_DRAW, HITCH_UPLOAD,
    HITCH_SHADER, HITCH_IO, HITCH_WAIT, HITCH_AUDIO, HITCH_KIND_COUNT };
extern int hitch_recorder_enabled;
uint64_t hitch_now(void);
unsigned hitch_classify(const char *name);
int hitch_start(const char *path, double threshold_ms);
void hitch_stop(void);
void hitch_program(uint32_t target, uint32_t program);
void hitch_note(unsigned kind, const char *name, uint32_t caller,
                uint64_t start, uint64_t end, uint32_t draw_count);
/* Called once per presented frame, after pacing. Durations are CPU wall time. */
void hitch_frame(uint64_t swap, uint64_t work_end, uint64_t flush_end,
                 uint64_t present_end, uint64_t target_ns, bool active);
#endif
