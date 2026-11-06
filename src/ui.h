/*
 * ui.h - look and feel: colours, the sidebar, cards, chips, button hints, dialogs and the on-screen
 * keyboard. The screens themselves are in app.c.
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_UI_H
#define LS_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

/* ---- colours: two themes, chosen in the settings ----
 * THEME_DARK  the original dark blue-grey look with the teal accent
 * THEME_OLED  like the desktop app's OLED mode: pure black background (the Vita's OLED switches those pixels off) */
enum { THEME_DARK, THEME_OLED, THEME_COUNT };
typedef struct {
    gcol bg, sidebar, card, card_hi, border, text, dim, faint, accent, accent_hi, accent_bg, on_accent;
    bool divider;                   /* draw thin lines between sidebar / page / hint bar (they have the same black otherwise) */
} ui_theme;
extern ui_theme g_th;
void ui_set_theme(int theme);       /* THEME_*; anything else = dark */
int  ui_theme_id(void);
const char *ui_theme_name(int theme);

#define C_BG        (g_th.bg)
#define C_SIDEBAR   (g_th.sidebar)
#define C_CARD      (g_th.card)
#define C_CARD_HI   (g_th.card_hi)
#define C_BORDER    (g_th.border)
#define C_TEXT      (g_th.text)
#define C_DIM       (g_th.dim)
#define C_FAINT     (g_th.faint)
#define C_ACCENT    (g_th.accent)
#define C_ACCENT_HI (g_th.accent_hi)
#define C_ACCENT_BG (g_th.accent_bg)
#define C_ON_ACCENT (g_th.on_accent)
#define C_GOOD      GRGB(102, 187, 106)
#define C_WARN      GRGB(255, 193, 77)
#define C_BAD       GRGB(239, 83, 80)

/* ---- layout ---- */
enum { TAB_RECEIVE, TAB_SEND, TAB_HISTORY, TAB_FILES, TAB_SETTINGS, TAB_COUNT };
#define SIDEBAR_W  212
#define CONTENT_X  236
#define CONTENT_R  936                                  /* right edge of the content area */
#define CONTENT_W  (CONTENT_R - CONTENT_X)
#define CONTENT_Y  86                                   /* first pixel row below the page title */
#define CONTENT_B  488                                  /* bottom of the content area */
#define HINT_Y     500

/* "1.5 MB" style */
void fmt_size(uint64_t bytes, char *out, size_t cap);
/* "10-17 12:03" in UTC, or "-- --:--" if the clock is unknown */
void fmt_clock(int64_t unix_time, char *out, size_t cap);
/* "2 min", "35 s", "1 h 5 min" */
void fmt_duration(uint64_t seconds, char *out, size_t cap);

/* ---- chrome ---- */
typedef struct {
    const char *ip;                 /* "" = no Wi-Fi                                     */
    const char *xfer_title;         /* NULL = no transfer running; else "Receiving"...   */
    const char *xfer_peer;
    int         xfer_pct;
    int         battery;            /* percent, -1 = unknown (nothing is drawn) */
    bool        charging;
    int         clock_h, clock_m;   /* wall-clock time shown left of the battery; clock_h < 0 = not shown */
} ui_side_info;

/* Background, sidebar (with the active tab marked) and the empty hint bar. */
void ui_draw_chrome(int tab, const ui_side_info *info);
/* The D-pad focus is on the tab list: outline around the current tab, and the hint bar says what the D-pad does there. Set it
 * every frame before drawing (false while a dialog is open). */
void ui_set_side_focus(bool on);
void ui_page_title(const char *title, const char *right);
/* Battery symbol with the percentage on its left; (right_x, cy) is its right-middle point. Returns the width. */
int  ui_battery(int right_x, int cy, int percent, bool charging);
/* "HH:MM" in the same size and font as the battery percentage; (right_x, cy) is its right-middle point. Returns the width. */
int  ui_clock(int right_x, int cy, int hour, int minute);
/* Hint bar: items separated by '|', each "CODE Label" with CODE one of X O SQ TRI L R LR START SEL DLR DUD DPAD. */
void ui_hints(const char *spec);
void ui_card(int x, int y, int w, int h, bool focused);
/* A rounded label; (right_x, cy) is its right-middle point. Returns its width. */
int  ui_chip(int right_x, int cy, const char *text, gcol col);
/* A button-shaped hint with the PlayStation symbol: left edge x. Returns the width. */
int  ui_action(int x, int cy, int pad_button, const char *label);
int  ui_action_width(int pad_button, const char *label);
void ui_progress(int x, int y, int w, int h, uint64_t done, uint64_t total, gcol col);
void ui_scrollbar(int x, int y, int h, int top, int rows, int count);
void ui_toast(const char *text);
/* Starts a dialog: the screen behind it (drawn by `under`) is dimmed and an empty card is drawn centred; returns the
 * card's top-left corner. The result is cached under `key` (any number that changes when the background changes), so
 * later frames only copy it back instead of drawing everything again. */
