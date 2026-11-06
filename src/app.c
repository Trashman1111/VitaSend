/*
 * app.c - the main loop of the user interface: tab keys, button / touch input, events from the other threads
 * (requests, transfers, installations), the screensaver timer, and the drawing dispatch. The pages and dialogs
 * themselves are in app_*.c (see app_int.h for the map).
 *
 *   Tabs (switch with L / R, or LEFT then UP / DOWN):  Receive | Send | History | Files | Settings
 *   Pages / dialogs on top:    file browser, on-screen keyboard, log, incoming request, transfer, message, confirm, installer
 *
 * Author: trashman (VitaSend)
 */
#include "app_int.h"

app_state A;

static char     s_toast[120];
static uint64_t s_toast_until;

void toast(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_toast, sizeof s_toast, fmt, ap);
    va_end(ap);
    s_toast_until = plat_time_ms() + 3500;
}

/* What a dialog dims behind it. */
void under_tab(void *ctx)
{
    draw_tab(A.tab, ctx);
}

void go(screen_id sc)
{
    /* an event that needs the user (a request, a message, a PIN...) ends the screensaver; a transfer that simply starts, or an
     * installation that starts by itself, does not - the screensaver shows its progress */
    if (A.saver && sc != A.sc && sc != SC_TRANSFER && sc != SC_INSTALL) saver_stop("screen change");
    if (sc >= SC_MESSAGE && A.sc != sc) ui_dialog_invalidate();      /* a dialog dims what is behind it: draw that afresh */
    if (sc == SC_HOME && A.sc <= SC_SETTINGS && A.sc != SC_HOME) A.side_focus = true;      /* the Receive page has nothing to steer at first */
    A.sc = sc;
    if (sc <= SC_SETTINGS) A.tab = sc;
}

static const char *const k_tab_names[TAB_COUNT] = { "Receive", "Send", "History", "Files", "Settings" };

/* The sidebar shows a small card while something is being transferred. */
/* The battery is asked for every few seconds only. */
static int      s_bat = -1;
static bool     s_bat_chg;
uint64_t s_bat_at;
static void battery_refresh(void)
{
    uint64_t now = plat_time_ms();
    if (s_bat_at && now - s_bat_at < 4000) return;
    s_bat_at = now;
    plat_battery(&s_bat, &s_bat_chg);
}

/* The "finished" chime, if the user did not switch it off. */
void chime_if_on(void)
{
    ls_settings cs;
    settings_snapshot(&cs);
    if (cs.sound) plat_chime();
}

void draw_chrome(int tab, const xfer_status *x)
{
    ls_settings hs;
    settings_snapshot(&hs);
    battery_refresh();
    int ch = -1, cm = 0;
    if (hs.show_clock && !plat_local_clock(&ch, &cm)) ch = -1;
    ui_side_info si = { .ip = (hs.hide_ids && A.ip[0]) ? "Address hidden" : A.ip, .battery = hs.show_battery ? s_bat : -1, .charging = s_bat_chg,
                        .clock_h = ch, .clock_m = cm };
    if (xfer_running(x)) {
        si.xfer_title = x->outgoing ? "Sending" : "Receiving";
        si.xfer_peer = x->peer;
        si.xfer_pct = xfer_percent(x);
    }
    ui_draw_chrome(tab, &si);
}

/* ============================================================== drawing / input dispatch */

void draw_tab(int tab, const xfer_status *x)
{
    switch (tab) {
    case TAB_SEND:     send_draw(x); break;
    case TAB_HISTORY:  history_draw(x); break;
    case TAB_FILES:    files_draw(x); break;
    case TAB_SETTINGS: settings_draw(x); break;
    default:           receive_draw(x); break;
    }
}

