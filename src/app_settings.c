/*
 * app_settings.c - the Settings tab, the log, and the dropdown lists for settings
 *
 * Author: trashman (VitaSend)
 */
#include "app_int.h"

static ui_cursor s_setcur;

void settings_reset(void) { memset(&s_setcur, 0, sizeof s_setcur); }


/* ============================================================== settings tab */

/* The rows, grouped under headers. */
enum { ST_NAME, ST_PIN, ST_HIDE,                                  /* This device        */
       ST_FOLDER, ST_ACCEPT, ST_AUTOINST, ST_EXISTS,              /* Receiving          */
       ST_THEME, ST_BATTERY, ST_CLOCK, ST_SAVER, ST_SOUND,        /* Display and sound  */
       ST_ABOUT, ST_CHECKUPD, ST_LOG, ST_QUIT,                    /* About              */
       ST_COUNT };

#define SET_TOP    CONTENT_Y
#define SET_AREA_B (CONTENT_B - 24)                              /* the info line sits below this */
#define SET_HEAD_H 30

static const char *set_group(int i)                              /* the header above row i, or NULL if i continues a group */
{
    switch (i) {
    case ST_NAME:   return "THIS DEVICE";
    case ST_FOLDER: return "RECEIVING";
    case ST_THEME:  return "DISPLAY AND SOUND";
    case ST_ABOUT:  return "ABOUT";
    }
    return NULL;
}

static const char *set_group_of(int i)
{
    const char *g = NULL;
    for (int k = 0; k <= i; k++) if (set_group(k)) g = set_group(k);
    return g;
}

static int set_row_h(int i) { return i == ST_ABOUT ? (LS_SUPPORT_TEXT[0] ? 116 : 90) : 48; }
static int set_head_h(int i, int top) { return (set_group(i) || i == top) ? SET_HEAD_H : 0; }

/* Keeps the highlighted row on the page (rows have different heights, so the usual fixed-rows cursor does not fit). */
static void set_fit(void)
{
    if (s_setcur.sel < 0) s_setcur.sel = 0;
    if (s_setcur.sel >= ST_COUNT) s_setcur.sel = ST_COUNT - 1;
    if (s_setcur.top > s_setcur.sel) s_setcur.top = s_setcur.sel;
    if (s_setcur.top < 0) s_setcur.top = 0;
    for (;;) {
        int y = SET_TOP;
        for (int i = s_setcur.top; i <= s_setcur.sel; i++) y += set_head_h(i, s_setcur.top) + set_row_h(i);
        if (y - 6 <= SET_AREA_B || s_setcur.top >= s_setcur.sel) break;
        s_setcur.top++;
    }
}

static const char *exists_label(int m)
{
    return m == EXISTS_SKIP ? "Skip - ignore files that are already there"
         : m == EXISTS_OVERWRITE ? "Overwrite - replace the old file with the new one"
         : "Keep both - the new file gets (1) added to its name";
}

static const char *autoinst_label(int m)
{
    return m == INSTALL_FAVORITES ? "Favorites only - a .vpk from a favorite is installed without asking"
         : m == INSTALL_EVERYONE ? "Everyone - any received .vpk is installed without asking (risky!)"
         : "Off - always ask before installing";
}

static const int k_saver_secs[] = { 0, 30, 60, 120, 300 };
#define SAVER_N 5

static const char *saver_label(int s)
{
    return s == 30 ? "After 30 seconds without use" : s == 60 ? "After 1 minute without use"
         : s == 120 ? "After 2 minutes without use" : s == 300 ? "After 5 minutes without use"
         : "Off - the screen always stays as it is";
}

