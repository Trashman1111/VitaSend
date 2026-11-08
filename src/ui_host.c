/*
 * ui_host.c - PC test build: no real screen and no controller.
 *
 *   --dump-screen FILE   the text of the screen is written to FILE whenever it changes: one line per row of the
 *                        UI, a first line "@ tab=... screen=...", and '>' in column 0 for the selected row
 *                        (FILE.hits lists the touch areas: "K x y w h keys" = presses buttons, "C ..." = calls a function,
 *                        "R ..." = list row, "S ..." = scrollable area)
 *   --dump-ppm FILE      the picture itself (960x544 PPM), at most every 250 ms - to look at the design
 *   --keys "a,b,wait500" button presses to "press" at start-up
 *   --keys-fifo PATH     more button presses can be written into this named pipe while the program runs
 *
 * Key names: up down left right cross circle square triangle start select l r, and waitNNN (milliseconds).
 * Touch: tap:X:Y  (a finger touches the screen at X,Y and lifts again)
 *        drag:X1:Y1:X2:Y2  (a finger moves from X1,Y1 to X2,Y2 within about 400 ms)
 *
 * Author: trashman (VitaSend)
 */
#include "plat.h"
#include "gfx.h"
#include "ui.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned key_ms(void)                 /* LS_KEY_MS=60 makes scripted tests faster (default 150) */
{
    static unsigned v;
    if (!v) {
        const char *e = getenv("LS_KEY_MS");
        v = e && atoi(e) >= 40 ? (unsigned)atoi(e) : 150;
    }
    return v;
}
#define TOK_CAP     512

static char     s_dump_path[256], s_ppm_path[256];
static char     s_last_dump[24000];

static char     s_tok[TOK_CAP][32];
static int      s_tok_head, s_tok_tail;
static int      s_fifo = -1;
static uint32_t s_hold_mask;
static uint64_t s_hold_until, s_gap_until;
static bool     s_gap_read = true;           /* the released state was reported at least once since the last key */

void ui_host_set_dump(const char *path)
{
    snprintf(s_dump_path, sizeof s_dump_path, "%s", path);
}

void ui_host_set_ppm(const char *path)
{
    snprintf(s_ppm_path, sizeof s_ppm_path, "%s", path);
}

static void push_token(const char *t, size_t n)
{
    int next = (s_tok_tail + 1) % TOK_CAP;
    if (next == s_tok_head || n == 0 || n >= sizeof s_tok[0]) return;
    memcpy(s_tok[s_tok_tail], t, n);
    s_tok[s_tok_tail][n] = 0;
    s_tok_tail = next;
}

static void push_script(const char *s)
{
    const char *p = s;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        const char *q = p;
        while (*q && !isspace((unsigned char)*q) && *q != ',') q++;
        push_token(p, (size_t)(q - p));
        p = q;
    }
}

void ui_host_set_keys(const char *script)
{
    push_script(script);
}

void ui_host_set_key_fifo(const char *path)
{
    mkfifo(path, 0600);
    s_fifo = open(path, O_RDWR | O_NONBLOCK);            /* O_RDWR: never sees EOF when the writer closes */
}

static void pump_fifo(void)
{
    if (s_fifo < 0) return;
    static char partial[64];
    static size_t plen;
    char buf[128];
    ssize_t n = read(s_fifo, buf, sizeof buf);
    for (ssize_t i = 0; i < n; i++) {
        char c = buf[i];
        if (isspace((unsigned char)c) || c == ',') {
            if (plen) { push_token(partial, plen); plen = 0; }
        } else if (plen + 1 < sizeof partial) {
            partial[plen++] = c;
        }
    }
}

/* ---- simulated finger: started by a tap:/drag: token, played back by ui_touch() ---- */
static struct {
    int      phase;                              /* 0 idle, 1 start pending, 2 finger down, 3 last "down" sample sent, 4 gap */
    int      x1, y1, x2, y2;
    uint64_t t0, dur, gap_until;
} s_tp;

static bool touch_busy(uint64_t now)
{
    return s_tp.phase != 0 || now < s_tp.gap_until;
}

static bool start_touch_token(const char *t, uint64_t now)
{
    int a, b, c, d;
    uint64_t dur;
    if (sscanf(t, "tap:%d:%d", &a, &b) == 2) { c = a; d = b; dur = key_ms(); }
    else if (sscanf(t, "drag:%d:%d:%d:%d", &a, &b, &c, &d) == 4) dur = 400;
    else return false;
    s_tp.phase = 1;
    s_tp.x1 = a; s_tp.y1 = b; s_tp.x2 = c; s_tp.y2 = d;
    s_tp.dur = dur;
    s_tp.t0 = now;
    return true;
}

