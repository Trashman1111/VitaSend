/*
 * app_pages.c - the Receive, Send and History tabs (and the "This device" details)
 *
 * Author: trashman (VitaSend)
 */
#include "app_int.h"

/* ---- state of the Send and History tabs ---- */
static ui_cursor s_pcur;
static ui_cursor s_hcur;
char s_text_to_send[OSK_MAX + 1];            /* a text message waiting to be sent ("" = none) - with s_sel / s_nsel: what will be sent */

void history_reset(void) { memset(&s_hcur, 0, sizeof s_hcur); }
void send_page_reset(void) { memset(&s_pcur, 0, sizeof s_pcur); }

/* The text typed on the keyboard becomes what the Send page will send (and the files selected so far are forgotten). */
void send_set_text(const char *text)
{
    ls_strlcpy(s_text_to_send, text, sizeof s_text_to_send);
    s_nsel = 0;
    send_page_reset();
}


/* ============================================================== receive tab */

const char *accept_label(int m)
{
    return m == AUTO_EVERYONE ? "Everyone" : m == AUTO_FAVORITES ? "Favorites" : "Off";
}

const char *accept_help(int m)
{
    return m == AUTO_EVERYONE ? "Accept everything without asking"
         : m == AUTO_FAVORITES ? "Favorites go through, others are asked"
         : "Ask every time";
}

#define RECENT_ROWS 5