void settings_draw(const xfer_status *x)
{
    draw_chrome(TAB_SETTINGS, x);
    ui_page_title("Settings", NULL);
    ls_settings st;
    settings_snapshot(&st);
    static const char *const label[ST_COUNT] = {
        "Device name", "PIN to send to this device", "Hide IP address and fingerprint",
        "Save received files in", "Quick Save", "Install received .vpk files", "When a received file already exists",
        "Colours", "Battery indicator", "Clock", "Screensaver", "Sound",
        "", "Check for updates at start", "Log", "" };
    char value[ST_COUNT][260];
    ls_strlcpy(value[ST_NAME], g_id.alias, sizeof value[0]);
    ls_strlcpy(value[ST_PIN], st.pin[0] ? st.pin : "Off", sizeof value[0]);
    ls_strlcpy(value[ST_HIDE], st.hide_ids ? "On - not shown in the left menu or on the Receive page" : "Off - shown in the left menu and on the Receive page", sizeof value[0]);
    ls_strlcpy(value[ST_FOLDER], st.receive_dir, sizeof value[0]);
    snprintf(value[ST_ACCEPT], sizeof value[0], "%s - %s", accept_label(st.auto_accept), accept_help(st.auto_accept));
    ls_strlcpy(value[ST_EXISTS], exists_label(st.on_exists), sizeof value[0]);
    ls_strlcpy(value[ST_AUTOINST], autoinst_label(st.auto_install), sizeof value[0]);
    snprintf(value[ST_THEME], sizeof value[0], "%s", st.theme == THEME_OLED ? "OLED - pure black, like the desktop app's OLED mode" : "Dark - the original blue-grey look");
    ls_strlcpy(value[ST_BATTERY], st.show_battery ? "On - percentage and symbol at the top right" : "Off - not shown", sizeof value[0]);
    ls_strlcpy(value[ST_CLOCK], st.show_clock ? "On - the time is shown at the top right, next to the battery" : "Off - not shown", sizeof value[0]);
    ls_strlcpy(value[ST_SAVER], saver_label(st.screensaver), sizeof value[0]);
    ls_strlcpy(value[ST_SOUND], st.sound ? "On - a short chime when a transfer or an installation has finished" : "Off - silent", sizeof value[0]);
    ls_strlcpy(value[ST_ABOUT], "", sizeof value[0]);
    ls_strlcpy(value[ST_CHECKUPD], st.check_updates ? "On - looks for a new version on GitHub when VitaSend starts (asks once a day at most)" : "Off - only when you check in the About box", sizeof value[0]);
    ls_strlcpy(value[ST_LOG], "Show what the app has been doing", sizeof value[0]);
    ls_strlcpy(value[ST_QUIT], "Quit VitaSend", sizeof value[0]);

    set_fit();
    int y = SET_TOP, shown = 0;
    for (int i = s_setcur.top; i < ST_COUNT; i++) {
        int hh = set_head_h(i, s_setcur.top), rh = set_row_h(i);
        if (y + hh + rh - 6 > SET_AREA_B) break;
        if (hh) {
            gfx_text(CONTENT_X + 6, y + 14, 12, GW_BOLD, C_ACCENT_HI, GA_LEFT, set_group_of(i));
            gfx_rect(CONTENT_X + 6 + gfx_text_width(12, GW_BOLD, set_group_of(i)) + 12, y + 14, CONTENT_W - 52 - gfx_text_width(12, GW_BOLD, set_group_of(i)), 1, C_BORDER);
            y += hh;
        }
        bool cur = i == s_setcur.sel;
        ui_card(CONTENT_X, y, CONTENT_W - 12, rh - 6, cur);          /* as wide as in History: the scrollbar stays clear */
        ui_hit_row(CONTENT_X, y - 3, CONTENT_W - 12, rh, &s_setcur, i, UI_BTN_CROSS);
        if (cur) gfx_focus(true);
        if (i == ST_QUIT) {
            gfx_icon(IC_CLOSE, (float)(CONTENT_X + 34), (float)(y + 21), 20, C_BAD);
            gfx_text_fit(CONTENT_X + 62, y + 21, CONTENT_W - 102, 17, GW_MED, C_BAD, GA_LEFT, value[i]);
        } else if (i == ST_ABOUT) {
            gfx_rrect(CONTENT_X + 22, y + 18, 48, 48, 14, C_ACCENT);
            gfx_icon(IC_LOGO, (float)(CONTENT_X + 46), (float)(y + 42), 40, GRGB(255, 255, 255));
            char l1[80];
            snprintf(l1, sizeof l1, "VitaSend  v%s", LS_APP_VERSION);
            gfx_text(CONTENT_X + 90, y + 24, 19, GW_BOLD, C_TEXT, GA_LEFT, l1);
            gfx_text(CONTENT_X + 90, y + 47, 14, GW_REG, C_DIM, GA_LEFT, "Version date  " LS_APP_DATE);
            gfx_text(CONTENT_X + 90, y + 67, 14, GW_REG, C_DIM, GA_LEFT, "Created by trashman");
            if (LS_SUPPORT_TEXT[0]) gfx_text_fit(CONTENT_X + 90, y + 91, CONTENT_W - 124, 14, GW_MED, C_ACCENT_HI, GA_LEFT, LS_SUPPORT_TEXT);
            /* the update button, at the right of the card */
            int bw = ui_action_width(PB_CROSS, "Check for updates");
            ui_action(CONTENT_R - 50 - bw, y + 38, PB_CROSS, "Check for updates");
        } else {
            gfx_text(CONTENT_X + 24, y + 13, 12, GW_REG, C_DIM, GA_LEFT, label[i]);
            gfx_text_fit(CONTENT_X + 24, y + 30, CONTENT_W - 92, 16, GW_MED, C_TEXT, GA_LEFT, value[i]);
            if (cur) gfx_icon(IC_BACK, (float)(CONTENT_R - 42), (float)(y + 21), 18, C_FAINT);
        }
        gfx_focus(false);
        y += rh;
        shown++;
    }
    ui_scrollbar(CONTENT_R - 6, SET_TOP, SET_AREA_B - SET_TOP, s_setcur.top, shown, ST_COUNT);
    ui_hit_scroll(CONTENT_X, SET_TOP, CONTENT_W, SET_AREA_B - SET_TOP, &s_setcur.top, &s_setcur.sel, ST_COUNT, 5, 56);
    char info[200];
    if (s_setcur.sel == ST_ACCEPT || s_setcur.sel == ST_AUTOINST) snprintf(info, sizeof info, "Press X to choose. Quick Save and automatic installation trust the name a sender announces.");
    else if (s_setcur.sel == ST_EXISTS || s_setcur.sel == ST_SAVER) snprintf(info, sizeof info, "Press X to choose from a list.");
    else if (s_setcur.sel == ST_ABOUT) snprintf(info, sizeof info, "Press X to look for a new version of VitaSend on GitHub now.");
    else if (s_setcur.sel == ST_CHECKUPD || s_setcur.sel == ST_HIDE) snprintf(info, sizeof info, "Press X to switch.");
    else if (s_setcur.sel == ST_BATTERY || s_setcur.sel == ST_CLOCK) snprintf(info, sizeof info, "Press X to switch. Shown at the top right of every page; the time is the Vita's own clock.");
    else snprintf(info, sizeof info, "Port %u - edit config.json on the memory card to change it.", g_id.port);
    gfx_text_fit(CONTENT_X + 4, CONTENT_B - 8, CONTENT_W - 8, 12, GW_REG, C_FAINT, GA_LEFT, info);

    ui_hints(s_setcur.sel == ST_PIN ? "DUD Move|X Change|SQ Clear PIN|O Back" : (s_setcur.sel == ST_ABOUT ? "DUD Move|X Check for updates|O Back" : "DUD Move|X Change|O Back"));
}

