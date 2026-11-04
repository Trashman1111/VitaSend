/*
 * ui_vita.c - shows the finished picture (gfx.c draws into normal memory) on the display and reads the buttons.
 *
 * Two framebuffers in CDRAM: the picture is copied into the one that is NOT on screen, then it is swapped in.
 *
 * Author: trashman (VitaSend)
 */
#include "gfx.h"

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/touch.h>

#include <string.h>

#define FB_W      GFX_W
#define FB_H      GFX_H
#define FB_STRIDE (2 * 1024 * 1024)           /* each buffer takes 2 MB (CDRAM blocks are a multiple of 256 KB) */

static uint8_t *s_base;
static SceUID   s_fb_uid = -1;
static int      s_next;                       /* the buffer to draw into next */
static int      s_tx0, s_ty0, s_tw = 1920, s_th = 1088;      /* active area of the front touch panel */

static void show(int idx)
{
    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof fb);
    fb.size = sizeof fb;
    fb.base = s_base + (size_t)idx * FB_STRIDE;
    fb.pitch = FB_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = FB_W;
    fb.height = FB_H;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

bool ui_init(void)
{
    s_fb_uid = sceKernelAllocMemBlock("ls_display", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 2 * FB_STRIDE, NULL);
    if (s_fb_uid < 0) return false;
    if (sceKernelGetMemBlockBase(s_fb_uid, (void **)&s_base) < 0) return false;

    uint32_t *a = (uint32_t *)s_base, *b = (uint32_t *)(s_base + FB_STRIDE);
    for (int i = 0; i < FB_W * FB_H; i++) a[i] = b[i] = 0xFF171310u;     /* the background colour until the first frame */
    show(0);
    s_next = 1;

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);

    /* front touch screen only: the back panel is never started, so it can never produce input */
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    SceTouchPanelInfo pi;
    memset(&pi, 0, sizeof pi);
    if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &pi) >= 0 && pi.maxAaX > pi.minAaX && pi.maxAaY > pi.minAaY) {
        s_tx0 = pi.minAaX;
        s_ty0 = pi.minAaY;
        s_tw = pi.maxAaX - pi.minAaX + 1;
        s_th = pi.maxAaY - pi.minAaY + 1;
    }
    return true;
}

void ui_input_refresh(void)
{
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
}

void ui_shutdown(void)
{
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_STOP);
    if (s_fb_uid >= 0) {
        sceKernelFreeMemBlock(s_fb_uid);
        s_fb_uid = -1;
        s_base = NULL;
    }
}

void ui_present_frame(const uint32_t *pixels)
{
    if (!s_base) return;
    sceDisplayWaitVblankStart();                         /* the previous swap has certainly happened by now */
    memcpy(s_base + (size_t)s_next * FB_STRIDE, pixels, (size_t)FB_W * FB_H * 4);
    show(s_next);
    s_next ^= 1;
}

/* Drawing a frame can take a while; a quick tap must not fall between two reads. So all controller samples that
 * arrived since the last call are combined (plus the newest one, which tells whether a button is still held). */
uint32_t ui_buttons(void)
{
    static uint64_t last_ts;
    SceCtrlData pads[8];
    memset(pads, 0, sizeof pads);
    int n = sceCtrlPeekBufferPositive(0, pads, 8);
    if (n <= 0) return 0;
    if (n > 8) n = 8;
    uint32_t raw = 0;
    uint64_t newest_ts = 0;
    uint32_t newest_buttons = 0;
    for (int i = 0; i < n; i++) {
        if (pads[i].timeStamp > last_ts) raw |= pads[i].buttons;
        if (pads[i].timeStamp >= newest_ts) { newest_ts = pads[i].timeStamp; newest_buttons = pads[i].buttons; }
    }
    raw |= newest_buttons;
    if (newest_ts > last_ts) last_ts = newest_ts;

    uint32_t b = 0;
    if (raw & SCE_CTRL_START)    b |= UI_BTN_START;
    if (raw & SCE_CTRL_SELECT)   b |= UI_BTN_SELECT;
    if (raw & SCE_CTRL_TRIANGLE) b |= UI_BTN_TRIANGLE;
    if (raw & SCE_CTRL_CIRCLE)   b |= UI_BTN_CIRCLE;
    if (raw & SCE_CTRL_CROSS)    b |= UI_BTN_CROSS;
    if (raw & SCE_CTRL_SQUARE)   b |= UI_BTN_SQUARE;
    if (raw & SCE_CTRL_UP)       b |= UI_BTN_UP;
    if (raw & SCE_CTRL_DOWN)     b |= UI_BTN_DOWN;
    if (raw & SCE_CTRL_LEFT)     b |= UI_BTN_LEFT;
    if (raw & SCE_CTRL_RIGHT)    b |= UI_BTN_RIGHT;
    if (raw & SCE_CTRL_LTRIGGER) b |= UI_BTN_L;
    if (raw & SCE_CTRL_RTRIGGER) b |= UI_BTN_R;
    return b;
}

/* The first finger on the front touch screen, scaled from panel units to the 960 x 544 picture. */
bool ui_touch(int *x, int *y)
{
    SceTouchData td;
    memset(&td, 0, sizeof td);
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1) <= 0 || td.reportNum == 0) return false;
    int px = (td.report[0].x - s_tx0) * FB_W / s_tw;
    int py = (td.report[0].y - s_ty0) * FB_H / s_th;
    if (px < 0) px = 0; else if (px >= FB_W) px = FB_W - 1;
    if (py < 0) py = 0; else if (py >= FB_H) py = FB_H - 1;
    *x = px;
    *y = py;
    return true;
}
