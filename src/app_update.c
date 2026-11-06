/*
 * app_update.c - the update dialogs: looking for a new version (at start, or from Settings > About), asking, downloading
 *
 * The work itself is update.c (a thread); this file shows its state and decides when to ask: at start-up the check runs by
 * itself (Settings > Check for updates at start), a new version is offered at most once a day, and never while something else
 * is going on (a transfer, a question, another dialog).
 *
 * Author: trashman (VitaSend)
 */
#include "app_int.h"
#include "update.h"

static screen_id s_ret = SC_SETTINGS;           /* where the dialog goes back to */
static bool      s_offered;                     /* the automatic question has been shown during this run */
static bool      s_auto_started;
static uint64_t  s_ip_since;                    /* when the Wi-Fi address appeared */

static unsigned auto_delay_ms(void)
{
#ifdef LS_HOST
    const char *e = getenv("LS_UPDATE_DELAY_MS");          /* tests */
    if (e && *e) return (unsigned)atoi(e);
#endif
    return 4000;                                           /* let the app settle (and Wi-Fi after waking up) first */
}

void upd_ui_open(bool manual, screen_id ret)
{
    upd_status us;
    update_snapshot(&us);
    s_ret = ret;
    if (us.state != UPD_CHECKING && us.state != UPD_AVAILABLE && us.state != UPD_DOWNLOADING && us.state != UPD_READY) {
        update_reset();
        if (!update_check(manual)) toast("Cannot start the check");
    }
    go(SC_UPDATE);
}

static void leave(void)
{
    upd_status us;
    update_snapshot(&us);
    if (us.state == UPD_UPTODATE || us.state == UPD_FAILED || us.state == UPD_READY) update_reset();
    go(s_ret);
}

static bool can_interrupt(void)
{
    return A.sc >= SC_HOME && A.sc <= SC_SETTINGS && !A.saver && !xfer_busy() && !request_pending() && !vpk_busy() && !iq_active() && !move_busy() && !delete_busy() && !unzip_ui_busy();
}

/* Called from the main loop. */
void upd_ui_tick(uint64_t now)
{
    upd_status us;
    update_snapshot(&us);

    /* the automatic check, a few seconds after the network is there */
    if (!s_auto_started) {
        if (!A.ip[0]) s_ip_since = 0;
        else {
            if (!s_ip_since) s_ip_since = now;
            if (now - s_ip_since >= auto_delay_ms()) {
                s_auto_started = true;
                ls_settings st;
                settings_snapshot(&st);
                if (st.check_updates) update_check(false);
                else ls_log("Update: the check at start is switched off");
            }
        }
    }

    /* the answer to an automatic check: silent unless a new version exists and today's question has not been asked yet */
    if (!us.manual && A.sc != SC_UPDATE) {
        if (us.state == UPD_UPTODATE || us.state == UPD_FAILED) update_reset();
        else if (us.state == UPD_AVAILABLE && !s_offered && can_interrupt()) {
            ls_settings st;
            settings_snapshot(&st);
            int today = 0;
            bool known = plat_local_date(&today);
            if (known && st.update_asked_day == today) {
                s_offered = true;
                ls_log("Update: %s is available (the question was already asked today)", us.tag);
            } else {
                s_offered = true;
                if (known) settings_set_update_asked_day(today);
                s_ret = A.sc;
                go(SC_UPDATE);
            }
        }
    }

    /* downloaded and checked: on to the installer */
    if (us.state == UPD_READY && A.sc == SC_UPDATE) {
        char file[320];
        ls_strlcpy(file, us.file, sizeof file);
        update_reset();
        install_open_update(file, s_ret);
    }
}

static void fmt_mb(uint64_t b, char *out, size_t cap)
{
    snprintf(out, cap, "%.1f MB", (double)b / (1024.0 * 1024.0));
}

