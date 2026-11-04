/*
 * app_int.h - what the files of the user interface share (app.c, app_files.c, app_pages.c, app_settings.c, app_dialogs.c,
 * app_install.c, app_saver.c). Nothing outside src/app*.c includes this; the outside world only sees app.h.
 *
 *   app.c           main loop, tab keys, touch / button input, events from the other threads, drawing dispatch
 *   app_pages.c     Receive (with the "This device" details), Send, History
 *   app_files.c     the file browser for sending, and the Files tab with its options menu
 *   app_settings.c  Settings, log, dropdown lists
 *   app_dialogs.c   on-screen keyboard, message viewer, incoming request, transfer, confirmations
 *   app_install.c   installing .vpk packages (one, or a queue)
 *   app_saver.c     the screensaver
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_APP_INT_H
#define LS_APP_INT_H

#include "app.h"
#include "discovery.h"
#include "history.h"
#include "identity.h"
#include "peers.h"
#include "plat.h"
#include "rx.h"
#include "send.h"
#include "settings.h"
#include "ui.h"
#include "touch.h"
#include "util.h"
#include "vpk.h"
#include "xfer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- screens ---- */

typedef enum {
    SC_HOME = TAB_RECEIVE, SC_SEND = TAB_SEND, SC_HISTORY = TAB_HISTORY, SC_FILES = TAB_FILES, SC_SETTINGS = TAB_SETTINGS,   /* the tabs */
    SC_BROWSER, SC_OSK, SC_LOG,
    SC_MESSAGE, SC_REQUEST, SC_TRANSFER, SC_CONFIRM, SC_INSTALL, SC_MENU, SC_PICK, SC_INFO     /* dialogs on top of a page */
} screen_id;

typedef enum { OP_ALIAS, OP_PIN_SET, OP_SEND_PIN, OP_MESSAGE, OP_ADD_IP, OP_RENAME, OP_NEWDIR } osk_purpose;
typedef enum { BM_SEND, BM_PICK_FOLDER } browser_mode;
typedef enum { CF_QUIT, CF_CANCEL_XFER, CF_DELETE, CF_SETTING } confirm_what;

#define RX_VPK_MAX_UI 8
#define MAX_SEL  64
#define PATH_CAP 300

/* ---- the state of the whole interface ---- */

typedef struct {
    screen_id    sc;                 /* what is on screen                                      */
    screen_id    tab;                /* the tab underneath (always SC_HOME..SC_SETTINGS)       */
    screen_id    ret;                /* where to go after a dialog that interrupted something  */
    bool         quit;
    bool         server_ok;
    char         ip[16];
    uint32_t     seen_serial;        /* last transfer we jumped to */
    bool         pin_answered;
    bool         saver;              /* the screensaver is showing (on top of whatever screen is current)  */
    uint64_t     last_input;         /* when a button / finger was last used                                */
    uint64_t     saver_t;            /* time of the last saver animation step                                */
    bool         side_focus;         /* the D-pad focus is on the tab list (LEFT goes there, RIGHT / X / O comes back) */
    bool         swallow_touch;      /* the finger that woke the screensaver is still down: its tap / drag does nothing */
    bool         touch_down;         /* a finger is on the screen right now                                  */
    float        sx, sy, vx, vy;     /* screensaver: position of the moving box (top left) and speed in px/s */
} app_state;

extern app_state A;

/* ---- a folder listing (the "send" picker and the Files tab) ---- */

typedef struct {
    plat_dirent *ent;
    int          nent;
    char         path[PATH_CAP];            /* "" = list of storage devices */
    ui_cursor    cur;
    uint64_t     free_bytes;             /* free space of the storage that holds `path`, UINT64_MAX = unknown / no folder */
} fbrowser;

/* ---- small helpers every screen uses ---- */

static inline const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    if (!s) s = strrchr(path, ':');
    return s && s[1] ? s + 1 : path;
}

static inline bool xfer_running(const xfer_status *x)
{
    return x->state == XF_WAITING || x->state == XF_ACTIVE;
}

