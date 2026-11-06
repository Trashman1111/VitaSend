/*
 * ui.c - Look and feel on top of gfx.c: sidebar, cards, chips, button hints, dialogs, on-screen keyboard, progress bars, and the
 * registry of tap areas that makes every widget usable by touch.
 *
 * Author: trashman (VitaSend)
 */
#include "ui.h"
#include "util.h"
#include "wifi.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================== formatting */

void fmt_size(uint64_t b, char *out, size_t cap)
{
    if (b < 1024) snprintf(out, cap, "%u B", (unsigned)b);
    else if (b < 1024u * 1024u) snprintf(out, cap, "%.1f KB", (double)b / 1024.0);
    else if (b < 1024ull * 1024 * 1024) snprintf(out, cap, "%.1f MB", (double)b / (1024.0 * 1024.0));
    else snprintf(out, cap, "%.2f GB", (double)b / (1024.0 * 1024.0 * 1024.0));
}

void fmt_clock(int64_t t, char *out, size_t cap)
{
    if (t < 1000000000) {                          /* the Vita clock was not available */
        snprintf(out, cap, "-- --:--");
        return;
    }
    time_t tt = (time_t)t;
    struct tm tmv;
    struct tm *r = gmtime_r(&tt, &tmv);
    if (!r) { snprintf(out, cap, "-- --:--"); return; }
    snprintf(out, cap, "%02d-%02d %02d:%02d", tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
}

void fmt_duration(uint64_t s, char *out, size_t cap)
{
    if (s < 60) snprintf(out, cap, "%u s", (unsigned)s);
    else if (s < 3600) snprintf(out, cap, "%u min %02u s", (unsigned)(s / 60), (unsigned)(s % 60));
    else snprintf(out, cap, "%u h %02u min", (unsigned)(s / 3600), (unsigned)((s % 3600) / 60));
}

/* ============================================================== touch areas */

#define HIT_MAX 160
enum { HK_KEYS, HK_CALL, HK_ROW, HK_SCROLL };
typedef struct {
    int       kind, x, y, w, h;
    uint32_t  keys;
    ui_hit_fn fn;
    void     *ctx;
    int       arg;
    ui_cursor *cur;
    int       index;
    int      *top, *sel;
    int       count, rows, rowh;
} hit_t;

static hit_t s_hits[HIT_MAX];
static int   s_nhits;
static hit_t s_drag;
static bool  s_dragging;
static int   s_drag_top0;
static void (*s_tab_cb)(int);

void ui_hit_reset(void) { s_nhits = 0; }

static hit_t *hit_add(int kind, int x, int y, int w, int h)
{
    if (s_nhits >= HIT_MAX) return NULL;
    hit_t *t = &s_hits[s_nhits++];
    memset(t, 0, sizeof *t);
    t->kind = kind; t->x = x; t->y = y; t->w = w; t->h = h;
    return t;
}

void ui_hit_keys(int x, int y, int w, int h, uint32_t keys)
{
    hit_t *t = hit_add(HK_KEYS, x, y, w, h);
    if (t) t->keys = keys;
}

void ui_hit_call(int x, int y, int w, int h, ui_hit_fn fn, void *ctx, int arg)
{
    hit_t *t = hit_add(HK_CALL, x, y, w, h);
    if (t) { t->fn = fn; t->ctx = ctx; t->arg = arg; }
}

void ui_hit_row(int x, int y, int w, int h, ui_cursor *c, int index, uint32_t keys)
{
    hit_t *t = hit_add(HK_ROW, x, y, w, h);
    if (t) { t->cur = c; t->index = index; t->keys = keys; }
}

void ui_hit_scroll(int x, int y, int w, int h, int *top, int *sel, int count, int rows, int rowh)
{
    hit_t *t = hit_add(HK_SCROLL, x, y, w, h);
    if (t) { t->top = top; t->sel = sel; t->count = count; t->rows = rows; t->rowh = rowh > 0 ? rowh : 1; }
}

void ui_set_tab_handler(void (*fn)(int tab)) { s_tab_cb = fn; }

static void tab_thunk(void *ctx, int arg)
{
    (void)ctx;
    if (s_tab_cb) s_tab_cb(arg);
}

/* The topmost area under the finger; scroll areas only when `scroll` is set, otherwise everything else. */
static const hit_t *hit_find(int x, int y, bool scroll)
{
    for (int i = s_nhits - 1; i >= 0; i--) {
        const hit_t *t = &s_hits[i];
        if ((t->kind == HK_SCROLL) != scroll) continue;
        if (x >= t->x && x < t->x + t->w && y >= t->y && y < t->y + t->h) return t;
    }
    return NULL;
}

uint32_t ui_touch_tap(int x, int y)
{
    const hit_t *t = hit_find(x, y, false);
    if (!t) return 0;
    switch (t->kind) {
    case HK_KEYS:
        return t->keys;
    case HK_CALL:
        if (t->fn) t->fn(t->ctx, t->arg);
        return 0;
    case HK_ROW: {
        t->cur->sel = t->index;
        return t->keys;
    }
    }
    return 0;
}

bool ui_touch_drag_begin(int x, int y)
{
    const hit_t *t = hit_find(x, y, true);
    s_dragging = t != NULL;
    if (t) {
        s_drag = *t;
        s_drag_top0 = *t->top;
    }
    return s_dragging;
}

void ui_touch_drag(int dy)
{
    if (!s_dragging || s_drag.count <= s_drag.rows) return;
    int steps = dy <= 0 ? (-dy + s_drag.rowh / 2) / s_drag.rowh : -((dy + s_drag.rowh / 2) / s_drag.rowh);   /* finger up = list up */
    int top = s_drag_top0 + steps, max = s_drag.count - s_drag.rows;
    if (top > max) top = max;
    if (top < 0) top = 0;
    *s_drag.top = top;
    if (s_drag.sel) {
        if (*s_drag.sel < top) *s_drag.sel = top;
        if (*s_drag.sel >= top + s_drag.rows) *s_drag.sel = top + s_drag.rows - 1;
    }
}

void ui_touch_drag_end(void) { s_dragging = false; }

size_t ui_hit_dump(char *out, size_t cap)
{
    size_t n = 0;
    if (cap) out[0] = 0;
    for (int i = 0; i < s_nhits && n + 80 < cap; i++) {
        const hit_t *t = &s_hits[i];
        static const char kinds[] = "KCRS";
        int extra = t->kind == HK_KEYS ? (int)t->keys : t->kind == HK_CALL ? t->arg : t->kind == HK_ROW ? t->index : 0;
        n += (size_t)snprintf(out + n, cap - n, "%c %d %d %d %d %d\n", kinds[t->kind], t->x, t->y, t->w, t->h, extra);
    }
    return n;
}

/* A PlayStation button symbol (PB_*) as the button bit it stands for; 0 for the ones that are no single button. */
static uint32_t pb_keys(int pb)
{
    switch (pb) {
    case PB_CROSS: return UI_BTN_CROSS;
    case PB_CIRCLE: return UI_BTN_CIRCLE;
    case PB_SQUARE: return UI_BTN_SQUARE;
    case PB_TRIANGLE: return UI_BTN_TRIANGLE;
    case PB_L: return UI_BTN_L;
    case PB_R: return UI_BTN_R;
    case PB_START: return UI_BTN_START;
    case PB_SELECT: return UI_BTN_SELECT;
    default: return 0;
    }
}

/* ============================================================== chrome */

static gcol with_alpha(gcol c, int a)
{
    return (c & 0x00FFFFFFu) | ((uint32_t)a << 24);
}

/* ============================================================== themes */

ui_theme g_th = {
    GRGB(15, 19, 23), GRGB(11, 14, 17), GRGB(25, 31, 37), GRGB(31, 40, 48), GRGB(40, 49, 58),
    GRGB(232, 238, 243), GRGB(140, 152, 164), GRGB(88, 99, 110),
    GRGB(38, 166, 154), GRGB(72, 208, 190), GRGBA(38, 166, 154, 46), GRGB(6, 24, 22), false
};
static int s_theme_id;

static const ui_theme k_oled = {
    GRGB(0, 0, 0), GRGB(0, 0, 0), GRGB(15, 15, 16), GRGB(26, 27, 28), GRGB(38, 40, 42),
    GRGB(240, 242, 244), GRGB(150, 156, 162), GRGB(96, 101, 106),
    GRGB(38, 166, 154), GRGB(72, 208, 190), GRGBA(38, 166, 154, 46), GRGB(2, 20, 18), true
};
static const ui_theme k_dark = {
    GRGB(15, 19, 23), GRGB(11, 14, 17), GRGB(25, 31, 37), GRGB(31, 40, 48), GRGB(40, 49, 58),
    GRGB(232, 238, 243), GRGB(140, 152, 164), GRGB(88, 99, 110),
    GRGB(38, 166, 154), GRGB(72, 208, 190), GRGBA(38, 166, 154, 46), GRGB(6, 24, 22), false
};

void ui_set_theme(int theme)
{
    s_theme_id = theme == THEME_OLED ? THEME_OLED : THEME_DARK;
    g_th = s_theme_id == THEME_OLED ? k_oled : k_dark;
}

int ui_theme_id(void) { return s_theme_id; }

const char *ui_theme_name(int theme) { return theme == THEME_OLED ? "OLED" : "Dark"; }

static bool s_side_focus;
static int  s_cur_tab;                                         /* the tab that is showing (the "L R Tabs" hint is only shown on Receive) */
static bool s_offer_wifi;                                      /* no Wi-Fi and not already switching it on: Triangle switches it on */
void ui_set_side_focus(bool on) { s_side_focus = on; }

static void wifi_tap_thunk(void *ctx, int arg)
{
    (void)ctx;
    (void)arg;
    wifi_user_action();
}

void ui_draw_chrome(int tab, const ui_side_info *info)
{
    static const char *const names[TAB_COUNT] = { "Receive", "Send", "History", "Files", "Settings" };
    static const int icons[TAB_COUNT] = { IC_RECEIVE, IC_SEND, IC_HISTORY, IC_FOLDER, IC_SETTINGS };

    gfx_clear(C_BG);
    gfx_rect(0, 0, SIDEBAR_W, HINT_Y, C_SIDEBAR);
    gfx_rect(0, HINT_Y, GFX_W, GFX_H - HINT_Y, C_SIDEBAR);
    if (g_th.divider) {                                        /* black on black: separate the areas with thin lines */
        gfx_rect(SIDEBAR_W - 1, 0, 1, HINT_Y, C_BORDER);
        gfx_rect(0, HINT_Y, GFX_W, 1, C_BORDER);
    }

    gfx_norec(true);                                           /* the sidebar is not part of the text dump */
    gfx_rrect(20, 22, 40, 40, 12, C_ACCENT);
    gfx_icon(IC_LOGO, 40, 42, 34, GRGB(255, 255, 255));
    gfx_text(70, 42, 21, GW_BOLD, C_TEXT, GA_LEFT, "VitaSend");

    for (int i = 0; i < TAB_COUNT; i++) {
        int y = 90 + i * 50;
        bool on = i == tab;
        if (on) gfx_rrect(12, y, SIDEBAR_W - 24, 44, 12, C_ACCENT_BG);
        if (on && s_side_focus) { gfx_rrect_outline(12, y, SIDEBAR_W - 24, 44, 12, 2, C_ACCENT_HI); }
        gfx_icon(icons[i], 38, (float)(y + 22), 26, on ? C_ACCENT_HI : C_DIM);
        gfx_text(62, y + 22, 17, GW_MED, on ? C_ACCENT_HI : C_DIM, GA_LEFT, names[i]);
        ui_hit_call(8, y - 3, SIDEBAR_W - 16, 50, tab_thunk, NULL, i);
    }

    int base = HINT_Y - 18;
    s_cur_tab = tab;
    s_offer_wifi = !(info && info->ip && info->ip[0]) && wifi_get() != WIFI_CONNECTING;
    if (info && info->ip && info->ip[0]) {
        gfx_icon(IC_WIFI, 30, (float)(base - 4), 18, C_GOOD);
        gfx_text_fit(48, base, SIDEBAR_W - 60, 14, GW_REG, C_DIM, GA_LEFT, info->ip);
    } else {                                                   /* no Wi-Fi: the row is a button that switches it on */
        bool busy = wifi_get() == WIFI_CONNECTING;
        gfx_icon(busy ? IC_WIFI : IC_WIFI_OFF, 30, (float)(base - 4), 18, C_WARN);
        gfx_text_fit(48, base, SIDEBAR_W - 60, 14, GW_REG, C_WARN, GA_LEFT, busy ? "Connecting..." : "No Wi-Fi");
        if (!busy) ui_hit_call(8, base - 22, SIDEBAR_W - 16, 38, wifi_tap_thunk, NULL, 0);
    }
    gfx_text(20, base - 28, 12, GW_REG, C_FAINT, GA_LEFT, "v" LS_APP_VERSION " by trashman");

    if (info && info->xfer_title) {
        int y = HINT_Y - 150;
        gfx_rrect(12, y, SIDEBAR_W - 24, 82, 12, C_CARD);
        gfx_text(24, y + 20, 13, GW_REG, C_DIM, GA_LEFT, info->xfer_title);
        gfx_text_fit(24, y + 42, SIDEBAR_W - 48, 15, GW_MED, C_TEXT, GA_LEFT, info->xfer_peer);
        ui_progress(24, y + 58, SIDEBAR_W - 48 - 40, 8, (uint64_t)info->xfer_pct, 100, C_ACCENT);
        char pc[8];
        snprintf(pc, sizeof pc, "%d%%", info->xfer_pct);
        gfx_text(SIDEBAR_W - 24, y + 62, 12, GW_MED, C_DIM, GA_RIGHT, pc);
        ui_hit_keys(12, y, SIDEBAR_W - 24, 82, UI_BTN_START);            /* tap the progress card: show the transfer */
    }
    gfx_norec(false);
    int rx = CONTENT_R;
    if (info && info->battery >= 0) rx -= ui_battery(rx, 22, info->battery, info->charging) + 18;
    if (info && info->clock_h >= 0) ui_clock(rx, 22, info->clock_h, info->clock_m);
}

int ui_battery(int right_x, int cy, int pct, bool charging)
{
    if (pct < 0) return 0;
    if (pct > 100) pct = 100;
    gcol col = charging ? C_GOOD : (pct <= 15 ? C_BAD : (pct <= 30 ? C_WARN : C_DIM));
    int bw = 28, bh = 14, x = right_x - 3 - bw;                 /* body; the little knob on the right is 3 px wide */
    gfx_rrect(x, cy - bh / 2, bw, bh, 3, col);
    gfx_rrect(x + 2, cy - bh / 2 + 2, bw - 4, bh - 4, 2, C_BG);
    int fill = (bw - 8) * pct / 100;
    if (fill < 2 && pct > 0) fill = 2;
    if (fill > 0) gfx_rrect(x + 4, cy - bh / 2 + 4, fill, bh - 8, 1, col);
    gfx_rect(right_x - 3, cy - 3, 3, 6, col);
    char t[8];
    snprintf(t, sizeof t, "%d%%", pct);
    int tw = gfx_text(x - 8, cy, 14, GW_MED, (charging || pct <= 30) ? col : C_TEXT, GA_RIGHT, t);
    return bw + 3 + 8 + tw;
}

int ui_clock(int right_x, int cy, int hour, int minute)
{
    char t[8];
    snprintf(t, sizeof t, "%02d:%02d", hour % 24, minute % 60);
    return gfx_text(right_x, cy, 14, GW_MED, C_TEXT, GA_RIGHT, t);
}

void ui_page_title(const char *title, const char *right)
{
    gfx_text_fit(CONTENT_X, 46, CONTENT_W - 140, 26, GW_BOLD, C_TEXT, GA_LEFT, title);
    if (right && *right) gfx_text_fit(CONTENT_R, 48, 260, 15, GW_REG, C_DIM, GA_RIGHT, right);
}

void ui_hints(const char *spec)
{
    if (s_side_focus) {
        bool lr = s_cur_tab == TAB_RECEIVE;
        if (s_offer_wifi) spec = wifi_get() == WIFI_FAILED ? (lr ? "DUD Switch tab|X Back to page|TRI Wi-Fi settings|LR Tabs" : "DUD Switch tab|X Back to page|TRI Wi-Fi settings")
                                                           : (lr ? "DUD Switch tab|X Back to page|TRI Turn on Wi-Fi|LR Tabs" : "DUD Switch tab|X Back to page|TRI Turn on Wi-Fi");
        else spec = lr ? "DUD Switch tab|X Back to page|LR Tabs" : "DUD Switch tab|X Back to page";
    }
    static const struct { const char *code; int a, b; } map[] = {
        { "X", PB_CROSS, -1 }, { "O", PB_CIRCLE, -1 }, { "SQ", PB_SQUARE, -1 }, { "TRI", PB_TRIANGLE, -1 },
        { "L", PB_L, -1 }, { "R", PB_R, -1 }, { "LR", PB_L, PB_R }, { "START", PB_START, -1 }, { "SEL", PB_SELECT, -1 },
        { "DLR", PB_DPAD_LR, -1 }, { "DUD", PB_DPAD_UD, -1 }, { "DPAD", PB_DPAD, -1 },
    };
    const int cy = HINT_Y + 22, size = 24;
    int x = 22;
    const char *p = spec;
    while (p && *p) {
        int x_start = x;
        const char *end = strchr(p, '|');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char item[80];
        if (len >= sizeof item) len = sizeof item - 1;
        memcpy(item, p, len);
        item[len] = 0;
        p = end ? end + 1 : NULL;

        char *sp = strchr(item, ' ');
        const char *label = "";
        if (sp) { *sp = 0; label = sp + 1; }
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
            if (strcmp(item, map[i].code)) continue;
            x += gfx_pad_button(map[i].a, x + gfx_pad_button_width(map[i].a, size) / 2, cy, size) + 3;
            if (map[i].b >= 0) x += gfx_pad_button(map[i].b, x + gfx_pad_button_width(map[i].b, size) / 2, cy, size) + 3;
            break;
        }
        int x_glyphs_end = x;
        x += 6;
        x += gfx_text(x, cy, 15, GW_MED, GRGB(176, 186, 198), GA_LEFT, label) + 26;

        /* the whole item (symbol + label) can be tapped; "LR" is split in the middle into L and R */
        int hx = x_start - 10, hw = x - 26 + 13 - hx;
        if (!strcmp(item, "LR")) {
            int mid = x_start + (x_glyphs_end - x_start) / 2;
            ui_hit_keys(hx, HINT_Y, mid - hx, GFX_H - HINT_Y, UI_BTN_L);
            ui_hit_keys(mid, HINT_Y, hx + hw - mid, GFX_H - HINT_Y, UI_BTN_R);
        } else {
            for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
                if (!strcmp(item, map[i].code) && map[i].b < 0 && pb_keys(map[i].a))
                    ui_hit_keys(hx, HINT_Y, hw, GFX_H - HINT_Y, pb_keys(map[i].a));
        }
    }
}

