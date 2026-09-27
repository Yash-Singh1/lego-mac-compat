#ifndef LP32_MOUSE_BUTTONS_H
#define LP32_MOUSE_BUTTONS_H

#include <stdint.h>

/*
 * Buttons that were down in *held and are up in pressed. *held becomes the
 * pressed mask. Only the five AppKit buttons (left, right, other, and the
 * two extras) are tracked.
 */
static inline uint32_t lp32_released_mouse_buttons(uint32_t *held, uint32_t pressed)
{
    uint32_t now = pressed & 0x1fu;
    uint32_t released = *held & ~now;
    *held = now;
    return released;
}

#endif
