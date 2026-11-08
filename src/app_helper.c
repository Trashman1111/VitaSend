/*
 * app_helper.c - the screen of the updater app (see plat_update_helper_run): while it installs the new VitaSend it shows
 * the logo, the name and "Update in progress" instead of a black screen.
 *
 * It runs in its own thread and keeps drawing until the process ends (the updater starts VitaSend and exits).
 *
 * Author: trashman (VitaSend)
 */
#include "app.h"
#include "gfx.h"
#include "plat.h"
#include "ui.h"
#include "util.h"

static volatile int s_run;

static void draw(unsigned ms)
{
    gfx_begin();
    gfx_clear(C_BG);

    gfx_rrect(GFX_W / 2 - 62, 92, 124, 124, 34, C_ACCENT);
    gfx_icon(IC_LOGO, (float)GFX_W / 2, 154, 104, GRGB(255, 255, 255));
    gfx_text(GFX_W / 2, 262, 40, GW_BOLD, C_TEXT, GA_CENTER, "VitaSend");
    gfx_text(GFX_W / 2, 314, 24, GW_MED, C_ACCENT_HI, GA_CENTER, "Update in progress");
    ui_spinner(GFX_W / 2, 372, 20, ms, C_ACCENT_HI);
    gfx_text(GFX_W / 2, 436, 15, GW_REG, C_DIM, GA_CENTER, "Please wait and do not turn off your Vita.");
    gfx_text(GFX_W / 2, 460, 15, GW_REG, C_DIM, GA_CENTER, "VitaSend starts again by itself.");

    gfx_end();
}

static void screen_thread(void *arg)
{
    (void)arg;
    uint64_t t0 = plat_time_ms();
    while (s_run) {
        draw((unsigned)(plat_time_ms() - t0));
        plat_sleep_ms(30);
    }
}

void helper_screen_start(void)
{
    if (s_run) return;
    ui_init();
    if (!gfx_init()) return;                 /* no memory for a picture: the update still works, the screen just stays dark */
    draw(0);                                 /* the first picture at once, before anything else is done */
    s_run = 1;
    if (!plat_thread_spawn("updscreen", screen_thread, NULL, 128 * 1024)) s_run = 0;
}