void ui_card(int x, int y, int w, int h, bool focused)
{
    gfx_rrect(x, y, w, h, 14, focused ? C_CARD_HI : C_CARD);
    if (focused) {
        gfx_rrect_outline(x, y, w, h, 14, 2, C_ACCENT);
    } else {
        gfx_rrect_outline(x, y, w, h, 14, 1, C_BORDER);
    }
}

int ui_chip(int right_x, int cy, const char *text, gcol col)
{
    int w = gfx_text_width(13, GW_MED, text) + 22;
    gfx_rrect(right_x - w, cy - 12, w, 24, 12, with_alpha(col, 44));
    gfx_text(right_x - w / 2, cy, 13, GW_MED, col, GA_CENTER, text);
    return w;
}

int ui_action_width(int pad_button, const char *label)
{
    return 8 + gfx_pad_button_width(pad_button, 22) + 8 + gfx_text_width(15, GW_MED, label) + 16;
}

int ui_action(int x, int cy, int pad_button, const char *label)
{
    int gw = gfx_pad_button_width(pad_button, 22);
    int tw = gfx_text_width(15, GW_MED, label);
    int w = 8 + gw + 8 + tw + 16;
    gfx_rrect(x, cy - 17, w, 34, 17, GRGB(36, 45, 54));
    gfx_rrect_outline(x, cy - 17, w, 34, 17, 1, C_BORDER);
    gfx_pad_button(pad_button, x + 8 + gw / 2 + 1, cy, 22);
    gfx_text(x + 8 + gw + 8, cy, 15, GW_MED, C_TEXT, GA_LEFT, label);
    if (pb_keys(pad_button)) ui_hit_keys(x - 3, cy - 24, w + 6, 48, pb_keys(pad_button));
    return w;
}