void receive_draw(const xfer_status *x)
{
    draw_chrome(TAB_RECEIVE, x);
    ui_page_title("Receive", NULL);

    /* ---- this device (the one thing on this page the D-pad can select: X opens the details) ---- */
    bool dev_focus = !A.side_focus;
    ui_card(CONTENT_X, CONTENT_Y, CONTENT_W, 150, dev_focus);
    ui_hit_keys(CONTENT_X, CONTENT_Y, CONTENT_W, 150, UI_BTN_CROSS);
    if (dev_focus) gfx_focus(true);
    int lx = CONTENT_X + 24;
    gfx_rrect(lx, CONTENT_Y + 24, 76, 76, 20, C_ACCENT_BG);
    ui_device_icon(g_id.device_type, g_id.device_model, (float)(lx + 38), (float)(CONTENT_Y + 62), strstr(g_id.device_model, "Vita") ? 60 : 50, C_ACCENT_HI);
    int tx = lx + 100, avail = CONTENT_R - 24 - tx;

    ls_settings hid;
    settings_snapshot(&hid);
    bool hide_ids = hid.hide_ids;
    const char *chip = "Ready";
    gcol chip_col = C_GOOD;
    char status[200] = "";
    gcol status_col = C_DIM;
    bool wifi_btn = false, wifi_spin = false;
    const char *wifi_btn_label = "Turn on Wi-Fi";
    if (!A.server_ok) {
        chip = "Server error"; chip_col = C_BAD;
        snprintf(status, sizeof status, "Server not running: %s", plat_net_error_text());
        status_col = C_BAD;
    } else if (!A.ip[0]) {
        /* no popup: the card itself says it, and offers the switch (the same one as the start-up question) */
        wifi_state ws = wifi_get();
        chip = ws == WIFI_CONNECTING ? "Connecting" : "No Wi-Fi";
        chip_col = C_WARN;
        snprintf(status, sizeof status, "%s", ws == WIFI_CONNECTING ? "Turning on Wi-Fi..." :
                 (ws == WIFI_FAILED ? "Not possible here - use Settings > Network" : "Wi-Fi not connected"));
        status_col = C_WARN;
        wifi_btn = ws != WIFI_CONNECTING;
        wifi_btn_label = ws == WIFI_FAILED ? "Wi-Fi settings" : "Turn on Wi-Fi";
        wifi_spin = ws == WIFI_CONNECTING;
    } else {
        if (xfer_running(x)) { chip = x->outgoing ? "Sending" : "Receiving"; chip_col = C_ACCENT_HI; }
        if (hide_ids) snprintf(status, sizeof status, "Address hidden \xC2\xB7 %s", g_id.https ? "https" : "http");
        else snprintf(status, sizeof status, "%s:%u \xC2\xB7 %s", A.ip, g_id.port, g_id.https ? "https" : "http");
    }
    int cw = ui_chip(CONTENT_R - 22, CONTENT_Y + 24, chip, chip_col);
    gfx_text(tx, CONTENT_Y + 24, 13, GW_REG, C_DIM, GA_LEFT, "This device");
    gfx_text_fit(tx, CONTENT_Y + 54, avail - cw, 24, GW_BOLD, C_TEXT, GA_LEFT, g_id.alias);
    char sub[100];
    snprintf(sub, sizeof sub, "%s \xC2\xB7 %s", g_id.device_model, g_id.device_type);
    gfx_text_fit(tx, CONTENT_Y + 80, avail, 15, GW_REG, C_DIM, GA_LEFT, sub);
    int sw = avail;
    if (wifi_btn) {
        int bw = ui_action_width(PB_TRIANGLE, wifi_btn_label);
        ui_action(CONTENT_R - 22 - bw, CONTENT_Y + 102, PB_TRIANGLE, wifi_btn_label);
        sw = avail - bw - 12;
    } else if (wifi_spin) {
        ui_spinner(CONTENT_R - 22 - 14, CONTENT_Y + 102, 10, (unsigned)plat_time_ms(), C_WARN);
        sw = avail - 40;
    }
    gfx_text_fit(tx, CONTENT_Y + 102, sw, 15, GW_MED, status_col, GA_LEFT, status);
    gfx_text(lx, CONTENT_Y + 132, 13, GW_REG, C_FAINT, GA_LEFT, "Fingerprint");
    /* (the fingerprint is cut short when the "Press X for details" hint needs the room) */
    gfx_text_fit(lx + 92, CONTENT_Y + 132, CONTENT_R - 24 - (lx + 92) - (dev_focus ? 132 : 0), 13, GW_REG, C_DIM, GA_LEFT,
                 hide_ids ? "Hidden (Settings > Hide IP address and fingerprint)" : g_id.fingerprint);
    if (dev_focus) {
        gfx_text(CONTENT_R - 24, CONTENT_Y + 132, 13, GW_REG, C_FAINT, GA_RIGHT, "Press X for details");
        gfx_focus(false);
    }

    /* ---- recent activity ---- */
    int ry = CONTENT_Y + 164;
    ui_card(CONTENT_X, ry, CONTENT_W, CONTENT_B - ry, false);
    gfx_text(lx, ry + 22, 15, GW_MED, C_DIM, GA_LEFT, "Recent activity");
    int n = history_count();
    if (n == 0) {
        gfx_text(lx, ry + 62, 15, GW_REG, C_FAINT, GA_LEFT, "Nothing yet - files you send or receive show up here");
        gfx_text(lx, ry + 94, 14, GW_REG, C_FAINT, GA_LEFT, "To receive: open LocalSend on another device on the same Wi-Fi and send to this Vita.");
        gfx_text(lx, ry + 118, 14, GW_REG, C_FAINT, GA_LEFT, "To send: switch to the Send tab (R), choose files and pick a device.");
    }
    for (int i = 0; i < RECENT_ROWS && i < n; i++) {
        hist_entry h;
        if (!history_get(i, &h)) break;
        int cy = ry + 52 + i * 32;
        gcol col = h.status == H_OK ? C_ACCENT_HI : (h.status == H_FAILED ? C_BAD : C_WARN);
        gfx_circle((float)(lx + 12), (float)cy, 12, GRGBA(255, 255, 255, 14));
        gfx_icon(h.outgoing ? IC_ARROW_UP : IC_ARROW_DOWN, (float)(lx + 12), (float)cy, 18, col);
        char what[220], when[24];
        if (h.is_text) snprintf(what, sizeof what, "%s \xC2\xB7 message: %s", h.peer, h.summary);
        else snprintf(what, sizeof what, "%s \xC2\xB7 %s", h.peer, h.summary);
        fmt_clock(h.time, when, sizeof when);
        int wt = gfx_text(CONTENT_R - 24, cy, 13, GW_REG, C_FAINT, GA_RIGHT, when);
        gfx_text_fit(lx + 36, cy, CONTENT_W - 100 - wt, 15, GW_REG, h.status == H_OK ? C_TEXT : col, GA_LEFT, what);
    }

    if (!A.ip[0]) ui_hints(wifi_get() == WIFI_CONNECTING ? "X Device info|LR Tabs|O Quit" : (wifi_get() == WIFI_FAILED ? "X Device info|TRI Wi-Fi settings|LR Tabs|O Quit" : "X Device info|TRI Turn on Wi-Fi|LR Tabs|O Quit"));
    else ui_hints(xfer_running(x) ? "X Device info|TRI Announce|START Transfer|LR Tabs|O Quit"
                                  : "X Device info|TRI Announce|LR Tabs|O Quit");
}