static void draw_screen(const xfer_status *x)
{
    static const char *const names[] = { "receive", "send", "history", "files", "settings", "browser", "keyboard", "log", "message", "request", "transfer", "confirm", "install", "menu", "pick", "info", "move", "view" };
    gfx_begin();
    ui_hit_reset();
    ui_set_side_focus(A.side_focus && A.sc <= SC_SETTINGS && !A.saver);
    if (A.saver) {
        gfx_note("tab=%s screen=saver pos=%d,%d", k_tab_names[A.tab], (int)A.sx, (int)A.sy);
        saver_draw(x);
        gfx_end();
        return;
    }
    gfx_note("tab=%s screen=%s%s", k_tab_names[A.tab], names[A.sc], (A.side_focus && A.sc <= SC_SETTINGS) ? " sidebar" : "");
    switch (A.sc) {
    case SC_HOME: case SC_SEND: case SC_HISTORY: case SC_FILES: case SC_SETTINGS: draw_tab(A.sc, x); break;
    case SC_BROWSER:  browser_draw(x); break;
    case SC_OSK:      osk_screen_draw(); break;
    case SC_LOG:      log_draw(x); break;
    case SC_MESSAGE:  message_draw(x); break;
    case SC_REQUEST:  request_draw(x); break;
    case SC_TRANSFER: transfer_draw(x); break;
    case SC_CONFIRM:  confirm_draw(x); break;
    case SC_INSTALL:  install_draw(x); break;
    case SC_MENU:     menu_draw(x); break;
    case SC_PICK:     pick_draw(x); break;
    case SC_INFO:     info_draw(x); break;
    case SC_MOVE:     move_draw(x); break;
    case SC_VIEW:     view_draw(x); break;
    }
    if (s_toast[0] && plat_time_ms() < s_toast_until) ui_toast(s_toast);
    gfx_end();
}

static void tab_changed(int tab)
{
    history_reset();
    if (tab == TAB_SETTINGS) settings_reset();
    if (tab == TAB_SEND) discovery_request_announce();
    if (tab == TAB_FILES) { files_ensure(); files_reload(); }
    go((screen_id)tab);
}

/* touch: a tap on a tab in the sidebar */
static void on_tab_tap(int tab)
{
    if (A.sc == (screen_id)tab) return;               /* already there */
    tab_changed(tab);
    A.side_focus = false;                             /* touching: no D-pad focus on the tab list */
}

static void handle_tab_keys(uint32_t p)
{
    if (p & UI_BTN_L) { A.side_focus = false; tab_changed((A.tab + TAB_COUNT - 1) % TAB_COUNT); return; }
    if (p & UI_BTN_R) { A.side_focus = false; tab_changed((A.tab + 1) % TAB_COUNT); return; }
    if (p & UI_BTN_START) {
        xfer_status x;
        xfer_snapshot(&x);
        /* D-pad test: on the Send tab START marks a favorite, unless a transfer is running right now */
        bool fav_start = A.sc == SC_SEND && !xfer_running(&x);
        if (x.state != XF_IDLE && !fav_start) { go(SC_TRANSFER); return; }
    }
    if (A.side_focus) {                                  /* D-pad test: the tab list has the focus */
        if (p & UI_BTN_UP)   tab_changed((A.tab + TAB_COUNT - 1) % TAB_COUNT);
        if (p & UI_BTN_DOWN) tab_changed((A.tab + 1) % TAB_COUNT);
        if ((p & UI_BTN_TRIANGLE) && !A.ip[0]) wifi_user_action();     /* (the hint bar offers it) */
        if (p & (UI_BTN_RIGHT | UI_BTN_CROSS | UI_BTN_CIRCLE)) A.side_focus = false;
        return;
    }
    if (p & UI_BTN_LEFT) { A.side_focus = true; return; }
    p &= ~(uint32_t)UI_BTN_RIGHT;
    switch (A.sc) {
    case SC_HOME:     receive_input(p); break;
    case SC_SEND:     send_input(p); break;
    case SC_HISTORY:  history_input(p); break;
    case SC_FILES:    files_input(p); break;
    case SC_SETTINGS: settings_input(p); break;
    default: break;
    }
}

static void handle_input(uint32_t p)
{
    if (!p) return;
    switch (A.sc) {
    case SC_HOME: case SC_SEND: case SC_HISTORY: case SC_FILES: case SC_SETTINGS: handle_tab_keys(p); break;
    case SC_BROWSER:  browser_input(p); break;
    case SC_OSK:      osk_screen_input(p); break;
    case SC_LOG:      log_input(p); break;
    case SC_MESSAGE:  message_input(p); break;
    case SC_REQUEST:  request_input(p); break;
    case SC_TRANSFER: transfer_input(p); break;
    case SC_CONFIRM:  confirm_input(p); break;
    case SC_INSTALL:  install_input(p); break;
    case SC_MENU:     menu_input(p); break;
    case SC_PICK:     pick_input(p); break;
    case SC_INFO:     info_input(p); break;
    case SC_MOVE:     move_input(p); break;
    case SC_VIEW:     view_input(p); break;
    }
}