void ui_progress(int x, int y, int w, int h, uint64_t done, uint64_t total, gcol col)
{
    gfx_rrect(x, y, w, h, h / 2, GRGB(38, 47, 56));
    if (total == 0 || done == 0) return;
    if (done > total) done = total;
    int fw = (int)((uint64_t)w * done / total);
    if (fw < h) fw = h;
    gfx_rrect(x, y, fw, h, h / 2, col);
}

void ui_scrollbar(int x, int y, int h, int top, int rows, int count)
{
    if (count <= rows || rows <= 0) return;
    gfx_rrect(x, y, 4, h, 2, GRGB(30, 38, 45));
    int th = h * rows / count;
    if (th < 16) th = 16;
    int ty = y + (h - th) * top / (count - rows);
    gfx_rrect(x, ty, 4, th, 2, C_FAINT);
}

void ui_toast(const char *text)
{
    int size = 16;
    int maxw = 700;
    int tw = gfx_text_width(size, GW_MED, text);
    if (tw > maxw) tw = maxw;
    int w = tw + 48, h = 42, x = (GFX_W - w) / 2, y = HINT_Y - 18 - h;
    gfx_shadow(x, y, w, h, 21);
    gfx_rrect(x, y, w, h, 21, GRGB(54, 63, 74));
    gfx_text_fit(GFX_W / 2, y + h / 2, maxw, size, GW_MED, C_TEXT, GA_CENTER, text);
}