enum { PK_ACCEPT, PK_INSTALL, PK_EXISTS, PK_SAVER, PK_COUNT };
static void pick_open(int what);

void settings_input(uint32_t p)
{
    if (p & UI_BTN_UP)   { s_setcur.sel--; set_fit(); }
    if (p & UI_BTN_DOWN) { s_setcur.sel++; set_fit(); }
    if (p & UI_BTN_CIRCLE) { go(SC_HOME); return; }
    ls_settings st;
    settings_snapshot(&st);
    if ((p & UI_BTN_SQUARE) && s_setcur.sel == ST_PIN) {
        settings_set_pin("");
        toast("PIN removed");
    }
    if (p & UI_BTN_CROSS) {
        switch (s_setcur.sel) {
        case ST_NAME:   open_osk(OP_ALIAS, "Device name", g_id.alias, 40, false, SC_SETTINGS); break;
        case ST_PIN:    open_osk(OP_PIN_SET, "PIN (digits only, empty = off)", st.pin, SET_PIN_MAX, true, SC_SETTINGS); break;
        case ST_HIDE:   settings_set_hide_ids(!st.hide_ids); break;
        case ST_FOLDER: browser_open(BM_PICK_FOLDER); break;
        case ST_ACCEPT:   pick_open(PK_ACCEPT); break;
        case ST_EXISTS:   pick_open(PK_EXISTS); break;
        case ST_AUTOINST: pick_open(PK_INSTALL); break;
        case ST_THEME:  settings_set_theme(st.theme == THEME_OLED ? THEME_DARK : THEME_OLED); ui_set_theme(st.theme == THEME_OLED ? THEME_DARK : THEME_OLED); break;
        case ST_BATTERY: settings_set_show_battery(!st.show_battery); break;
        case ST_CLOCK:   settings_set_show_clock(!st.show_clock); break;
        case ST_SAVER:   pick_open(PK_SAVER); break;
        case ST_SOUND:
            settings_set_sound(!st.sound);
            if (!st.sound) plat_chime();                         /* switched on: let the user hear it */
            break;
        case ST_ABOUT:    upd_ui_open(true, SC_SETTINGS); break;
        case ST_CHECKUPD: settings_set_check_updates(!st.check_updates); break;
        case ST_LOG:    go(SC_LOG); break;
        case ST_QUIT:   confirm_open(CF_QUIT, SC_SETTINGS); break;
        }
    }
}