/* D-pad buttons repeat while held. After the screensaver was woken, the buttons that are still held do nothing until they have
 * been let go (input_swallow). */
static uint32_t s_in_prev, s_in_blocked;
static uint64_t s_in_since[6], s_in_next[6];

static void input_swallow(void)
{
    s_in_blocked = s_in_prev;
    memset(s_in_since, 0, sizeof s_in_since);
}

static uint32_t read_input(void)
{
    uint32_t b = ui_buttons();
    s_in_blocked &= b;                                   /* let go: free again */
    uint32_t pressed = b & ~s_in_prev & ~s_in_blocked;
    uint64_t now = plat_time_ms();
    static const uint32_t rep[6] = { UI_BTN_UP, UI_BTN_DOWN, UI_BTN_LEFT, UI_BTN_RIGHT, UI_BTN_L, UI_BTN_R };
    for (int i = 0; i < 6; i++) {
        if (!(b & rep[i]) || (s_in_blocked & rep[i])) { s_in_since[i] = 0; continue; }
        if (pressed & rep[i]) { s_in_since[i] = now; s_in_next[i] = now + 380; continue; }
        /* the shoulder buttons switch tabs: no auto-repeat there, it would race through them */
        if (rep[i] == UI_BTN_L || rep[i] == UI_BTN_R) continue;
        if (s_in_since[i] && now >= s_in_next[i]) { pressed |= rep[i]; s_in_next[i] = now + 70; }
    }
    s_in_prev = b;
    return pressed;
}

/* The front touch screen: a tap becomes the button presses of whatever was tapped; a drag scrolls a list. */
static uint32_t read_touch(bool *changed)
{
    int tx = 0, ty = 0;
    bool down = ui_touch(&tx, &ty);
    A.touch_down = down;
    if (A.swallow_touch) {                               /* the finger that woke the screensaver: nothing it does counts */
        if (!down) A.swallow_touch = false;
        touch_reset();
        return 0;
    }
    touch_event te;
    if (!touch_feed(down, tx, ty, &te)) return 0;
    *changed = true;
    switch (te.kind) {
    case TOUCH_TAP:
        A.side_focus = false;
        return ui_touch_tap(te.sx, te.sy);
    case TOUCH_DRAG_BEGIN:
        ui_touch_drag_begin(te.sx, te.sy);
        ui_touch_drag(te.dy);
        break;
    case TOUCH_DRAG:
        ui_touch_drag(te.dy);
        break;
    case TOUCH_DRAG_END:
        ui_touch_drag_end();
        break;
    }
    return 0;
}