static uint32_t *s_dlg_bg;
static int       s_dlg_key = -1;

void ui_dialog_invalidate(void) { s_dlg_key = -1; }

void ui_dialog_begin(int key, int w, int h, void (*under)(void *), void *ctx, int *x, int *y)
{
    key = key * 4 + s_theme_id;                            /* a theme change must not reuse the old picture */
    int dx = (GFX_W - w) / 2, dy = (HINT_Y - h) / 2;
    *x = dx;
    *y = dy;
    size_t bytes = (size_t)GFX_W * GFX_H * sizeof(uint32_t);
    if (s_dlg_bg && key == s_dlg_key) {
        memcpy(gfx_pixels(), s_dlg_bg, bytes);
        ui_hit_reset();                                    /* what is behind the dialog cannot be tapped */
        return;
    }
    if (under) {
        gfx_norec(true);                                   /* what is behind the dialog is not part of the text dump */
        under(ctx);
        gfx_norec(false);
    }
    ui_hit_reset();
    gfx_dim(175);
    gfx_rect(0, HINT_Y, GFX_W, GFX_H - HINT_Y, C_SIDEBAR);          /* the hint bar is redrawn by the dialog */
    if (g_th.divider) gfx_rect(0, HINT_Y, GFX_W, 1, C_BORDER);
    gfx_shadow(dx, dy, w, h, 18);
    gfx_rrect(dx, dy, w, h, 18, C_CARD);
    gfx_rrect_outline(dx, dy, w, h, 18, 1, C_BORDER);
    if (!s_dlg_bg) s_dlg_bg = malloc(bytes);
    if (s_dlg_bg) {
        memcpy(s_dlg_bg, gfx_pixels(), bytes);
        s_dlg_key = key;
    }
}