/* ============================================================== log */

#define LOG_ROWS 20

void log_draw(const xfer_status *x)
{
    draw_chrome(TAB_SETTINGS, x);
    ui_page_title("Log", "also saved as log.txt");
    ui_card(CONTENT_X, CONTENT_Y - 6, CONTENT_W, CONTENT_B - CONTENT_Y + 6, false);
    static char lines[LOG_ROWS][LS_LOG_LINE_LEN];
    int n = ls_log_tail(lines, LOG_ROWS);
    for (int i = 0; i < n; i++) {
        const char *l = lines[i];
        gcol col = C_DIM;
        if (strstr(l, "FAIL") || strstr(l, "failed") || strstr(l, "error")) col = C_BAD;
        else if (strstr(l, "OK ") || strstr(l, "registered") || strstr(l, "found")) col = C_GOOD;
        gfx_text_fit(CONTENT_X + 16, CONTENT_Y + 10 + i * 19, CONTENT_W - 32, 12, GW_REG, col, GA_LEFT, l);
    }
    ui_hints("O Back");
}

void log_input(uint32_t p)
{
    if (p & (UI_BTN_CIRCLE | UI_BTN_CROSS)) go(SC_SETTINGS);
}

/* ============================================================== dropdown lists for settings */
/* Every setting with more than two options is chosen from a list (a rule for all settings to come). Quick Save and "install
 * received .vpk files" are risky: their choice is confirmed with what it does and what it risks (X = yes, O = no). The others
 * apply at once. */

typedef struct { const char *label, *short_help, *does, *risk; bool risky; } pk_option;
typedef struct { const char *title; const pk_option *opt; int n; bool confirm; } pk_def;

static const pk_option k_pk_accept[3] = {
    { "Off", "Ask me about every request",
      "Every incoming request is shown and you accept or reject it yourself.",
      "No risk: nothing arrives unless you say yes.", false },
    { "Favorites", "Accept requests from favorites, ask about the rest",
      "Requests from your favorite devices are accepted straight away. Everyone else is still asked.",
      "A device only has to announce a favorite's name to be treated as that favorite. Files can fill the memory card.", true },
    { "Everyone", "Accept every request without asking",
      "Every request is accepted at once, from any device on the network.",
      "Anyone on your Wi-Fi can put files on your memory card. Use it only on a network you trust.", true },
};

static const pk_option k_pk_install[3] = {
    { "Off - always ask first", "A received .vpk is installed only after you confirm",
      "A received .vpk package is only installed after you have looked at it and pressed Install.",
      "No risk: nothing is installed without you.", false },
    { "Favorites only", "A .vpk from a favorite is installed on arrival",
      "A .vpk that arrives from one of your favorites is installed as soon as it is complete.",
      "A package can run anything on your Vita, and a favorite's name can be faked by others on your network.", true },
    { "Everyone", "Every received .vpk is installed on arrival",
      "Any .vpk that arrives is installed immediately, without a question.",
      "Anyone on your network could put an app on your Vita - even a harmful one. Strongly discouraged.", true },
};