static inline int xfer_percent(const xfer_status *x)
{
    int pct = x->bytes_total ? (int)(x->bytes_done * 100 / x->bytes_total) : 0;
    if (x->state == XF_DONE && !x->bytes_total) pct = 100;
    return pct > 100 ? 100 : pct;
}


/* ---- app.c ---- */
void go(screen_id sc);                                  /* switch to a screen (and keep the tab / focus / screensaver rules) */
void toast(const char *fmt, ...) LS_PRINTF(1, 2);       /* a short message at the bottom */
void under_tab(void *ctx);                              /* what a dialog dims behind it: the current tab */
void draw_tab(int tab, const xfer_status *x);
void draw_chrome(int tab, const xfer_status *x);        /* background, tab list, battery / clock, transfer card */
void chime_if_on(void);                                 /* the "finished" chime, unless the user switched it off */

/* ---- app_files.c ---- */
extern char s_sel[MAX_SEL][PATH_CAP];                   /* what is ticked to be sent (with s_text_to_send: the send selection) */
extern int  s_nsel;
void browser_draw(const xfer_status *x);
void browser_input(uint32_t p);
void browser_open(browser_mode mode);
void files_draw(const xfer_status *x);
void files_input(uint32_t p);
void files_ensure(void);
void files_reload(void);
bool files_name_entered(bool rename, const char *name);
void menu_draw(const xfer_status *x);
void menu_input(uint32_t p);
void do_delete(void);                                   /* after the confirmation */
void delete_question(char *q, size_t cap);

/* ---- app_pages.c ---- */
extern char s_text_to_send[OSK_MAX + 1];                /* a text message waiting to be sent ("" = none) */
const char *accept_label(int m);
const char *accept_help(int m);
void receive_draw(const xfer_status *x);
void receive_input(uint32_t p);
void info_draw(const xfer_status *x);
void info_input(uint32_t p);
void send_draw(const xfer_status *x);
void send_input(uint32_t p);
void send_page_reset(void);
void send_set_text(const char *text);
void history_draw(const xfer_status *x);
void history_input(uint32_t p);
void history_reset(void);

/* ---- app_settings.c ---- */
void settings_draw(const xfer_status *x);
void settings_input(uint32_t p);
void settings_reset(void);
void log_draw(const xfer_status *x);
void log_input(uint32_t p);
void pick_draw(const xfer_status *x);
void pick_input(uint32_t p);
void pick_apply(void);
void setting_confirm_draw(const xfer_status *x);

/* ---- app_dialogs.c ---- */
void open_osk(osk_purpose purpose, const char *title, const char *initial, int max_len, bool digits, screen_id ret);
void osk_screen_draw(void);
void osk_screen_input(uint32_t p);
screen_id osk_return_screen(void);
bool osk_is_pin_prompt(void);
void show_message(const char *title, const char *body, screen_id ret);
void message_draw(const xfer_status *x);
void message_input(uint32_t p);
bool request_pending(void);
void request_draw(const xfer_status *x);
void request_input(uint32_t p);
void transfer_draw(const xfer_status *x);
void transfer_input(uint32_t p);
void confirm_open(confirm_what what, screen_id ret);
void confirm_draw(const xfer_status *x);
void confirm_input(uint32_t p);

/* ---- app_install.c ---- */
void install_open(const char *path, screen_id ret);
void install_draw(const xfer_status *x);
void install_input(uint32_t p);
void iq_open(const char *const *paths, int n, screen_id ret, bool autostart);
bool iq_active(void);                                   /* several packages in one go: showing / running / done */
bool iq_running(void);                                  /* ... and it is installing right now */
void iq_tick(void);
void vpk_chime_events(const vpk_status *vs);

/* ---- app_saver.c ---- */
uint32_t idle_unit_ms(void);                            /* 1000 on the Vita; tests shorten it */
void saver_start(void);
void saver_stop(const char *why);
void saver_step(void);
void saver_draw(const xfer_status *x);

#endif