void ui_device_icon(const char *type, const char *model, float cx, float cy, float size, gcol c)
{
    int ic = IC_DESKTOP;
    if (model && strstr(model, "Vita")) ic = IC_VITA;
    else if (type && !strcmp(type, "mobile")) ic = IC_PHONE;
    else if (type && !strcmp(type, "web")) ic = IC_WEB;
    else if (type && (!strcmp(type, "headless") || !strcmp(type, "server"))) ic = IC_SERVER;
    gfx_icon(ic, cx, cy, size, c);
}

static void boot_body(const char *status, gcol status_color)
{
    gfx_clear(C_BG);
    gfx_rrect(GFX_W / 2 - 34, 30, 68, 68, 20, C_ACCENT);
    gfx_icon(IC_LOGO, (float)GFX_W / 2, 64, 56, GRGB(255, 255, 255));
    gfx_text(GFX_W / 2, 126, 30, GW_BOLD, C_TEXT, GA_CENTER, "VitaSend");
    gfx_text(GFX_W / 2, 154, 14, GW_REG, C_DIM, GA_CENTER, "for PlayStation Vita  -  v" LS_APP_VERSION " by trashman");
    gfx_text_fit(GFX_W / 2, 196, 840, 19, GW_MED, status_color, GA_CENTER, status ? status : "");

    ui_card(80, 224, 800, 262, false);
    static char lines[16][LS_LOG_LINE_LEN];
    int n = ls_log_tail(lines, 16);
    for (int i = 0; i < n; i++) {
        const char *l = lines[i];
        gcol col = C_DIM;
        if (strstr(l, "FAIL") || strstr(l, "failed") || strstr(l, "error")) col = C_BAD;
        else if (strstr(l, "OK ") || strstr(l, "registered") || strstr(l, "found")) col = C_GOOD;
        gfx_text_fit(100, 244 + i * 15, 760, 12, GW_REG, col, GA_LEFT, l);
    }
    gfx_rect(0, HINT_Y, GFX_W, GFX_H - HINT_Y, C_SIDEBAR);
}

void ui_boot_screen(const char *status, gcol status_color, const char *footer)
{
    gfx_begin();
    boot_body(status, status_color);
    ui_hints(footer);
    gfx_end();
}