void receive_input(uint32_t p)
{
    if (p & UI_BTN_CROSS) { go(SC_INFO); return; }
    if (p & UI_BTN_TRIANGLE) {
        if (!A.ip[0]) wifi_user_action();                 /* without Wi-Fi there is nobody to announce to: switch it on instead */
        else { discovery_request_announce(); toast("Announcing..."); }
    }
    if (p & UI_BTN_CIRCLE) confirm_open(CF_QUIT, SC_HOME);
}

/* ---- "This device": everything there is to know about this device, in one list (X on the Receive page) ---- */

static void info_row(int x, int *y, int w, const char *label, gcol col, const char *fmt, ...)
{
    char v[260];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v, sizeof v, fmt, ap);
    va_end(ap);
    gfx_text(x, *y, 13, GW_REG, C_DIM, GA_LEFT, label);
    gfx_text_fit(x + 130, *y, w - 130, 15, GW_MED, col, GA_LEFT, v);
    *y += 28;
}

void info_draw(const xfer_status *x)
{
    int dx, dy;
    ui_dialog_begin(SC_INFO | (A.tab << 8), 760, 452, under_tab, (void *)x, &dx, &dy);
    ls_settings st;
    settings_snapshot(&st);
    int lx = dx + 32, w = 760 - 64;
    gfx_circle((float)(lx + 24), (float)(dy + 44), 24, C_ACCENT_BG);
    ui_device_icon(g_id.device_type, g_id.device_model, (float)(lx + 24), (float)(dy + 44), 28, C_ACCENT_HI);
    gfx_text(lx + 62, dy + 36, 22, GW_BOLD, C_TEXT, GA_LEFT, "This device");
    gfx_text_fit(lx + 62, dy + 62, w - 62, 14, GW_REG, C_DIM, GA_LEFT, "What the other LocalSend devices see - and how this one is set up");
    gfx_line((float)(dx + 28), (float)(dy + 84), (float)(dx + 732), (float)(dy + 84), 1, C_BORDER);

    int y = dy + 108;
    info_row(lx, &y, w, "Name", C_TEXT, "%s", g_id.alias);
    info_row(lx, &y, w, "Device", C_TEXT, "%s  \xC2\xB7  %s", g_id.device_model, g_id.device_type);
    if (!A.server_ok) info_row(lx, &y, w, "Network", C_BAD, "Server not running: %s", plat_net_error_text());
    else if (!A.ip[0]) info_row(lx, &y, w, "Network", C_WARN, "Wi-Fi not connected");
    else if (st.hide_ids) info_row(lx, &y, w, "Network", C_TEXT, "Address hidden  \xC2\xB7  port %u  \xC2\xB7  %s", g_id.port, g_id.https ? "https" : "http");
    else info_row(lx, &y, w, "Network", C_TEXT, "%s : %u  \xC2\xB7  %s", A.ip, g_id.port, g_id.https ? "https (encrypted)" : "http");
    info_row(lx, &y, w, "Protocol", C_TEXT, "LocalSend v%s  \xC2\xB7  find devices on 224.0.0.167", LS_PROTO_VERSION);
    if (st.hide_ids) info_row(lx, &y, w, "Fingerprint", C_DIM, "Hidden (Settings > Hide IP address and fingerprint)");
    else {
        gfx_text(lx, y, 13, GW_REG, C_DIM, GA_LEFT, "Fingerprint");
        gfx_text(lx + 130, y - 8, 12, GW_REG, C_TEXT, GA_LEFT, g_id.fingerprint);
        char half[40];
        memcpy(half, g_id.fingerprint + 32, 32);
        half[32] = 0;
        gfx_text(lx + 130, y + 8, 12, GW_REG, C_TEXT, GA_LEFT, half);
        y += 36;
    }
    char free_s[24] = "";
    uint64_t fr = plat_free_space(st.receive_dir);
    if (fr != UINT64_MAX) { fmt_size(fr, free_s, sizeof free_s); }
    if (free_s[0]) info_row(lx, &y, w, "Receives into", C_TEXT, "%s  \xC2\xB7  %s free", st.receive_dir, free_s);
    else info_row(lx, &y, w, "Receives into", C_TEXT, "%s", st.receive_dir);
    info_row(lx, &y, w, "Quick Save", C_TEXT, "%s  \xC2\xB7  PIN: %s", accept_label(st.auto_accept), st.pin[0] ? "set" : "off");
    info_row(lx, &y, w, "Install .vpk", st.auto_install ? C_WARN : C_TEXT, "%s", st.auto_install == INSTALL_FAVORITES ? "Favorites only - installed without asking"
             : st.auto_install == INSTALL_EVERYONE ? "Everyone - installed without asking" : "Off - always asks first");
    info_row(lx, &y, w, "App", C_TEXT, "VitaSend v%s  \xC2\xB7  by trashman  \xC2\xB7  %s", LS_APP_VERSION, plat_own_title_id());

    ui_action(lx, dy + 452 - 48, PB_CROSS, "Close");
    ui_hints("X Close");
}