bool ui_touch(int *x, int *y)
{
    uint64_t now = plat_time_ms();
    switch (s_tp.phase) {
    case 1:
        s_tp.phase = 2;
        s_tp.t0 = now;
        *x = s_tp.x1; *y = s_tp.y1;
        return true;
    case 2: {
        uint64_t el = now - s_tp.t0;
        if (el >= s_tp.dur) {                    /* the last position is reported once, then the finger lifts */
            s_tp.phase = 3;
            *x = s_tp.x2; *y = s_tp.y2;
        } else {
            *x = s_tp.x1 + (int)((int64_t)(s_tp.x2 - s_tp.x1) * (int64_t)el / (int64_t)s_tp.dur);
            *y = s_tp.y1 + (int)((int64_t)(s_tp.y2 - s_tp.y1) * (int64_t)el / (int64_t)s_tp.dur);
        }
        return true;
    }
    case 3:
        s_tp.phase = 0;
        s_tp.gap_until = now + key_ms();
        return false;
    default:
        return false;
    }
}

static uint32_t key_mask(const char *name)
{
    static const struct { const char *n; uint32_t m; } t[] = {
        { "up", UI_BTN_UP }, { "down", UI_BTN_DOWN }, { "left", UI_BTN_LEFT }, { "right", UI_BTN_RIGHT },
        { "cross", UI_BTN_CROSS }, { "circle", UI_BTN_CIRCLE }, { "square", UI_BTN_SQUARE },
        { "triangle", UI_BTN_TRIANGLE }, { "start", UI_BTN_START }, { "select", UI_BTN_SELECT },
        { "l", UI_BTN_L }, { "r", UI_BTN_R }
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcmp(name, t[i].n)) return t[i].m;
    return 0;
}

void ui_input_refresh(void) { }

uint32_t ui_buttons(void)
{
    static int slow = -1;                        /* reading the controller / touch screen takes a few ms on a real Vita */
    if (slow < 0) { const char *e = getenv("LS_SLOW_INPUT_MS"); slow = e ? atoi(e) : 0; }
    if (slow > 0) plat_sleep_ms((uint32_t)slow);
    uint64_t now = plat_time_ms();
    pump_fifo();
    if (now < s_hold_until) return s_hold_mask;
    if (touch_busy(now)) return 0;               /* a simulated finger is on the screen: nothing else happens meanwhile */
    if (now < s_gap_until || !s_gap_read) {      /* a slow program (sanitizer build) must still see every release */
        s_gap_read = true;
        return 0;
    }
    while (s_tok_head != s_tok_tail) {
        const char *t = s_tok[s_tok_head];
        s_tok_head = (s_tok_head + 1) % TOK_CAP;
        if (!strncmp(t, "wait", 4)) {
            s_gap_until = now + (uint64_t)atoi(t + 4);
            return 0;
        }
        if (start_touch_token(t, now)) return 0;
        uint32_t m = key_mask(t);
        if (m) {
            s_hold_mask = m;
            s_gap_read = false;
            s_hold_until = now + key_ms();
            s_gap_until = s_hold_until + key_ms();
            return m;
        }
    }
    return 0;
}

bool ui_init(void)           { return true; }
void ui_shutdown(void)       {}

static void write_atomic(const char *path, const void *data, size_t len)
{
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    fwrite(data, 1, len, f);
    fclose(f);
    rename(tmp, path);
}

void ui_present_frame(const uint32_t *px)
{
    if (s_dump_path[0]) {                               /* where a finger would hit something: "<dump file>.hits" */
        static char hits[16000], last_hits[16000];
        size_t hn = ui_hit_dump(hits, sizeof hits);
        if (strcmp(hits, last_hits) != 0) {
            memcpy(last_hits, hits, hn + 1);
            char hp[300];
            snprintf(hp, sizeof hp, "%s.hits", s_dump_path);
            write_atomic(hp, hits, hn);
        }
    }
    if (s_dump_path[0]) {
        static char text[24000];
        size_t n = gfx_dump_text(text, sizeof text);
        if (strcmp(text, s_last_dump) != 0) {
            memcpy(s_last_dump, text, n + 1);
            write_atomic(s_dump_path, text, n);
        }
    }
    if (s_ppm_path[0]) {
        static uint64_t last;
        uint64_t now = plat_time_ms();
        if (now - last >= 250) {
            last = now;
            static unsigned char rgb[GFX_W * GFX_H * 3 + 32];
            int hl = snprintf((char *)rgb, 32, "P6\n%d %d\n255\n", GFX_W, GFX_H);
            unsigned char *o = rgb + hl;
            for (int i = 0; i < GFX_W * GFX_H; i++) {
                *o++ = (unsigned char)(px[i] & 0xFF);
                *o++ = (unsigned char)((px[i] >> 8) & 0xFF);
                *o++ = (unsigned char)((px[i] >> 16) & 0xFF);
            }
            write_atomic(s_ppm_path, rgb, (size_t)(o - rgb));
        }
    }
}