void ui_spinner(int cx, int cy, int r, unsigned ms, gcol col)
{
    const int dots = 10;
    int head = (int)((ms / 90u) % (unsigned)dots);
    for (int i = 0; i < dots; i++) {
        float a = (float)i * 6.2831853f / (float)dots - 1.5708f;
        int age = (head - i + dots) % dots;                      /* 0 = the newest dot */
        int alpha = 255 - age * 22;
        gfx_circle((float)cx + (float)r * cosf(a), (float)cy + (float)r * sinf(a), 2.2f + (age == 0 ? 0.6f : 0), with_alpha(col, alpha));
    }
}

void ui_wifi_dialog(int state, unsigned ms, const char *note)
{
    gfx_begin();
    gfx_norec(true);
    boot_body("", C_DIM);
    gfx_norec(false);
    ui_hit_reset();
    gfx_dim(175);
    gfx_rect(0, HINT_Y, GFX_W, GFX_H - HINT_Y, C_SIDEBAR);
    if (g_th.divider) gfx_rect(0, HINT_Y, GFX_W, 1, C_BORDER);

    int w = 620, h = state == UI_WIFI_FAILED ? 312 : 176;
    int dx = (GFX_W - w) / 2, dy = (HINT_Y - h) / 2;
    gfx_shadow(dx, dy, w, h, 18);
    gfx_rrect(dx, dy, w, h, 18, C_CARD);
    gfx_rrect_outline(dx, dy, w, h, 18, 1, C_BORDER);

    int lx = dx + 32, tw = w - 64 - 72;
    int icx = lx + 28, icy = dy + 58;
    gfx_circle((float)icx, (float)icy, 28, with_alpha(C_WARN, 44));
    const char *title, *sub;
    if (state == UI_WIFI_BUSY) {
        title = "Turning on Wi-Fi...";
        sub = "Connecting to your network";
        ui_spinner(icx, icy, 13, ms, C_WARN);
    } else {
        gfx_icon(IC_WIFI_OFF, (float)icx, (float)icy + 0.6f, 24, C_WARN);       /* (the symbol's own centre sits a hair above its box) */
        if (state == UI_WIFI_FAILED) { title = "Wi-Fi could not be turned on"; sub = "The Vita did not allow it from inside the app."; }
        else                         { title = "Wi-Fi is turned off";          sub = "VitaSend needs Wi-Fi to find other devices."; }
    }
    gfx_text_fit(lx + 72, dy + 46, tw, 22, GW_BOLD, C_TEXT, GA_LEFT, title);
    gfx_text_fit(lx + 72, dy + 74, tw, 15, GW_REG, C_DIM, GA_LEFT, sub);

    int by = dy + h - 44;
    if (state == UI_WIFI_BUSY) {
        int bw = w - 64, bx = lx, bar_y = dy + 96;
        gfx_rrect(bx, bar_y, bw, 6, 3, GRGB(38, 47, 56));
        int seg = bw / 4, pos = (int)((ms / 6u) % (unsigned)(bw + seg)) - seg;       /* a piece running to and fro */
        int x0 = pos < 0 ? 0 : pos, x1 = pos + seg > bw ? bw : pos + seg;
        if (x1 > x0) gfx_rrect(bx + x0, bar_y, x1 - x0, 6, 3, C_ACCENT);
        ui_action(lx, by, PB_CIRCLE, "Cancel");
        ui_hints("O Cancel");
    } else if (state == UI_WIFI_FAILED) {
        gfx_line_t ln[3];
        const char *msg = "Turn it on in the Vita settings (Settings > Network). VitaSend keeps waiting and continues by itself.";
        int n = gfx_wrap(msg, 15, GW_REG, w - 64, ln, 3);
        for (int i = 0; i < n; i++) {
            char tmp[160];
            int l = ln[i].len < (int)sizeof tmp - 1 ? ln[i].len : (int)sizeof tmp - 1;
            memcpy(tmp, ln[i].p, (size_t)l);
            tmp[l] = 0;
            gfx_text(lx, dy + 116 + i * 22, 15, GW_REG, C_TEXT, GA_LEFT, tmp);
        }
        if (note && note[0]) gfx_text_fit(lx, dy + 116 + n * 22 + 8, w - 64, 14, GW_MED, C_WARN, GA_LEFT, note);
        else gfx_text_fit(lx, dy + 116 + n * 22 + 8, w - 64, 14, GW_REG, C_DIM, GA_LEFT, "TRIANGLE opens the Wi-Fi settings (VitaSend closes; start it again afterwards).");
        int row1 = dy + h - 88;
        int a = ui_action(lx, row1, PB_CROSS, "Try again");
        ui_action(lx + a + 12, row1, PB_TRIANGLE, "Wi-Fi settings");
        a = ui_action(lx, by, PB_SQUARE, "Continue without Wi-Fi");
        ui_action(lx + a + 12, by, PB_CIRCLE, "Quit");
        ui_hints("X Try again|TRI Wi-Fi settings|SQ Continue without Wi-Fi|O Quit");
    } else {
        int a = ui_action(lx, by, PB_CROSS, "Turn on Wi-Fi");
        a += 12 + ui_action(lx + a + 12, by, PB_SQUARE, "Continue without Wi-Fi");
        ui_action(lx + a + 12, by, PB_CIRCLE, "Quit");
        ui_hints("X Turn on Wi-Fi|SQ Continue without Wi-Fi|O Quit");
    }
    gfx_end();
}

