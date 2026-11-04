/*
 * gfx.h - a small 2D renderer: everything is drawn into a 960x544 picture in normal memory and
 * sent to the screen once per frame (ui_present_frame, implemented by ui_vita.c / ui_host.c).
 *
 *   - rectangles, rounded rectangles, circles, lines, polygons - all anti-aliased
 *   - text: UTF-8, embedded fonts (Poppins, with Liberation Sans for Greek / Cyrillic / rare letters)
 *   - icons and PlayStation button symbols drawn from lines (no image files needed)
 *
 * Colours are 0xAABBGGRR like the Vita's display. The alpha channel works ("source over").
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_GFX_H
#define LS_GFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GFX_W 960
#define GFX_H 544

typedef uint32_t gcol;
#define GRGBA(r, g, b, a) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
#define GRGB(r, g, b) GRGBA(r, g, b, 255)

/* the same buttons as the platform layer reports (bit masks) */
enum {
    UI_BTN_START    = 1 << 0,
    UI_BTN_SELECT   = 1 << 1,
    UI_BTN_TRIANGLE = 1 << 2,
    UI_BTN_CIRCLE   = 1 << 3,
    UI_BTN_CROSS    = 1 << 4,
    UI_BTN_SQUARE   = 1 << 5,
    UI_BTN_UP       = 1 << 6,
    UI_BTN_DOWN     = 1 << 7,
    UI_BTN_LEFT     = 1 << 8,
    UI_BTN_RIGHT    = 1 << 9,
    UI_BTN_L        = 1 << 10,
    UI_BTN_R        = 1 << 11,
};

/* ---- platform part (ui_vita.c / ui_host.c) ---- */
bool     ui_init(void);
void     ui_shutdown(void);
void     ui_present_frame(const uint32_t *pixels);       /* 960 x 544, called once per finished frame */
uint32_t ui_buttons(void);                               /* currently held buttons (UI_BTN_* bits)   */
/* Front touch screen (never the back panel): true while a finger is on it; x, y in screen pixels (960 x 544).
 * With several fingers only the first one is reported. */
bool     ui_touch(int *x, int *y);

/* ---- frame ---- */
bool      gfx_init(void);                                /* allocates the picture; false = out of memory */
void      gfx_shutdown(void);
uint32_t *gfx_pixels(void);
void      gfx_begin(void);                               /* start of a frame */
void      gfx_end(void);                                 /* end of a frame: sends it to the screen */

/* ---- shapes (all in pixels; float parameters are positions in the continuous plane: pixel centers at +0.5) ---- */
void gfx_clip(int x, int y, int w, int h);               /* later drawing is limited to this rectangle */
void gfx_clip_reset(void);
void gfx_clear(gcol c);
void gfx_rect(int x, int y, int w, int h, gcol c);
void gfx_rrect(int x, int y, int w, int h, int r, gcol c);                        /* filled, rounded corners */
void gfx_rrect_outline(int x, int y, int w, int h, int r, int thick, gcol c);
void gfx_circle(float cx, float cy, float r, gcol c);
void gfx_ring(float cx, float cy, float r, float thick, gcol c);
void gfx_line(float x0, float y0, float x1, float y1, float thick, gcol c);       /* round ends */
void gfx_poly(const float *xy, int n, gcol c);                                    /* filled polygon, n points */
void gfx_dim(int alpha);                                 /* darkens everything drawn so far (0..255) */
void gfx_shadow(int x, int y, int w, int h, int r);      /* soft drop shadow below a card */

/* ---- text ---- */
enum { GW_REG = 0, GW_MED = 1, GW_BOLD = 2 };
enum { GA_LEFT = 0, GA_CENTER = 1, GA_RIGHT = 2 };

/* `cy` is the vertical middle of the capital letters. Returns the width in pixels. */
int  gfx_text(int x, int cy, int size, int weight, gcol c, int align, const char *s);
/* same, but cut off with "..." when wider than maxw */
int  gfx_text_fit(int x, int cy, int maxw, int size, int weight, gcol c, int align, const char *s);
int  gfx_text_width(int size, int weight, const char *s);
/* Breaks s into lines no wider than maxw (at spaces / line breaks, or anywhere inside a very long word). */
typedef struct { const char *p; int len; } gfx_line_t;
int  gfx_wrap(const char *s, int size, int weight, int maxw, gfx_line_t *out, int max_lines);
/* Number of bytes of the last UTF-8 character before s[len] (for the on-screen keyboard's delete key). */
size_t gfx_utf8_prev(const char *s, size_t len);

/* ---- icons and button symbols ---- */
enum {
    IC_LOGO, IC_RECEIVE, IC_SEND, IC_HISTORY, IC_SETTINGS, IC_STAR, IC_STAR_OUTLINE, IC_FOLDER, IC_FILE, IC_PACKAGE,
    IC_CHECK, IC_CLOSE, IC_PLUS, IC_PHONE, IC_DESKTOP, IC_LAPTOP, IC_WEB, IC_SERVER, IC_VITA, IC_MESSAGE,
    IC_ARROW_DOWN, IC_ARROW_UP, IC_ARROW_LEFT, IC_WIFI, IC_REFRESH, IC_BACK
};
void gfx_icon(int icon, float cx, float cy, float size, gcol c);

enum { PB_CROSS, PB_CIRCLE, PB_SQUARE, PB_TRIANGLE, PB_L, PB_R, PB_START, PB_SELECT, PB_DPAD, PB_DPAD_LR, PB_DPAD_UD };
/* Draws the symbol centred at (cx, cy) and returns its width. size = height in pixels. */
int  gfx_pad_button(int button, int cx, int cy, int size);
int  gfx_pad_button_width(int button, int size);

/* ---- test support (PC build only; harmless elsewhere) ---- */
void gfx_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   /* a line for the text dump, e.g. "tab=Send" */
void gfx_focus(bool on);                                 /* text drawn while this is on marks its line as the selected one */
size_t gfx_dump_text(char *out, size_t cap);             /* the frame as plain text, one line per row of the UI */
void gfx_record(int x, int cy, const char *s);           /* adds text to the dump without drawing it */
void gfx_norec(bool on);                                 /* while on, drawn text is not added to the dump */

#endif