static const pk_option k_pk_exists[3] = {
    { "Keep both", "The new file gets (1) added to its name", NULL, NULL, false },      /* EXISTS_KEEP_BOTH = 0 */
    { "Skip", "Ignore files that are already there", NULL, NULL, false },              /* EXISTS_SKIP = 1      */
    { "Overwrite", "Replace the old file with the new one", NULL, NULL, false },       /* EXISTS_OVERWRITE = 2 */
};

static const pk_option k_pk_saver[SAVER_N] = {
    { "Off", "The screen always stays as it is", NULL, NULL, false },
    { "After 30 seconds", "Without a button, touch or incoming transfer", NULL, NULL, false },
    { "After 1 minute", "Without a button, touch or incoming transfer", NULL, NULL, false },
    { "After 2 minutes", "Without a button, touch or incoming transfer", NULL, NULL, false },
    { "After 5 minutes", "Without a button, touch or incoming transfer", NULL, NULL, false },
};

static const pk_def k_pk[PK_COUNT] = {
    [PK_ACCEPT]  = { "Quick Save", k_pk_accept, 3, true },
    [PK_INSTALL] = { "Install received .vpk files", k_pk_install, 3, true },
    [PK_EXISTS]  = { "When a received file already exists", k_pk_exists, 3, false },
    [PK_SAVER]   = { "Screensaver", k_pk_saver, SAVER_N, false },
};

static int s_pk_what, s_pk_value;
static ui_cursor s_pk_cur;

static const pk_option *pk_opts(int what) { return k_pk[what].opt; }
static const char *pk_title(int what) { return k_pk[what].title; }

static int pk_current(int what)
{
    ls_settings st;
    settings_snapshot(&st);
    switch (what) {
    case PK_ACCEPT:  return st.auto_accept;
    case PK_INSTALL: return st.auto_install;
    case PK_EXISTS:  return st.on_exists;
    default: {
        int k = 0;
        for (int i = 0; i < SAVER_N; i++) if (k_saver_secs[i] == st.screensaver) k = i;
        return k;
    }
    }
}

static void pick_open(int what)
{
    s_pk_what = what;
    memset(&s_pk_cur, 0, sizeof s_pk_cur);
    s_pk_cur.sel = pk_current(what);
    go(SC_PICK);
}

void pick_apply(void)
{
    switch (s_pk_what) {
    case PK_ACCEPT:  settings_set_auto_accept(s_pk_value); break;
    case PK_INSTALL: settings_set_auto_install(s_pk_value); break;
    case PK_EXISTS:  settings_set_on_exists(s_pk_value); break;
    case PK_SAVER:   settings_set_screensaver(k_saver_secs[s_pk_value]); break;
    }
    toast("%s: %s", pk_title(s_pk_what), pk_opts(s_pk_what)[s_pk_value].label);
}

static void under_settings(void *ctx) { draw_tab(SC_SETTINGS, ctx); }

