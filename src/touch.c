/*
 * touch.c - tap / drag recognition for the front touch screen.
 *
 * Author: trashman (VitaSend)
 */
#include "touch.h"

static bool s_down, s_drag;
static int  s_sx, s_sy, s_lx, s_ly;

void touch_reset(void)
{
    s_down = s_drag = false;
}

bool touch_feed(bool down, int x, int y, touch_event *ev)
{
    if (down && !s_down) {                              /* finger put on the screen */
        s_down = true;
        s_drag = false;
        s_sx = s_lx = x;
        s_sy = s_ly = y;
        return false;
    }
    if (down) {                                         /* finger still there */
        s_lx = x;
        s_ly = y;
        int dx = x - s_sx, dy = y - s_sy;
        if (!s_drag && dx * dx + dy * dy > TOUCH_SLOP * TOUCH_SLOP) {
            s_drag = true;
            ev->kind = TOUCH_DRAG_BEGIN;
        } else if (s_drag) {
            ev->kind = TOUCH_DRAG;
        } else {
            return false;
        }
        ev->sx = s_sx; ev->sy = s_sy; ev->dx = dx; ev->dy = dy;
        return true;
    }
    if (s_down) {                                       /* finger lifted */
        s_down = false;
        ev->kind = s_drag ? TOUCH_DRAG_END : TOUCH_TAP;
        ev->sx = s_sx; ev->sy = s_sy;
        ev->dx = s_lx - s_sx; ev->dy = s_ly - s_sy;
        s_drag = false;
        return true;
    }
    return false;
}