/* ============================================================== list cursor */

void ui_cursor_clamp(ui_cursor *c, int count, int rows)
{
    if (count <= 0) { c->sel = c->top = 0; return; }
    if (c->sel < 0) c->sel = 0;
    if (c->sel >= count) c->sel = count - 1;
    if (c->sel < c->top) c->top = c->sel;
    if (c->sel >= c->top + rows) c->top = c->sel - rows + 1;
    if (c->top < 0) c->top = 0;
}

void ui_cursor_move(ui_cursor *c, int delta, int count, int rows)
{
    c->sel += delta;
    ui_cursor_clamp(c, count, rows);
}

/* ============================================================== on-screen keyboard */

#define OSK_COLS  14
#define OSK_ROWS  7
#define OSK_PAGES 4

/* Page 0 must stay exactly like this: the test script types with it. */
static const char *const k_pages[OSK_PAGES][OSK_ROWS] = {
    { "1234567890-_=+", "qwertyuiop[]()", "asdfghjkl;:'\"/", "zxcvbnm,.?!@#$", "QWERTYUIOP{}<>", "ASDFGHJKL%^&*\\", "ZXCVBNM`~|    " },
    { "àáâãäåæçèéêëìí", "îïðñòóôõöøùúûü", "ýþżßœšžłąęćńśź", "ÀÁÂÃÄÅÆÇÈÉÊËÌÍ", "ÎÏÐÑÒÓÔÕÖØÙÚÛÜ", "ÝÞŻŒŠŽŁĄĘĆŃŚŹŸ", "ğışİĞŞ€£©°…—¿¡" },
    { "абвгдежзийклмн", "опрстуфхцчшщъы", "ьэюяёіїєґў’-.,", "АБВГДЕЖЗИЙКЛМН", "ОПРСТУФХЦЧШЩЪЫ", "ЬЭЮЯЁІЇЄҐЎ?!:;", "~\"'()@#$%&*+=/" },
    { "αβγδεζηθικλμνξ", "οπρσςτυφχψωάέή", "ίόύώϊϋΐΰ;·,.?!", "ΑΒΓΔΕΖΗΘΙΚΛΜΝΞ", "ΟΠΡΣΤΥΦΧΨΩΆΈΉΊ", "ΌΎΏΪΫ~\"'()@#$%", "&*/\\:;<>[]{}|`" },
};
static const char *const k_page_names[OSK_PAGES] = { "Abc", "Áé", "Бв", "Αβ" };

static bool osk_cell(int page, int row, int col, char out[5])
{
    out[0] = 0;
    if (page < 0 || page >= OSK_PAGES || row < 0 || row >= OSK_ROWS || col < 0 || col >= OSK_COLS) return false;
    const char *p = k_pages[page][row];
    for (int i = 0; i <= col; i++) {
        if (!*p) return false;
        size_t n = 1;
        unsigned char c = (unsigned char)*p;
        if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
        if (i == col) { memcpy(out, p, n); out[n] = 0; return true; }
        p += n;
    }
    return false;
}

void osk_open(osk_state *o, const char *title, const char *initial, int max_len, bool digits_only)
{
    memset(o, 0, sizeof *o);
    ls_strlcpy(o->title, title, sizeof o->title);
    if (initial) ls_strlcpy(o->text, initial, sizeof o->text);
    o->max_len = max_len > OSK_MAX ? OSK_MAX : max_len;
    o->digits_only = digits_only;
}

static bool osk_key_at(const osk_state *o, int col, int row, char out[5])
{
    if (o->digits_only) {
        if (col < 0 || col >= 10 || row != 0) { out[0] = 0; return false; }
        out[0] = "1234567890"[col];
        out[1] = 0;
        return true;
    }
    return osk_cell(o->page, row, col, out);
}

int osk_handle(osk_state *o, uint32_t pressed)
{
    int cols = o->digits_only ? 10 : OSK_COLS;
    int rows = o->digits_only ? 1 : OSK_ROWS;
    if (pressed & UI_BTN_LEFT)  o->col = (o->col + cols - 1) % cols;
    if (pressed & UI_BTN_RIGHT) o->col = (o->col + 1) % cols;
    if (pressed & UI_BTN_UP)    o->row = (o->row + rows - 1) % rows;
    if (pressed & UI_BTN_DOWN)  o->row = (o->row + 1) % rows;
    if (!o->digits_only) {
        if (pressed & UI_BTN_L) o->page = (o->page + OSK_PAGES - 1) % OSK_PAGES;
        if (pressed & UI_BTN_R) o->page = (o->page + 1) % OSK_PAGES;
    }

    size_t len = strlen(o->text);
    if (pressed & UI_BTN_CROSS) {
        char key[5];
        if (osk_key_at(o, o->col, o->row, key) && key[0] && (int)(len + strlen(key)) <= o->max_len)
            memcpy(o->text + len, key, strlen(key) + 1);
    }
    if ((pressed & UI_BTN_TRIANGLE) && !o->digits_only && (int)len < o->max_len) {
        o->text[len] = ' ';
        o->text[len + 1] = 0;
    }
    if ((pressed & UI_BTN_SQUARE) && len > 0) o->text[len - gfx_utf8_prev(o->text, len)] = 0;
    if (pressed & UI_BTN_START) return OSK_OK;
    if (pressed & UI_BTN_CIRCLE) return OSK_CANCEL;
    return OSK_CONTINUE;
}