/* Things other threads ask the UI to show. */
static void check_events(void)
{
    rx_tick();
    iq_tick();
    move_tick();

    /* no new uploads while a package is being installed */
    if (vpk_busy() && request_pending()) rx_decide(false);

    /* a sender is waiting for our answer */
    if (A.sc != SC_REQUEST && request_pending()) {
        A.ret = (A.sc == SC_OSK) ? osk_return_screen() : A.sc;
        go(SC_REQUEST);
    } else if (A.sc == SC_REQUEST && !request_pending()) {
        go(A.ret);                                   /* it was answered elsewhere or the sender gave up */
    }

    char manual[120];
    if (discovery_take_manual_result(manual, sizeof manual)) toast("%s", manual);

    /* a text message arrived */
    char from[64], text[HIST_TEXT_MAX];
    if (A.sc != SC_OSK && A.sc != SC_REQUEST && A.sc != SC_INSTALL && rx_take_message(from, sizeof from, text, sizeof text)) {
        char title[96];
        snprintf(title, sizeof title, "Message from %s", from);
        show_message(title, text, A.sc == SC_MESSAGE ? A.tab : (A.sc == SC_CONFIRM ? A.tab : A.sc));
    }

    /* a transfer started: show it */
    xfer_status x;
    xfer_snapshot(&x);
    if (x.serial != A.seen_serial && x.state != XF_IDLE && A.sc != SC_OSK && A.sc != SC_REQUEST && A.sc != SC_CONFIRM && A.sc != SC_INSTALL) {
        A.seen_serial = x.serial;
        go(SC_TRANSFER);
    }
    if (A.sc == SC_TRANSFER && x.state == XF_IDLE) go(A.tab);   /* e.g. we rejected the request */

    /* a transfer ended: wake the screensaver (once) - unless received packages are going to be installed automatically, then it
     * is the end of the installation that wakes it. A finished download plays the chime - unless the received packages are
     * going to be installed automatically, then the chime waits for the end of the installation too. */
    static uint32_t chime_xfer, dl_pending;
    static bool saver_hold;                                  /* the screensaver stays until the installation that follows is over */
    bool x_ended = x.state == XF_DONE || x.state == XF_FAILED || x.state == XF_CANCELLED;
    static uint32_t ended_serial;
    if (x_ended && x.serial != ended_serial) {
        ended_serial = x.serial;
        if (A.saver) {
            bool install_follows = false;
            if (x.state == XF_DONE && !x.outgoing && x.files_done > 0) {
                ls_settings ws;
                settings_snapshot(&ws);
                char wv[RX_VPK_MAX_UI][RX_PATH_CAP];
                install_follows = (ws.auto_install == INSTALL_EVERYONE || (ws.auto_install == INSTALL_FAVORITES && rx_received_from_favorite())) &&
                                  rx_received_vpks(wv, RX_VPK_MAX_UI) > 0;
            }
            if (install_follows) saver_hold = true;
            else saver_stop("transfer finished");
        }
    }
    if (x.state == XF_DONE && !x.outgoing && x.serial != chime_xfer) {
        chime_xfer = x.serial;
        if (x.files_done > 0) {
            ls_settings cs;
            settings_snapshot(&cs);
            char cvp[RX_VPK_MAX_UI][RX_PATH_CAP];
            if (cs.auto_install != INSTALL_ASK && rx_received_vpks(cvp, RX_VPK_MAX_UI) > 0) dl_pending = x.serial;   /* decided below */
            else chime_if_on();
        }
    }
    vpk_status cvs;
    vpk_snapshot(&cvs);
    vpk_chime_events(&cvs);
    /* an installation ended (all of a queue): wake the screensaver, once per installation. Two independent triggers, so a missed
     * edge can never leave the screensaver on: the finished state of a new serial, and the busy -> idle change. */
    static uint32_t woke_vpk;
    static bool inst_was_busy;
    bool inst_busy = cvs.state == VPK_RUNNING || iq_running();
    bool inst_end_state = cvs.state == VPK_DONE || cvs.state == VPK_FAILED || cvs.state == VPK_CANCELLED;
    if (inst_busy != inst_was_busy) ls_log("installation %s", inst_busy ? "running" : "idle");
    if (!inst_busy && ((inst_end_state && cvs.serial != woke_vpk) || inst_was_busy)) {
        woke_vpk = cvs.serial;
        ui_input_refresh();                                  /* the system installer may have reset the input sampling */
        saver_stop("installation finished");
        saver_hold = false;
    }
    inst_was_busy = inst_busy;

    /* a finished receive brought .vpk files: install them without asking, if the user switched that on */
    static uint32_t auto_serial;
    if (x.state == XF_DONE && !x.outgoing && x.serial != auto_serial && x.serial == A.seen_serial &&
        A.sc != SC_OSK && A.sc != SC_REQUEST && A.sc != SC_CONFIRM && A.sc != SC_INSTALL && !vpk_busy()) {
        auto_serial = x.serial;
        bool started = false;
        ls_settings as;
        settings_snapshot(&as);
        if (as.auto_install == INSTALL_EVERYONE || (as.auto_install == INSTALL_FAVORITES && rx_received_from_favorite())) {
            char vp[RX_VPK_MAX_UI][RX_PATH_CAP];
            int n = rx_received_vpks(vp, RX_VPK_MAX_UI);
            if (n > 0) {
                const char *pp[RX_VPK_MAX_UI];
                for (int i = 0; i < n; i++) pp[i] = vp[i];
                ls_log("auto-install: %d package(s) received from %s", n, as.auto_install == INSTALL_EVERYONE ? "anyone" : "a favorite");
                iq_open(pp, n, (screen_id)A.tab, true);
                started = vpk_busy() || iq_active();
            }
        }
        if (dl_pending == x.serial) {                    /* nothing is going to be installed after all: announce the download now */
            dl_pending = 0;
            if (!started) chime_if_on();
        }
    }

    /* the screensaver was waiting for an automatic installation that never started (or is over already) */
    if (saver_hold && !vpk_busy() && !iq_running()) {
        saver_hold = false;
        saver_stop("transfer finished");
    }

    /* the other device wants a PIN */
    if (!send_needs_pin()) A.pin_answered = false;
    else if (!A.pin_answered && !osk_is_pin_prompt()) {
        open_osk(OP_SEND_PIN, "The other device needs a PIN", "", SET_PIN_MAX, true, SC_TRANSFER);
    }
}

