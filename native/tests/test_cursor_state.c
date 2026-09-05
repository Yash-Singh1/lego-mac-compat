#include "cursor_state.h"
#include <assert.h>
#include <stdio.h>

struct mouse {
    bool hidden, captured, fail_release;
    unsigned hides, shows, captures, releases, warps;
};

static int hide(void *context, bool hidden)
{
    struct mouse *mouse = context;
    mouse->hidden = hidden;
    if (hidden) ++mouse->hides;
    else ++mouse->shows;
    return 0;
}

static int capture(void *context, bool captured)
{
    struct mouse *mouse = context;
    if (!captured && mouse->fail_release) return 17;
    mouse->captured = captured;
    if (captured) ++mouse->captures;
    else ++mouse->releases;
    return 0;
}

static int warp(void *context, float x, float y)
{
    struct mouse *mouse = context;
    assert(x == 960 && y == 600);
    ++mouse->warps;
    return 0;
}

int main(void)
{
    struct lp32_cursor_state state = {0};
    struct mouse mouse = {0};
    const struct lp32_cursor_host host = {&mouse, hide, capture, warp};

    /* Startup/background requests update guest intent without taking the mouse. */
    assert(!lp32_cursor_hide(&state, &host, true));
    assert(!lp32_cursor_capture(&state, &host, true));
    assert(!lp32_cursor_warp(&state, &host, 960, 600));
    assert(state.hide_count == 1 && !mouse.hidden && !mouse.captured && !mouse.warps);
    assert(!lp32_cursor_focus(&state, &host, true));
    assert(mouse.hidden && mouse.captured);
    assert(!lp32_cursor_warp(&state, &host, 960, 600));
    assert(mouse.warps == 1);

    /* Nested hides own one host hide. App switching releases it immediately. */
    assert(!lp32_cursor_hide(&state, &host, true));
    assert(state.hide_count == 2 && mouse.hides == 1);
    assert(!lp32_cursor_focus(&state, &host, false));
    assert(!mouse.hidden && !mouse.captured && state.hide_count == 2);
    unsigned calls = mouse.hides + mouse.captures + mouse.warps;
    for (unsigned i = 0; i < 100; ++i) {
        assert(!lp32_cursor_capture(&state, &host, true));
        assert(!lp32_cursor_warp(&state, &host, 960, 600));
    }
    assert(calls == mouse.hides + mouse.captures + mouse.warps);
    assert(!lp32_cursor_hide(&state, &host, true));
    assert(!mouse.hidden && state.hide_count == 3);
    assert(!lp32_cursor_hide(&state, &host, false));
    assert(!lp32_cursor_focus(&state, &host, true));
    assert(mouse.hidden && mouse.captured && mouse.hides == 2);

    /* Source opens its menu in the background: returning must not capture. */
    assert(!lp32_cursor_focus(&state, &host, false));
    assert(!lp32_cursor_capture(&state, &host, false));
    assert(!lp32_cursor_hide(&state, &host, false));
    assert(!lp32_cursor_hide(&state, &host, false));
    assert(!lp32_cursor_hide(&state, &host, false));
    assert(state.hide_count == 0);
    assert(!lp32_cursor_focus(&state, &host, true));
    assert(!mouse.hidden && !mouse.captured);
    assert(mouse.hides == mouse.shows && mouse.captures == mouse.releases);

    /* A host failure must not prevent the other release or disable retries. */
    assert(!lp32_cursor_hide(&state, &host, true));
    assert(!lp32_cursor_capture(&state, &host, true));
    mouse.fail_release = true;
    assert(lp32_cursor_focus(&state, &host, false) == 17);
    assert(!mouse.hidden && mouse.captured);
    assert(!lp32_cursor_warp(&state, &host, 960, 600));
    assert(mouse.warps == 1);
    mouse.fail_release = false;
    assert(!lp32_cursor_focus(&state, &host, false));
    assert(!mouse.captured);
    puts("cursor focus, background requests, nesting and host retries: PASS");
    return 0;
}