/* touch: a tap on a key types it; a tap on an alphabet tab switches to it */
static void osk_tap_key(void *ctx, int arg)
{
    osk_state *o = ctx;
    o->row = arg >> 8;
    o->col = arg & 255;
    osk_handle(o, UI_BTN_CROSS);
}

static void osk_tap_page(void *ctx, int arg)
{
    ((osk_state *)ctx)->page = arg;
}

void osk_draw(const osk_state *o, bool secret)
{
    gfx_clear(C_BG);
    gfx_rect(0, HINT_Y, GFX_W, GFX_H - HINT_Y, C_SIDEBAR);
    gfx_text_fit(48, 44, 640, 24, GW_BOLD, C_TEXT, GA_LEFT, o->title);

    if (!o->digits_only) {                                              /* page tabs */
        int x = GFX_W - 48;
        for (int i = OSK_PAGES - 1; i >= 0; i--) {
            int w = gfx_text_width(16, GW_MED, k_page_names[i]) + 28;
            x -= w;
            ui_hit_call(x - 3, 18, w + 6, 52, osk_tap_page, (void *)o, i);
            if (i == o->page) gfx_rrect(x, 28, w, 32, 16, C_ACCENT_BG);
            gfx_text(x + w / 2, 44, 16, GW_MED, i == o->page ? C_ACCENT_HI : C_DIM, GA_CENTER, k_page_names[i]);
            x -= 6;
        }
    }

    /* the text field: show the end of the text when it is long */
    ui_card(48, 74, GFX_W - 96, 56, true);
    char shown[OSK_MAX + 1];
    size_t len = strlen(o->text);
    if (secret) {
        size_t k = 0;
        for (size_t i = 0; i < len && k + 1 < sizeof shown; i++) if (((unsigned char)o->text[i] & 0xC0) != 0x80) shown[k++] = '*';
        shown[k] = 0;
    } else {
        memcpy(shown, o->text, len + 1);
    }
    const char *start = shown;
    int maxw = GFX_W - 96 - 40 - 90;
    while (*start && gfx_text_width(22, GW_REG, start) > maxw) {
        start++;
        while (((unsigned char)*start & 0xC0) == 0x80) start++;
    }
    int tw = gfx_text(70, 102, 22, GW_REG, C_TEXT, GA_LEFT, start);
    gfx_rect(70 + tw + 2, 90, 2, 24, C_ACCENT_HI);
    char count[32];
    snprintf(count, sizeof count, "%d / %d", (int)len, o->max_len);
    gfx_text(GFX_W - 48 - 20, 102, 14, GW_REG, C_DIM, GA_RIGHT, count);

    int rows = o->digits_only ? 1 : OSK_ROWS;
    int cols = o->digits_only ? 10 : OSK_COLS;
    int kw = o->digits_only ? 64 : 54, kh = o->digits_only ? 56 : 40, gap = 6;
    int x0 = (GFX_W - (cols * kw + (cols - 1) * gap)) / 2;
    int y0 = o->digits_only ? 190 : 156;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            bool sel = (r == o->row && c == o->col);
            int x = x0 + c * (kw + gap), y = y0 + r * (kh + gap);
            if (!o->digits_only && o->page == 0 && r == OSK_ROWS - 1 && c >= 10) {           /* the space bar spans four keys */
                if (c != 10) continue;
                bool bar_sel = (o->row == r && o->col >= 10);
                int w = 4 * kw + 3 * gap;
                ui_hit_call(x - 3, y - 3, w + 6, kh + 6, osk_tap_key, (void *)o, (r << 8) | c);
                gfx_rrect(x, y, w, kh, 10, bar_sel ? C_ACCENT : GRGB(36, 45, 54));
                gfx_text(x + w / 2, y + kh / 2, 16, GW_MED, bar_sel ? C_ON_ACCENT : C_DIM, GA_CENTER, "Space");
                continue;
            }
            char key[5];
            osk_key_at(o, c, r, key);
            if (key[0]) ui_hit_call(x - 3, y - 3, kw + 6, kh + 6, osk_tap_key, (void *)o, (r << 8) | c);
            gfx_rrect(x, y, kw, kh, 10, sel ? C_ACCENT : GRGB(36, 45, 54));
            if (key[0] && strcmp(key, " ")) {
                gfx_norec(true);                                           /* keys are not part of the text dump */
                gfx_text(x + kw / 2, y + kh / 2, o->digits_only ? 24 : 20, GW_MED, sel ? C_ON_ACCENT : C_TEXT, GA_CENTER, key);
                gfx_norec(false);
            }
        }
    }
    if (o->digits_only) gfx_text(GFX_W / 2, y0 + kh + 40, 14, GW_REG, C_DIM, GA_CENTER, "Digits only");

    ui_hints(o->digits_only ? "DPAD Move|X Type|SQ Delete|START Done|O Cancel"
                            : "DPAD Move|X Type|SQ Delete|TRI Space|LR Alphabet|START Done|O Cancel");
}