void info_input(uint32_t p)
{
    if (p & (UI_BTN_CROSS | UI_BTN_CIRCLE)) go(SC_HOME);
}

/* ============================================================== send tab */

static int picker_peers(ls_peer *out)
{
    ls_peer all[MAX_PEERS];
    int n = peers_snapshot(all, MAX_PEERS), k = 0;
    for (int pass = 0; pass < 2; pass++)                  /* favourites first, then the rest (each newest first) */
        for (int i = 0; i < n; i++)
            if (settings_is_fav(all[i].fingerprint) == (pass == 0)) out[k++] = all[i];
    return k;
}

#define PEER_TOP  226
#define PEER_ROWS 4
#define PEER_STEP 66

static void peer_star_tap(void *ctx, int arg)
{
    (void)ctx;
    ls_peer pv[MAX_PEERS];
    int n = picker_peers(pv);
    if (arg < 0 || arg >= n) return;
    bool now = settings_toggle_fav(pv[arg].fingerprint, pv[arg].alias);
    toast(now ? "%s is now a favorite" : "%s is no longer a favorite", pv[arg].alias);
}

void send_draw(const xfer_status *x)
{
    draw_chrome(TAB_SEND, x);
    ui_page_title("Send", NULL);

    /* ---- what will be sent ---- */
    ui_card(CONTENT_X, CONTENT_Y, CONTENT_W, 92, false);
    int lx = CONTENT_X + 22;
    bool text = s_nsel == 0 && s_text_to_send[0];
    int icon = text ? IC_MESSAGE : (s_nsel > 0 ? IC_FILE : IC_PLUS);
    gfx_rrect(lx, CONTENT_Y + 18, 56, 56, 16, C_ACCENT_BG);
    gfx_icon(icon, (float)(lx + 28), (float)(CONTENT_Y + 46), 32, C_ACCENT_HI);
    int bw1 = ui_action_width(PB_SQUARE, "Files"), bw2 = ui_action_width(PB_TRIANGLE, "Text");
    int bx = CONTENT_R - 20 - bw1 - 10 - bw2;
    ui_action(bx, CONTENT_Y + 46, PB_SQUARE, "Files");
    ui_action(bx + bw1 + 10, CONTENT_Y + 46, PB_TRIANGLE, "Text");
    int tx = lx + 76, avail = bx - tx - 14;
    char l1[120], l2[320] = "";
    if (text) {
        snprintf(l1, sizeof l1, "Text message");
        ls_strlcpy(l2, s_text_to_send, sizeof l2);
    } else if (s_nsel > 0) {
        snprintf(l1, sizeof l1, "%d item%s selected", s_nsel, s_nsel == 1 ? "" : "s");
        snprintf(l2, sizeof l2, "%s%s", base_name(s_sel[0]), s_nsel > 1 ? " and more" : "");
    } else {
        snprintf(l1, sizeof l1, "Nothing selected yet");
        snprintf(l2, sizeof l2, "Choose files or folders, or write a message");
    }
    gfx_text_fit(tx, CONTENT_Y + 33, avail, 18, GW_MED, C_TEXT, GA_LEFT, l1);
    gfx_text_fit(tx, CONTENT_Y + 59, avail, 14, GW_REG, C_DIM, GA_LEFT, l2);

    /* ---- nearby devices ---- */
    gfx_text(CONTENT_X + 4, 204, 18, GW_MED, C_TEXT, GA_LEFT, "Nearby devices");
    ls_peer pv[MAX_PEERS];
    int n = picker_peers(pv);
    if (n == 0) {
        gfx_icon(IC_WIFI, (float)(CONTENT_X + CONTENT_W / 2), 316, 56, C_FAINT);
        gfx_text(CONTENT_X + CONTENT_W / 2, 366, 18, GW_MED, C_WARN, GA_CENTER, "No devices found yet");
        gfx_text(CONTENT_X + CONTENT_W / 2, 394, 15, GW_REG, C_DIM, GA_CENTER, "Open LocalSend on the other device (same Wi-Fi).");
        gfx_text(CONTENT_X + CONTENT_W / 2, 416, 15, GW_REG, C_DIM, GA_CENTER, "Not showing up? Press Select and type its IP address.");
    } else {
        ui_cursor_clamp(&s_pcur, n, PEER_ROWS);
        uint64_t now = plat_time_ms();
        for (int i = 0; i < PEER_ROWS && s_pcur.top + i < n; i++) {
            int idx = s_pcur.top + i, y = PEER_TOP + i * PEER_STEP, cy = y + 29;
            bool cur = idx == s_pcur.sel;
            const ls_peer *pe = &pv[idx];
            ui_card(CONTENT_X, y, CONTENT_W - 12, 58, cur);
            ui_hit_row(CONTENT_X, y - 4, CONTENT_W - 12, PEER_STEP, &s_pcur, idx, UI_BTN_CROSS);
            if (cur) gfx_focus(true);
            gfx_circle((float)(CONTENT_X + 38), (float)cy, 21, C_ACCENT_BG);
            ui_device_icon(pe->type, pe->model, (float)(CONTENT_X + 38), (float)cy, 26, C_ACCENT_HI);
            unsigned age = now > pe->last_seen_ms ? (unsigned)((now - pe->last_seen_ms) / 1000) : 0;
            bool fav = settings_is_fav(pe->fingerprint);
            char sub[160], seen[24];
            snprintf(sub, sizeof sub, "%s:%u \xC2\xB7 %s \xC2\xB7 %s", pe->ip, pe->port, pe->model[0] ? pe->model : pe->type, pe->https ? "https" : "http");
            snprintf(seen, sizeof seen, "%us ago", age);
            int rx = CONTENT_R - 12 - 22;
            ui_hit_call(rx - 8 - 28, y - 4, 56, PEER_STEP, peer_star_tap, NULL, idx);
            if (fav) { gfx_icon(IC_STAR, (float)(rx - 8), (float)cy, 22, C_WARN); rx -= 30; }
            else { gfx_icon(IC_STAR_OUTLINE, (float)(rx - 8), (float)cy, 22, cur ? C_FAINT : GRGB(52, 62, 72)); rx -= 30; }
            int sw = gfx_text(rx, cy, 13, GW_REG, age > 150 ? C_FAINT : C_DIM, GA_RIGHT, seen);
            gfx_text_fit(CONTENT_X + 74, cy - 11, rx - sw - CONTENT_X - 100, 18, GW_MED, age > 150 ? C_DIM : C_TEXT, GA_LEFT, pe->alias);
            gfx_text_fit(CONTENT_X + 74, cy + 11, rx - sw - CONTENT_X - 100, 13, GW_REG, C_DIM, GA_LEFT, sub);
            gfx_focus(false);
        }
        ui_scrollbar(CONTENT_R - 6, PEER_TOP, PEER_ROWS * PEER_STEP - 8, s_pcur.top, PEER_ROWS, n);
        ui_hit_scroll(CONTENT_X, PEER_TOP, CONTENT_W, PEER_ROWS * PEER_STEP, &s_pcur.top, &s_pcur.sel, n, PEER_ROWS, PEER_STEP);
    }

    ui_hints(xfer_running(x) ? "X Send|SQ Files|TRI Text|SEL Add by IP|START Transfer"
                             : "X Send|SQ Files|TRI Text|SEL Add by IP|START Favorite");
}