void upd_ui_draw(const xfer_status *x)
{
    upd_status us;
    update_snapshot(&us);
    int dx, dy;
    ui_dialog_begin(SC_UPDATE | (A.tab << 8), 700, 340, under_tab, (void *)x, &dx, &dy);
    int lx = dx + 32, w = 700 - 64;
    char line[200];

    gcol tint = C_ACCENT_BG;
    int icon = IC_REFRESH;
    gcol icol = C_ACCENT_HI;
    const char *title = "Update";
    if (us.state == UPD_UPTODATE) { title = "VitaSend is up to date"; icon = IC_CHECK; icol = C_GOOD; tint = GRGBA(102, 187, 106, 40); }
    else if (us.state == UPD_FAILED) { title = "Update check failed"; icon = IC_CLOSE; icol = C_BAD; tint = GRGBA(239, 83, 80, 40); if (us.was_download) title = "Download failed"; }
    else if (us.state == UPD_AVAILABLE) { title = "A new version is available"; icon = IC_PACKAGE; }
    else if (us.state == UPD_DOWNLOADING || us.state == UPD_READY) { title = "Downloading the update"; icon = IC_ARROW_DOWN; }
    else if (us.state == UPD_CHECKING) { title = "Looking for updates"; }
    gfx_circle((float)(lx + 28), (float)(dy + 52), 28, tint);
    gfx_icon(icon, (float)(lx + 28), (float)(dy + 52), 30, icol);
    gfx_text(lx + 72, dy + 40, 22, GW_BOLD, C_TEXT, GA_LEFT, title);
    gfx_text_fit(lx + 72, dy + 68, w - 72, 15, GW_REG, C_DIM, GA_LEFT, "From GitHub: Trashman1111/vitasend");

    switch (us.state) {
    case UPD_CHECKING:
    case UPD_IDLE:
        gfx_text_fit(lx, dy + 140, w, 17, GW_MED, C_TEXT, GA_LEFT, "Asking GitHub for the newest version...");
        ui_spinner(lx + w - 30, dy + 150, 14, (unsigned)plat_time_ms(), C_ACCENT);
        ui_action(lx, dy + 280, PB_CIRCLE, "Cancel");
        ui_hints("O Cancel");
        break;
    case UPD_UPTODATE:
        snprintf(line, sizeof line, "You have version %s. The newest release is %s.", us.current, us.tag[0] ? us.tag : us.current);
        gfx_text_fit(lx, dy + 140, w, 17, GW_MED, C_TEXT, GA_LEFT, line);
        ui_action(lx, dy + 280, PB_CROSS, "Close");
        ui_hints("X Close");
        break;
    case UPD_AVAILABLE: {
        char sz[24];
        fmt_mb(us.size, sz, sizeof sz);
        snprintf(line, sizeof line, "VitaSend %s%s  (you have %s)", us.tag, us.prerelease ? "  \xC2\xB7  pre-release" : "", us.current);
        gfx_text_fit(lx, dy + 128, w, 22, GW_BOLD, C_TEXT, GA_LEFT, line);
        snprintf(line, sizeof line, "Download %s, check it, then install it over this version.", us.size ? sz : "the package");
        gfx_text_fit(lx, dy + 160, w, 15, GW_REG, C_DIM, GA_LEFT, line);
        gfx_text_fit(lx, dy + 186, w, 15, GW_REG, C_DIM, GA_LEFT, "Your settings and received files stay.");
        if (us.prerelease) gfx_text_fit(lx, dy + 216, w, 14, GW_MED, C_WARN, GA_LEFT, "A pre-release may still have problems.");
        int w1 = ui_action_width(PB_CROSS, "Download and install");
        ui_action(lx, dy + 280, PB_CROSS, "Download and install");
        ui_action(lx + w1 + 10, dy + 280, PB_CIRCLE, "Not now");
        ui_hints("X Download and install|O Not now");
        break;
    }
    case UPD_DOWNLOADING:
    case UPD_READY: {
        char a[24], b[24];
        fmt_mb(us.done, a, sizeof a);
        fmt_mb(us.size, b, sizeof b);
        snprintf(line, sizeof line, "VitaSend %s", us.tag);
        gfx_text_fit(lx, dy + 128, w, 20, GW_BOLD, C_TEXT, GA_LEFT, line);
        snprintf(line, sizeof line, us.state == UPD_READY ? "Done - checking the package..." : "%s of %s", a, us.size ? b : "?");
        gfx_text_fit(lx, dy + 160, w, 15, GW_REG, C_DIM, GA_LEFT, line);
        ui_progress(lx, dy + 196, w - 70, 12, us.done, us.size ? us.size : 1, C_ACCENT);
        snprintf(line, sizeof line, "%d %%", us.size ? (int)(us.done * 100 / us.size) : 0);
        gfx_text(lx + w, dy + 202, 17, GW_MED, C_TEXT, GA_RIGHT, line);
        ui_action(lx, dy + 280, PB_CIRCLE, "Cancel");
        ui_hints("O Cancel");
        break;
    }
    case UPD_FAILED:
        gfx_text_fit(lx, dy + 140, w, 16, GW_MED, C_BAD, GA_LEFT, us.err);
        {
            int w1 = ui_action_width(PB_CROSS, "Close");
            ui_action(lx, dy + 280, PB_CROSS, "Close");
            ui_action(lx + w1 + 10, dy + 280, PB_SQUARE, "Try again");
            ui_hints("X Close|SQ Try again");
        }
        break;
    }
}

void upd_ui_input(uint32_t p)
{
    upd_status us;
    update_snapshot(&us);
    switch (us.state) {
    case UPD_CHECKING:
    case UPD_IDLE:
        if (p & UI_BTN_CIRCLE) { update_cancel(); leave(); }
        break;
    case UPD_UPTODATE:
        if (p & (UI_BTN_CROSS | UI_BTN_CIRCLE)) leave();
        break;
    case UPD_AVAILABLE:
        if (p & UI_BTN_CIRCLE) leave();
        if (p & UI_BTN_CROSS) { if (!update_download()) toast("Cannot start the download"); }
        break;
    case UPD_DOWNLOADING:
        if (p & UI_BTN_CIRCLE) update_cancel();
        break;
    case UPD_READY:
        break;                                  /* upd_ui_tick() moves on to the installer */
    case UPD_FAILED:
        if (p & (UI_BTN_CROSS | UI_BTN_CIRCLE)) leave();
        if (p & UI_BTN_SQUARE) {
            if (us.was_download) { if (!update_download()) toast("Cannot start the download"); }
            else { update_reset(); update_check(true); }
        }
        break;
    }
}

bool upd_ui_busy(void)
{
    upd_status us;
    update_snapshot(&us);
    return us.state == UPD_CHECKING || us.state == UPD_DOWNLOADING;
}