void app_run(const app_opts *opts)
{
    memset(&A, 0, sizeof A);
    A.sc = SC_HOME;
    A.tab = SC_HOME;
    A.ret = SC_HOME;
    A.side_focus = true;                            /* the first page has nothing the D-pad could steer: start in the tab list */
    A.server_ok = opts->server_ok;
    bool services_pending = opts->start_services != NULL;     /* "Continue without Wi-Fi": the server waits for the network */
    {
        ls_settings st0;
        settings_snapshot(&st0);
        ui_set_theme(st0.theme);
    }
    plat_net_local_ip(A.ip);
    ui_set_tab_handler(on_tab_tap);

    xfer_status x0;
    xfer_snapshot(&x0);
    A.seen_serial = x0.serial;

    uint64_t t_net = 0, t_awake = 0, t_draw = 0, t0 = plat_time_ms();
    A.last_input = t0;
    uint32_t last_log = ls_log_counter();
    screen_id last_sc = (screen_id)-1;
    bool last_saver = false;
    bool dirty = true;

    while (!g_quit && !A.quit) {
        uint64_t now = plat_time_ms();

        uint32_t p = read_input();
        bool touched = false;
        p |= read_touch(&touched);
        if (p || touched || A.touch_down) A.last_input = now;
        if (A.saver && (p || touched || A.touch_down)) {         /* the press or touch that wakes it does nothing else */
            saver_stop(p ? "button" : "touch");
            input_swallow();
            A.swallow_touch = A.touch_down || touched;
            p = 0;
            dirty = true;
        }
        if (p || touched) dirty = true;
        handle_input(p);
        check_events();

        /* idle for long enough: start the screensaver (not while a question waits for the user, or while typing) */
        {
            ls_settings ss;
            settings_snapshot(&ss);
            if (!A.saver && ss.screensaver > 0 && A.sc != SC_REQUEST && A.sc != SC_OSK && A.sc != SC_VIEW && !move_busy() &&
                now > A.last_input && now - A.last_input > (uint64_t)ss.screensaver * idle_unit_ms()) {   /* (the wake-up stamps last_input AFTER `now` was read: never subtract a newer time) */
                saver_start();
                dirty = true;
            }
            if (A.saver && !ss.screensaver) { saver_stop("switched off"); dirty = true; }         /* switched off meanwhile */
        }
        if (A.saver) saver_step();

        if (now - t_net > (wifi_get() == WIFI_CONNECTING ? 500u : 2000u)) {   /* Wi-Fi came / went / changed address? */
            t_net = now;
            char cur[16] = "";
            plat_net_local_ip(cur);
            if (wifi_poll(cur[0] != 0)) dirty = true;
            if (strcmp(cur, A.ip) != 0) {
                ls_strlcpy(A.ip, cur, sizeof A.ip);
                if (cur[0]) {
                    ls_log("Wi-Fi address is now %s", cur);
                    if (services_pending) {
                        services_pending = false;
                        A.server_ok = opts->start_services();
                    } else {
                        discovery_network_changed();
                    }
                } else {
                    ls_log("Wi-Fi lost");
                }
                dirty = true;
            }
        }
        if (now - t_awake > 8000) {
            t_awake = now;
            plat_keep_awake();
            ui_input_refresh();
        }

        uint32_t lc = ls_log_counter();
        bool fast = A.sc == SC_TRANSFER || A.sc == SC_REQUEST || A.sc == SC_INSTALL || A.sc == SC_MOVE || (A.sc == SC_VIEW && view_loading()) || xfer_busy() || wifi_get() == WIFI_CONNECTING;
        if (lc != last_log) { last_log = lc; if (A.sc == SC_LOG || A.sc == SC_HOME) dirty = true; }
        if (A.sc != last_sc) { last_sc = A.sc; dirty = true; }
        if (A.saver != last_saver) { last_saver = A.saver; dirty = true; }
        if (A.saver) fast = true;
        if (dirty || now - t_draw > (A.saver ? 33u : (fast ? 120u : 500u))) {
            xfer_status x;
            xfer_snapshot(&x);
            draw_screen(&x);
            t_draw = now;
            dirty = false;
        }
        if (opts->run_seconds && now - t0 > opts->run_seconds * 1000u) break;
        plat_sleep_ms(30);
    }
}
