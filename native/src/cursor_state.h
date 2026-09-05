#ifndef LP32_CURSOR_STATE_H
#define LP32_CURSOR_STATE_H

#include <stdbool.h>
#include <stdint.h>

/* Zero-initialize; callers serialize every operation, including host calls.
   Guest intent survives focus loss, but only a focused window owns the mouse. */
struct lp32_cursor_state {
    uint32_t hide_count;
    bool capture_requested;
    bool focused;
    bool host_hidden;
    bool host_captured;
};

struct lp32_cursor_host {
    void *context;
    int (*set_hidden)(void *context, bool hidden);
    int (*set_captured)(void *context, bool captured);
    int (*warp)(void *context, float x, float y);
};

int lp32_cursor_focus(struct lp32_cursor_state *, const struct lp32_cursor_host *, bool);
int lp32_cursor_hide(struct lp32_cursor_state *, const struct lp32_cursor_host *, bool);
int lp32_cursor_capture(struct lp32_cursor_state *, const struct lp32_cursor_host *, bool);
int lp32_cursor_warp(struct lp32_cursor_state *, const struct lp32_cursor_host *, float, float);

#endif
