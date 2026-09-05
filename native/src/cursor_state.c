#include "cursor_state.h"

static int apply(struct lp32_cursor_state *state, const struct lp32_cursor_host *host)
{
    bool captured = state->focused && state->capture_requested;
    bool hidden = state->focused && state->hide_count != 0;
    int error = 0;
    if (state->host_captured != captured) {
        error = host->set_captured(host->context, captured);
        if (!error) state->host_captured = captured;
    }
    /* Attempt both releases even if one host operation fails. Only undo the
       single native hide this bridge owns, regardless of guest nesting. */
    if (state->host_hidden != hidden) {
        int hide_error = host->set_hidden(host->context, hidden);
        if (!hide_error) state->host_hidden = hidden;
        if (!error) error = hide_error;
    }
    return error;
}

int lp32_cursor_focus(struct lp32_cursor_state *state, const struct lp32_cursor_host *host,
                      bool focused)
{
    state->focused = focused;
    return apply(state, host);
}

int lp32_cursor_hide(struct lp32_cursor_state *state, const struct lp32_cursor_host *host,
                     bool hidden)
{
    if (hidden) {
        if (state->hide_count != UINT32_MAX) ++state->hide_count;
    } else if (state->hide_count) {
        --state->hide_count;
    }
    return apply(state, host);
}

int lp32_cursor_capture(struct lp32_cursor_state *state, const struct lp32_cursor_host *host,
                        bool captured)
{
    state->capture_requested = captured;
    return apply(state, host);
}

int lp32_cursor_warp(struct lp32_cursor_state *state, const struct lp32_cursor_host *host,
                     float x, float y)
{
    return state->focused ? host->warp(host->context, x, y) : 0;
}