void pick_draw(const xfer_status *x)
{
    int dx, dy;
    const pk_def *def = &k_pk[s_pk_what];
    int n = def->n, step = n > 3 ? 54 : 66, ch = step - 8, h = n > 3 ? 94 + n * step + 80 : 380;
    ui_dialog_begin(SC_PICK | (s_pk_what << 8), 660, h, under_settings, (void *)x, &dx, &dy);
    const pk_option *o = def->opt;
    int cur = pk_current(s_pk_what);
    gfx_text(dx + 32, dy + 40, 22, GW_BOLD, C_TEXT, GA_LEFT, def->title);
    gfx_text(dx + 32, dy + 68, 14, GW_REG, C_DIM, GA_LEFT, def->confirm ? "Choose with the D-pad. You will be asked to confirm." : "Choose with the D-pad.");
    for (int i = 0; i < n; i++) {
        int y = dy + 94 + i * step;
        bool on = i == s_pk_cur.sel;
        ui_card(dx + 28, y, 604, ch, on);
        ui_hit_row(dx + 28, y, 604, step - 4, &s_pk_cur, i, UI_BTN_CROSS);
        if (on) gfx_focus(true);
        gfx_text_fit(dx + 52, y + (ch > 50 ? 20 : 16), 500, 17, GW_MED, o[i].risky ? C_WARN : C_TEXT, GA_LEFT, o[i].label);
        gfx_text_fit(dx + 52, y + (ch > 50 ? 42 : 34), 520, 13, GW_REG, C_DIM, GA_LEFT, o[i].short_help);
        if (i == cur) gfx_icon(IC_CHECK, (float)(dx + 606), (float)(y + ch / 2), 22, C_GOOD);
        gfx_focus(false);
    }
    int w1 = ui_action_width(PB_CROSS, "Choose");
    ui_action(dx + 32, dy + h - 50, PB_CROSS, "Choose");
    ui_action(dx + 32 + w1 + 10, dy + h - 50, PB_CIRCLE, "Cancel");
    ui_hints("DUD Move|X Choose|O Cancel");
}

void pick_input(uint32_t p)
{
    int n = k_pk[s_pk_what].n;
    if (p & UI_BTN_UP)   ui_cursor_move(&s_pk_cur, -1, n, n);
    if (p & UI_BTN_DOWN) ui_cursor_move(&s_pk_cur, 1, n, n);
    if (p & UI_BTN_CIRCLE) { go(SC_SETTINGS); return; }
    if (p & UI_BTN_CROSS) {
        if (s_pk_cur.sel == pk_current(s_pk_what)) { go(SC_SETTINGS); return; }     /* unchanged */
        s_pk_value = s_pk_cur.sel;
        if (!k_pk[s_pk_what].confirm) { pick_apply(); go(SC_SETTINGS); return; }
        confirm_open(CF_SETTING, SC_SETTINGS);
    }
}

void setting_confirm_draw(const xfer_status *x)
{
    int dx, dy;
    ui_dialog_begin(SC_CONFIRM | (s_pk_what << 8) | (s_pk_value << 12) | (CF_SETTING << 20), 700, 400, under_settings, (void *)x, &dx, &dy);
    const pk_option *o = &pk_opts(s_pk_what)[s_pk_value];
    char line[200];
    gfx_text(dx + 32, dy + 42, 22, GW_BOLD, C_TEXT, GA_LEFT, "Use this setting?");
    snprintf(line, sizeof line, "%s:  %s", pk_title(s_pk_what), o->label);
    gfx_text_fit(dx + 32, dy + 82, 640, 18, GW_BOLD, C_ACCENT_HI, GA_LEFT, line);

    gfx_line_t ln[4];
    int y = dy + 118;
    gfx_text(dx + 32, y, 13, GW_MED, C_DIM, GA_LEFT, "WHAT IT DOES");
    y += 24;
    int n = gfx_wrap(o->does, 16, GW_REG, 636, ln, 3);
    for (int i = 0; i < n; i++, y += 23) { char b[160]; int l = ln[i].len < 159 ? ln[i].len : 159; memcpy(b, ln[i].p, (size_t)l); b[l] = 0; gfx_text(dx + 32, y, 16, GW_REG, C_TEXT, GA_LEFT, b); }
    y += 14;
    gfx_text(dx + 32, y, 13, GW_MED, o->risky ? C_WARN : C_GOOD, GA_LEFT, o->risky ? "RISK" : "RISK");
    y += 24;
    n = gfx_wrap(o->risk, 16, GW_REG, 636, ln, 3);
    for (int i = 0; i < n; i++, y += 23) { char b[160]; int l = ln[i].len < 159 ? ln[i].len : 159; memcpy(b, ln[i].p, (size_t)l); b[l] = 0; gfx_text(dx + 32, y, 16, GW_REG, o->risky ? C_WARN : C_GOOD, GA_LEFT, b); }

    int w = ui_action_width(PB_CROSS, "Yes, use it");
    ui_action(dx + 32, dy + 352, PB_CROSS, "Yes, use it");
    ui_action(dx + 32 + w + 10, dy + 352, PB_CIRCLE, "No");
    ui_hints("X Yes|O No");
}

