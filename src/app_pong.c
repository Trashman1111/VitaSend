/*
 * app_pong.c - the glue between the Pong easter egg (pong.c, which knows nothing of this program) and the interface:
 *
 *   - unlocking: 11 taps on the logo at the top left (each within 3 seconds of the one before); the unlock is kept in pong.json
 *   - opening: L and R pressed together, from any of the five pages (and again to leave the game)
 *   - the game screen: the top bar as everywhere (logo, battery, clock), the court, and at the bottom left the progress of a
 *     running transfer / installation / unzipping / update download, so that a download can be followed while playing
 *
 * Author: trashman (VitaSend)
 */
#include "app_int.h"
#include "pong.h"
#include <stdlib.h>
#include "unzip.h"

#define UNLOCK_TAPS   11
#define UNLOCK_GAP_MS 3000

void pong_glue_init(void)
{
    pong_load(plat_data_dir());
#ifdef LS_HOST
    const char *sim = getenv("LS_PONG_SIM");               /* test build: bot games instead of the program (see pong_simulate) */
    if (sim && *sim) pong_simulate(sim);
#endif
    ui_set_logo_handler(pong_logo_tap);
}

void pong_logo_tap(void)
{
    if (pong_unlocked()) return;
    static int      taps;
    static uint64_t last;
    uint64_t now = plat_time_ms();
    if (now - last > UNLOCK_GAP_MS) taps = 0;                  /* too slow: start counting again */
    last = now;
    if (++taps < UNLOCK_TAPS) return;
    taps = 0;
    pong_unlock();
    ls_log("Easter egg unlocked");
    toast("Easter egg unlocked - press L and R together to play");
}

static void pong_close(void)
{
    pong_finish();
    int deferred = A.pong_defer;
    A.pong_defer = 0;
    A.sc = (screen_id)A.tab;                                   /* back on the page underneath ... */
    ui_dialog_invalidate();
    if (deferred) go((screen_id)deferred);                    /* ... or on the transfer / installation that came meanwhile */
}

/* L and R at the same moment. */
bool pong_open_keys(uint32_t pressed)
{
    if (!(pressed & (UI_BTN_L | UI_BTN_R)) || !(A.held & UI_BTN_L) || !(A.held & UI_BTN_R)) return false;
    if (!pong_unlocked()) return false;
    if (A.sc == SC_PONG) { pong_close(); return true; }
    if (A.sc > SC_SETTINGS) return false;                      /* not over a dialog */
    A.side_focus = false;
    A.pong_defer = 0;
    if (!pong_started() || pong_over()) pong_new_game();
    else pong_pause(true);                                     /* a game that was left: it waits for START */
    go(SC_PONG);
    return true;
}

void pong_screen_left(void)
{
    pong_pause(true);
}

void pong_screen_tick(uint64_t now)
{
    pong_input in = { .up = (A.held & UI_BTN_UP) != 0, .down = (A.held & UI_BTN_DOWN) != 0 };
    if (!A.swallow_touch && A.touch_down) { in.touch = true; in.touch_y = A.touch_y; }
    pong_step(now, &in);
}

void pong_screen_input(uint32_t p)
{
    if (p & UI_BTN_CIRCLE) { pong_close(); return; }
    if (p & UI_BTN_START) { pong_pause(!pong_paused()); return; }
    if (p & UI_BTN_CROSS) {
        if (pong_over()) pong_new_game();
        else if (pong_paused()) pong_pause(false);
    }
}

/* What is going on in the background, as "Receiving 42%" - or nothing. */
static bool background_job(const xfer_status *x, char *label, size_t cap, int *pct)
{
    vpk_status vs;
    vpk_snapshot(&vs);
    if (vs.state == VPK_RUNNING) {                             /* "Unpacking", "Installing ...": the stage the installer is in */
        ls_strlcpy(label, vs.stage[0] ? vs.stage : "Installing", cap);
        *pct = vs.pct < 0 ? 0 : vs.pct;
        return true;
    }
    if (xfer_running(x)) {
        ls_strlcpy(label, x->outgoing ? "Sending" : "Receiving", cap);
        *pct = xfer_percent(x);
        return true;
    }
    unzip_status uz;
    unzip_snapshot(&uz);
    if (uz.state == UZ_RUNNING) {
        ls_strlcpy(label, "Unzipping", cap);
        *pct = uz.total ? (int)(uz.done * 100 / uz.total) : 0;
        return true;
    }
    upd_status us;
    update_snapshot(&us);
    if (us.state == UPD_DOWNLOADING) {
        ls_strlcpy(label, "Downloading the update", cap);
        *pct = us.size ? (int)(us.done * 100 / us.size) : 0;
        return true;
    }
    return false;
}

void pong_screen_draw(const xfer_status *x)
{
    gfx_clear(C_BG);
    draw_topbar();                                             /* logo, battery, clock: where they are everywhere else */
    pong_draw();

    char label[96], line[140];
    int pct = 0;
    if (background_job(x, label, sizeof label, &pct)) {
        if (pct > 100) pct = 100;
        snprintf(line, sizeof line, "%s  %d%%", label, pct);
        gfx_text_fit(PONG_X, 508, 560, 15, GW_MED, C_TEXT, GA_LEFT, line);
        ui_progress(PONG_X, 522, 240, 6, (uint64_t)pct, 100, C_ACCENT);
    }
}