void ui_dialog_invalidate(void);                 /* forget the cached background (call when a dialog opens) */
void ui_dialog_begin(int key, int w, int h, void (*under)(void *), void *ctx, int *x, int *y);
/* Device-type icon: "mobile", "desktop", "web", "headless", "server" or a model containing "Vita". */
void ui_device_icon(const char *type, const char *model, float cx, float cy, float size, gcol c);
/* Plain start-up / failure screen. */
void ui_boot_screen(const char *status, gcol status_color, const char *footer);
/* Start-up question "Wi-Fi is turned off" on top of the boot screen. state: 0 = ask, 1 = switching it on, 2 = could not.
 * ms = any ever-growing time (animates the spinner). Registers the buttons for touch. */
enum { UI_WIFI_ASK, UI_WIFI_BUSY, UI_WIFI_FAILED };
void ui_wifi_dialog(int state, unsigned ms, const char *note);   /* note: a short extra line (failed state), or NULL */
/* A turning dot ring (something is going on); (cx, cy) is its centre. */
void ui_spinner(int cx, int cy, int r, unsigned ms, gcol col);

/* Scrolling list bookkeeping: keeps `sel` visible inside a window of `rows` rows. */
typedef struct { int sel, top; } ui_cursor;
void ui_cursor_move(ui_cursor *c, int delta, int count, int rows);
void ui_cursor_clamp(ui_cursor *c, int count, int rows);

/* ---- touch: areas that react to a finger ----
 * Every frame the screens register what can be tapped while they draw it (the table is cleared at the start of a
 * frame). Areas registered later lie on top of earlier ones. A tap is turned into the same button presses the
 * controller would give, so the screens need no separate touch logic. */
typedef void (*ui_hit_fn)(void *ctx, int arg);
void ui_hit_reset(void);
/* A tap presses these buttons (UI_BTN_* bits). */
void ui_hit_keys(int x, int y, int w, int h, uint32_t keys);
/* A tap calls fn(ctx, arg). */
void ui_hit_call(int x, int y, int w, int h, ui_hit_fn fn, void *ctx, int arg);
/* A list row: a tap selects it in `c`, then `keys` are pressed (0 = just select). */
void ui_hit_row(int x, int y, int w, int h, ui_cursor *c, int index, uint32_t keys);
/* A scrollable area: dragging a finger over it moves `*top` (the first visible row; each rowh pixels = one row). If `sel`
 * is given it is kept inside the visible rows. */
void ui_hit_scroll(int x, int y, int w, int h, int *top, int *sel, int count, int rows, int rowh);
/* Sidebar taps call this with the tab number (TAB_*). */
void ui_set_tab_handler(void (*fn)(int tab));

/* Feeding finger events in (see touch.h). A tap returns the buttons to press (0 = none). */
void     ui_input_refresh(void);                  /* re-arms controller + touch sampling (the system installer may reset them) */
uint32_t ui_touch_tap(int x, int y);
bool     ui_touch_drag_begin(int x, int y);       /* false: nothing scrollable below the finger */
void     ui_touch_drag(int dy);                   /* dy = finger movement since the touch began */
void     ui_touch_drag_end(void);
/* For the PC tests: the current areas as text, one per line: "K x y w h keys" / "C x y w h arg" / "R x y w h index" / "S x y w h". */
size_t   ui_hit_dump(char *out, size_t cap);

/* ---- on-screen keyboard ---- */
#define OSK_MAX 400
typedef struct {
    char title[64];
    char text[OSK_MAX + 1];
    int  max_len;                  /* in bytes */
    bool digits_only;
    int  page;                     /* 0 = ASCII, 1 = accented Latin, 2 = Cyrillic, 3 = Greek */
    int  col, row;                 /* cursor in the key grid */
} osk_state;

enum { OSK_CONTINUE = 0, OSK_OK, OSK_CANCEL };
void osk_open(osk_state *o, const char *title, const char *initial, int max_len, bool digits_only);
int  osk_handle(osk_state *o, uint32_t pressed);       /* returns OSK_* */
void osk_draw(const osk_state *o, bool secret);

#endif
