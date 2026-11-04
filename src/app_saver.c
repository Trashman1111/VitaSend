/*
 * app_saver.c - the screensaver
 *
 * Author: trashman (VitaSend)
 */
#include "app_int.h"

/* ============================================================== screensaver */
/* After a while without input the screen goes black and the VitaSend name and logo drift around it in straight lines,
 * bouncing off the edges like the old DVD players, with the progress of a running transfer or installation. Receiving goes
 * on as usual; a request, a message or the end of a transfer brings the app back, and so does any button or touch. */

#define SAVER_W 372
#define SAVER_H 118

uint32_t idle_unit_ms(void)                           /* 1000 on the Vita; tests shorten it */
{
    static int unit = -1;
    if (unit < 0) {
        const char *e = getenv("LS_IDLE_UNIT_MS");
        unit = e && atoi(e) > 0 ? atoi(e) : 1000;
    }
    return (uint32_t)unit;
}

void saver_start(void)
{
    uint8_t r[4];
    if (!plat_random(r, sizeof r)) memset(r, 0x5a, sizeof r);
    A.sx = (float)(r[0] * (GFX_W - SAVER_W) / 255);
    A.sy = (float)(r[1] * (GFX_H - SAVER_H) / 255);
    A.vx = (r[2] & 1) ? 30.0f : -30.0f;               /* a slow drift (pixels per second) */
    A.vy = (r[3] & 1) ? 21.0f : -21.0f;
    A.saver = true;
    A.saver_t = plat_time_ms();
    ls_log("screensaver on");
}

void saver_stop(const char *why)
{
    if (!A.saver) return;
    A.saver = false;
    A.last_input = plat_time_ms();                   /* the idle time counts from here */
    ui_dialog_invalidate();
    ls_log("screensaver off (%s)", why);
}

void saver_step(void)
{
    uint64_t now = plat_time_ms();
    float dt = (float)(now - A.saver_t) / 1000.0f;
    A.saver_t = now;
    if (dt > 0.25f) dt = 0.25f;
    A.sx += A.vx * dt;
    A.sy += A.vy * dt;
    float maxx = (float)(GFX_W - SAVER_W), maxy = (float)(GFX_H - SAVER_H);
    if (A.sx < 0) { A.sx = 0; A.vx = -A.vx; }
    if (A.sx > maxx) { A.sx = maxx; A.vx = -A.vx; }
    if (A.sy < 0) { A.sy = 0; A.vy = -A.vy; }
    if (A.sy > maxy) { A.sy = maxy; A.vy = -A.vy; }
}

void saver_draw(const xfer_status *x)
{
    gfx_clear(GRGB(0, 0, 0));
    int bx = (int)A.sx, by = (int)A.sy;
    gfx_rrect(bx, by, 64, 64, 18, C_ACCENT);
    gfx_icon(IC_LOGO, (float)(bx + 32), (float)(by + 32), 54, GRGB(255, 255, 255));
    gfx_text(bx + 82, by + 32, 40, GW_BOLD, GRGB(235, 240, 245), GA_LEFT, "VitaSend");

    /* progress of a running transfer / installation */
    char line[96];
    vpk_status vs;
    vpk_snapshot(&vs);
    int pct = -1;
    if (xfer_running(x)) {
        pct = xfer_percent(x);
        snprintf(line, sizeof line, "%s %d%%", x->outgoing ? "Sending" : "Receiving", pct);
    } else if (vs.state == VPK_RUNNING) {
        pct = vs.pct;
        snprintf(line, sizeof line, "Installing %d%%", pct);
    }
    if (pct >= 0) {
        gfx_text(bx + 2, by + 94, 18, GW_MED, C_ACCENT_HI, GA_LEFT, line);
        ui_progress(bx + 2, by + 106, SAVER_W - 8, 8, (uint64_t)pct, 100, C_ACCENT);
    }
}