static void send_to_peer(const ls_peer *peer)
{
    bool ok;
    if (s_nsel == 0 && s_text_to_send[0]) {
        ok = send_start_text(peer, s_text_to_send);
    } else if (s_nsel > 0) {
        send_item *items = NULL;
        int cnt = 0, cap = 0;
        for (int i = 0; i < s_nsel; i++) send_collect(s_sel[i], &items, &cnt, &cap);
        if (cnt == 0) { free(items); toast("There is no file to send in your selection"); return; }
        ok = send_start_files(peer, items, cnt);     /* takes ownership of items */
    } else {
        toast("Choose files (Square) or write a message (Triangle) first");
        return;
    }
    if (!ok) { toast("Another transfer is still running"); return; }
    s_nsel = 0;
    s_text_to_send[0] = 0;
    go(SC_TRANSFER);
}

void send_input(uint32_t p)
{
    ls_peer pv[MAX_PEERS];
    int n = picker_peers(pv);
    if (p & UI_BTN_UP)   ui_cursor_move(&s_pcur, -1, n, PEER_ROWS);
    if (p & UI_BTN_DOWN) ui_cursor_move(&s_pcur, 1, n, PEER_ROWS);
    if (p & UI_BTN_SQUARE) { browser_open(BM_SEND); return; }
    if (p & UI_BTN_TRIANGLE) { open_osk(OP_MESSAGE, "Message to send", s_text_to_send, 300, false, SC_SEND); return; }
    if (p & UI_BTN_SELECT) { open_osk(OP_ADD_IP, "IP of the other device, e.g. 192.168.0.12", "", 21, false, SC_SEND); return; }
    if (p & UI_BTN_CIRCLE) { go(SC_HOME); return; }
    if (n == 0) return;
    ui_cursor_clamp(&s_pcur, n, PEER_ROWS);
    const ls_peer *peer = &pv[s_pcur.sel];
    if (p & UI_BTN_START) {                         /* (LEFT / RIGHT change column, so START marks a favorite) */
        bool now = settings_toggle_fav(peer->fingerprint, peer->alias);
        toast(now ? "%s is now a favorite" : "%s is no longer a favorite", peer->alias);
    }
    if (p & UI_BTN_CROSS) send_to_peer(peer);
}

