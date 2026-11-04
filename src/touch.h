/*
 * touch.h - turns the raw finger position of the front touch screen into taps and drags.
 * Platform independent: ui_vita.c / ui_host.c only say "finger down at x,y" or "no finger".
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_TOUCH_H
#define LS_TOUCH_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TOUCH_TAP,           /* finger lifted without having moved much: (sx, sy) is where it touched */
    TOUCH_DRAG_BEGIN,    /* finger moved further than a tap allows; (dx, dy) = movement since touching down */
    TOUCH_DRAG,          /* finger still moving */
    TOUCH_DRAG_END       /* finger lifted after a drag */
} touch_kind;

typedef struct {
    touch_kind kind;
    int sx, sy;          /* where the finger first touched */
    int dx, dy;          /* current position minus (sx, sy) */
} touch_event;

#define TOUCH_SLOP 16    /* pixels (of 960x544) a finger may wobble and still count as a tap */

/* Call once per frame. Returns true when `ev` was filled in (at most one event per call). */
bool touch_feed(bool down, int x, int y, touch_event *ev);
/* Forget a finger that is currently down (e.g. after the screen changed completely). */
void touch_reset(void);

#endif