/* ============================================================== history tab */

#define HIST_TOP  CONTENT_Y
#define HIST_ROWS 6
#define HIST_STEP 62

static const char *status_word(hist_status s)
{
    return s == H_OK ? "OK" : s == H_CANCELLED ? "Cancelled" : s == H_REJECTED ? "Rejected" : "Failed";
}

void history_draw(const xfer_status *x)
{
    draw_chrome(TAB_HISTORY, x);
    int n = history_count();
    char right[32] = "";
    if (n > 0) snprintf(right, sizeof right, "%d entr%s", n, n == 1 ? "y" : "ies");
    ui_page_title("History", right);
    if (n == 0) {
        gfx_icon(IC_HISTORY, (float)(CONTENT_X + CONTENT_W / 2), 270, 64, C_FAINT);
        gfx_text(CONTENT_X + CONTENT_W / 2, 330, 18, GW_MED, C_DIM, GA_CENTER, "Nothing yet.");
        gfx_text(CONTENT_X + CONTENT_W / 2, 358, 15, GW_REG, C_FAINT, GA_CENTER, "Everything you send or receive will be listed here.");
    } else {
        ui_cursor_clamp(&s_hcur, n, HIST_ROWS);
        for (int i = 0; i < HIST_ROWS && s_hcur.top + i < n; i++) {
            int idx = s_hcur.top + i, y = HIST_TOP + i * HIST_STEP, cy = y + 28;
            hist_entry h;
            if (!history_get(idx, &h)) break;
            bool cur = idx == s_hcur.sel;
            ui_card(CONTENT_X, y, CONTENT_W - 12, 56, cur);
            ui_hit_row(CONTENT_X, y - 3, CONTENT_W - 12, HIST_STEP, &s_hcur, idx, UI_BTN_CROSS);
            if (cur) gfx_focus(true);
            gcol rc = h.status == H_OK ? C_GOOD : h.status == H_FAILED ? C_BAD : C_WARN;
            gfx_circle((float)(CONTENT_X + 34), (float)cy, 19, C_ACCENT_BG);
            gfx_icon(h.outgoing ? IC_ARROW_UP : IC_ARROW_DOWN, (float)(CONTENT_X + 34), (float)cy, 22, C_ACCENT_HI);
            char when[24], sz[24], what[220];
            fmt_clock(h.time, when, sizeof when);
            fmt_size(h.bytes, sz, sizeof sz);
            if (h.is_text) snprintf(what, sizeof what, "%s \xC2\xB7 message: %s", h.outgoing ? "Sent" : "Received", h.summary);
            else snprintf(what, sizeof what, "%s \xC2\xB7 %s \xC2\xB7 %s", h.outgoing ? "Sent" : "Received", h.summary, sz);
            int rx = CONTENT_R - 12 - 18;
            int cw = ui_chip(rx, cy - 10, status_word(h.status), rc);
            gfx_text(rx, cy + 14, 12, GW_REG, C_FAINT, GA_RIGHT, when);
            gfx_text_fit(CONTENT_X + 66, cy - 10, CONTENT_W - 66 - cw - 50, 17, GW_MED, C_TEXT, GA_LEFT, h.peer);
            gfx_text_fit(CONTENT_X + 66, cy + 13, CONTENT_W - 66 - cw - 50, 13, GW_REG, C_DIM, GA_LEFT, what);
            gfx_focus(false);
        }
        ui_scrollbar(CONTENT_R - 6, HIST_TOP, HIST_ROWS * HIST_STEP - 6, s_hcur.top, HIST_ROWS, n);
        ui_hit_scroll(CONTENT_X, HIST_TOP, CONTENT_W, HIST_ROWS * HIST_STEP, &s_hcur.top, &s_hcur.sel, n, HIST_ROWS, HIST_STEP);
    }
    hist_entry cur;
    bool is_msg = n > 0 && history_get(s_hcur.sel, &cur) && cur.is_text;
    const char *act = is_msg ? "X Read message" : "X Show in Files";
    char hints[96];
    snprintf(hints, sizeof hints, xfer_running(x) ? "DUD Move|%s|START Transfer|O Back" : "DUD Move|%s|O Back", act);
    ui_hints(hints);
}

void history_input(uint32_t p)
{
    int n = history_count();
    if (p & UI_BTN_UP)   ui_cursor_move(&s_hcur, -1, n, HIST_ROWS);
    if (p & UI_BTN_DOWN) ui_cursor_move(&s_hcur, 1, n, HIST_ROWS);
    if (p & UI_BTN_CIRCLE) { go(SC_HOME); return; }
    if ((p & UI_BTN_CROSS) && n > 0) {
        hist_entry h;
        if (history_get(s_hcur.sel, &h) && h.is_text) {
            char title[96];
            snprintf(title, sizeof title, "Message %s %s", h.outgoing ? "to" : "from", h.peer);
            show_message(title, h.text, SC_HISTORY);
        } else if (history_get(s_hcur.sel, &h)) {
            if (h.path[0]) files_show_file(h.path);
            else toast("No location was saved for this entry");
        }
    }
}

