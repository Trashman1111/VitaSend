/*
 * gfx.c - shapes and text. See gfx.h.
 *
 * Author: trashman (VitaSend)
 */
#include "gfx.h"
#include "fontdata.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

/* ============================================================== frame buffer */

static uint32_t *s_px;
static int s_cx0, s_cy0, s_cx1 = GFX_W, s_cy1 = GFX_H;       /* clip rectangle [x0,x1) x [y0,y1) */

bool gfx_init(void)
{
    if (s_px) return true;
    s_px = malloc((size_t)GFX_W * GFX_H * sizeof *s_px);
    if (!s_px) return false;
    memset(s_px, 0, (size_t)GFX_W * GFX_H * sizeof *s_px);
    return true;
}

void gfx_shutdown(void)
{
    free(s_px);
    s_px = NULL;
}

uint32_t *gfx_pixels(void) { return s_px; }

void gfx_clip_reset(void) { s_cx0 = 0; s_cy0 = 0; s_cx1 = GFX_W; s_cy1 = GFX_H; }

void gfx_clip(int x, int y, int w, int h)
{
    s_cx0 = x < 0 ? 0 : x;
    s_cy0 = y < 0 ? 0 : y;
    s_cx1 = x + w > GFX_W ? GFX_W : x + w;
    s_cy1 = y + h > GFX_H ? GFX_H : y + h;
    if (s_cx1 < s_cx0) s_cx1 = s_cx0;
    if (s_cy1 < s_cy0) s_cy1 = s_cy0;
}

/* ---- test support: remembers the text of the frame ---- */
#define REC_MAX 600
typedef struct { int x, cy; bool focus; char text[128]; } rec_item;
static rec_item s_rec[REC_MAX];
static int      s_nrec;
static bool     s_focus;
static char     s_notes[256];

void gfx_note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_notes, sizeof s_notes, fmt, ap);
    va_end(ap);
}

void gfx_focus(bool on) { s_focus = on; }

static bool s_norec;

void gfx_norec(bool on) { s_norec = on; }

static void rec_add(int x, int cy, const char *s, size_t n)
{
    if (s_nrec >= REC_MAX || s_norec) return;
    rec_item *r = &s_rec[s_nrec++];
    r->x = x;
    r->cy = cy;
    r->focus = s_focus;
    if (n >= sizeof r->text) n = sizeof r->text - 1;
    memcpy(r->text, s, n);
    r->text[n] = 0;
}

void gfx_record(int x, int cy, const char *s) { rec_add(x, cy, s, strlen(s)); }

void gfx_begin(void)
{
    gfx_clip_reset();
    s_nrec = 0;
    s_focus = false;
    s_notes[0] = 0;
}

void gfx_end(void)
{
    if (s_px) ui_present_frame(s_px);
}

static int rec_cmp(const void *a, const void *b)
{
    const rec_item *x = a, *y = b;
    int bx = (x->cy + 5) / 10, by = (y->cy + 5) / 10;
    if (bx != by) return bx - by;
    return x->x - y->x;
}

size_t gfx_dump_text(char *out, size_t cap)
{
    static rec_item sorted[REC_MAX];
    memcpy(sorted, s_rec, (size_t)s_nrec * sizeof sorted[0]);
    qsort(sorted, (size_t)s_nrec, sizeof sorted[0], rec_cmp);
    size_t pos = 0;
#define PUT(...) do { if (pos + 1 < cap) { int k_ = snprintf(out + pos, cap - pos, __VA_ARGS__); if (k_ > 0) pos += (size_t)k_; if (pos >= cap) pos = cap - 1; } } while (0)
    if (s_notes[0]) PUT("@ %s\n", s_notes);
    for (int i = 0; i < s_nrec;) {
        int j = i;
        bool focus = false;
        int bucket = (sorted[i].cy + 5) / 10;
        while (j < s_nrec && (sorted[j].cy + 5) / 10 == bucket) { focus |= sorted[j].focus; j++; }
        PUT("%c", focus ? '>' : ' ');
        for (int k = i; k < j; k++) PUT("%s%s", k > i ? "  " : "", sorted[k].text);
        PUT("\n");
        i = j;
    }
#undef PUT
    if (cap) out[pos < cap ? pos : cap - 1] = 0;
    return pos;
}

/* ============================================================== pixels */

static inline uint32_t mix(uint32_t d, uint32_t c, unsigned a)      /* a = 0..255; two colour lanes at a time */
{
    if (a >= 255) return c | 0xFF000000u;
    unsigned a1 = a + (a >> 7);                                      /* 0..256 */
    unsigned ia = 256 - a1;
    uint32_t rb = (((c & 0x00FF00FFu) * a1 + (d & 0x00FF00FFu) * ia) >> 8) & 0x00FF00FFu;
    uint32_t g = (((c & 0x0000FF00u) * a1 + (d & 0x0000FF00u) * ia) >> 8) & 0x0000FF00u;
    return 0xFF000000u | rb | g;
}

/* coverage 0..1 (as 0..255) combined with the colour's own alpha */
static inline void plot(int x, int y, gcol c, unsigned cov)
{
    if (x < s_cx0 || x >= s_cx1 || y < s_cy0 || y >= s_cy1 || !cov) return;
    unsigned a = (cov * (c >> 24) + 127) / 255;
    if (!a) return;
    uint32_t *p = &s_px[y * GFX_W + x];
    *p = mix(*p, c, a);
}

static inline unsigned cov255(float v)          /* clamp(v, 0, 1) * 255 */
{
    if (v <= 0.f) return 0;
    if (v >= 1.f) return 255;
    return (unsigned)(v * 255.f + 0.5f);
}

void gfx_clear(gcol c)
{
    if (!s_px) return;
    uint32_t v = c | 0xFF000000u;
    for (int i = 0; i < GFX_W * GFX_H; i++) s_px[i] = v;
}

void gfx_rect(int x, int y, int w, int h, gcol c)
{
    if (!s_px || w <= 0 || h <= 0) return;
    int x0 = x < s_cx0 ? s_cx0 : x, y0 = y < s_cy0 ? s_cy0 : y;
    int x1 = x + w > s_cx1 ? s_cx1 : x + w, y1 = y + h > s_cy1 ? s_cy1 : y + h;
    unsigned a = c >> 24;
    if (a == 0) return;
    if (a == 255) {
        uint32_t v = c | 0xFF000000u;
        for (int yy = y0; yy < y1; yy++) {
            uint32_t *p = &s_px[yy * GFX_W + x0];
            for (int xx = x0; xx < x1; xx++) *p++ = v;
        }
    } else {
        for (int yy = y0; yy < y1; yy++) {
            uint32_t *p = &s_px[yy * GFX_W + x0];
            for (int xx = x0; xx < x1; xx++, p++) *p = mix(*p, c, a);
        }
    }
}

void gfx_dim(int alpha)
{
    gfx_rect(0, 0, GFX_W, GFX_H, GRGBA(0, 0, 0, alpha));
}

/* ---- rounded rectangles ---- */

/* How much of the pixel (px,py) lies inside a quarter circle of radius r around (ccx,ccy)? */
static inline float circ_cov(int px, int py, float ccx, float ccy, float r)
{
    float dx = (float)px + 0.5f - ccx, dy = (float)py + 0.5f - ccy;
    return r + 0.5f - sqrtf(dx * dx + dy * dy);
}

void gfx_rrect(int x, int y, int w, int h, int r, gcol c)
{
    if (!s_px || w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 0) { gfx_rect(x, y, w, h, c); return; }
    gfx_rect(x + r, y, w - 2 * r, h, c);
    gfx_rect(x, y + r, r, h - 2 * r, c);
    gfx_rect(x + w - r, y + r, r, h - 2 * r, c);
    for (int q = 0; q < 4; q++) {
        int qx = (q & 1) ? x + w - r : x, qy = (q & 2) ? y + h - r : y;
        float ccx = (q & 1) ? (float)(x + w - r) : (float)(x + r);
        float ccy = (q & 2) ? (float)(y + h - r) : (float)(y + r);
        for (int yy = qy; yy < qy + r; yy++)
            for (int xx = qx; xx < qx + r; xx++)
                plot(xx, yy, c, cov255(circ_cov(xx, yy, ccx, ccy, (float)r)));
    }
}

void gfx_rrect_outline(int x, int y, int w, int h, int r, int t, gcol c)
{
    if (!s_px || w <= 0 || h <= 0 || t <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;
    gfx_rect(x + r, y, w - 2 * r, t, c);
    gfx_rect(x + r, y + h - t, w - 2 * r, t, c);
    gfx_rect(x, y + r, t, h - 2 * r, c);
    gfx_rect(x + w - t, y + r, t, h - 2 * r, c);
    if (r == 0) {
        gfx_rect(x, y, t, t, c); gfx_rect(x + w - t, y, t, t, c);
        gfx_rect(x, y + h - t, t, t, c); gfx_rect(x + w - t, y + h - t, t, t, c);
        return;
    }
    for (int q = 0; q < 4; q++) {
        int qx = (q & 1) ? x + w - r : x, qy = (q & 2) ? y + h - r : y;
        float ccx = (q & 1) ? (float)(x + w - r) : (float)(x + r);
        float ccy = (q & 2) ? (float)(y + h - r) : (float)(y + r);
        for (int yy = qy; yy < qy + r; yy++)
            for (int xx = qx; xx < qx + r; xx++) {
                float outer = circ_cov(xx, yy, ccx, ccy, (float)r);
                float inner = (float)(r - t) > 0.f ? circ_cov(xx, yy, ccx, ccy, (float)(r - t)) : 0.f;
                float v = (outer > 1.f ? 1.f : outer) - (inner > 1.f ? 1.f : (inner < 0.f ? 0.f : inner));
                plot(xx, yy, c, cov255(v));
            }
    }
}

/* ---- circles, lines ---- */

void gfx_circle(float cx, float cy, float r, gcol c)
{
    if (!s_px || r <= 0) return;
    int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1), y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            plot(x, y, c, cov255(circ_cov(x, y, cx, cy, r)));
}

void gfx_ring(float cx, float cy, float r, float thick, gcol c)
{
    if (!s_px || r <= 0) return;
    float ro = r + thick * 0.5f, ri = r - thick * 0.5f;
    int x0 = (int)floorf(cx - ro - 1), x1 = (int)ceilf(cx + ro + 1), y0 = (int)floorf(cy - ro - 1), y1 = (int)ceilf(cy + ro + 1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float o = circ_cov(x, y, cx, cy, ro);
            if (o <= 0.f) continue;
            float i = ri > 0.f ? circ_cov(x, y, cx, cy, ri) : 0.f;
            float v = (o > 1.f ? 1.f : o) - (i > 1.f ? 1.f : (i < 0.f ? 0.f : i));
            plot(x, y, c, cov255(v));
        }
}

void gfx_line(float x0, float y0, float x1, float y1, float thick, gcol c)
{
    if (!s_px) return;
    float hw = thick * 0.5f;
    int bx0 = (int)floorf((x0 < x1 ? x0 : x1) - hw - 1), bx1 = (int)ceilf((x0 > x1 ? x0 : x1) + hw + 1);
    int by0 = (int)floorf((y0 < y1 ? y0 : y1) - hw - 1), by1 = (int)ceilf((y0 > y1 ? y0 : y1) + hw + 1);
    float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    for (int y = by0; y <= by1; y++)
        for (int x = bx0; x <= bx1; x++) {
            float px = (float)x + 0.5f - x0, py = (float)y + 0.5f - y0;
            float t = len2 > 0.f ? (px * dx + py * dy) / len2 : 0.f;
            if (t < 0.f) t = 0.f;
            if (t > 1.f) t = 1.f;
            float ex = px - t * dx, ey = py - t * dy;
            plot(x, y, c, cov255(hw + 0.5f - sqrtf(ex * ex + ey * ey)));
        }
}

/* ---- polygons: even-odd fill, 4 sub-scanlines per pixel row, exact horizontal coverage ---- */
void gfx_poly(const float *xy, int n, gcol c)
{
    if (!s_px || n < 3 || n > 32) return;
    float ymin = xy[1], ymax = xy[1], xmin = xy[0], xmax = xy[0];
    for (int i = 1; i < n; i++) {
        if (xy[2 * i + 1] < ymin) ymin = xy[2 * i + 1];
        if (xy[2 * i + 1] > ymax) ymax = xy[2 * i + 1];
        if (xy[2 * i] < xmin) xmin = xy[2 * i];
        if (xy[2 * i] > xmax) xmax = xy[2 * i];
    }
    int px0 = (int)floorf(xmin), px1 = (int)ceilf(xmax);
    if (px0 < 0) px0 = 0;
    if (px1 > GFX_W) px1 = GFX_W;
    int width = px1 - px0;
    if (width <= 0 || width > GFX_W) return;
    float acc[GFX_W + 1];
    enum { SUB = 4 };
    for (int y = (int)floorf(ymin); y <= (int)ceilf(ymax); y++) {
        if (y < s_cy0 || y >= s_cy1) continue;
        memset(acc, 0, sizeof(float) * (size_t)(width + 1));
        for (int s = 0; s < SUB; s++) {
            float sy = (float)y + ((float)s + 0.5f) / (float)SUB;
            float xs[34];
            int nx = 0;
            for (int i = 0; i < n; i++) {
                int j = (i + 1) % n;
                float ay = xy[2 * i + 1], by = xy[2 * j + 1];
                if ((ay <= sy && by > sy) || (by <= sy && ay > sy)) {
                    float t = (sy - ay) / (by - ay);
                    xs[nx++] = xy[2 * i] + t * (xy[2 * j] - xy[2 * i]);
                }
            }
            for (int a = 1; a < nx; a++) {                   /* insertion sort */
                float v = xs[a];
                int b = a - 1;
                while (b >= 0 && xs[b] > v) { xs[b + 1] = xs[b]; b--; }
                xs[b + 1] = v;
            }
            for (int k = 0; k + 1 < nx; k += 2) {
                float a = xs[k] - (float)px0, b = xs[k + 1] - (float)px0;
                if (a < 0.f) a = 0.f;
                if (b > (float)width) b = (float)width;
                if (b <= a) continue;
                int ia = (int)floorf(a), ib = (int)floorf(b);
                if (ia == ib) { acc[ia] += (b - a) / (float)SUB; continue; }
                acc[ia] += ((float)(ia + 1) - a) / (float)SUB;
                for (int xx = ia + 1; xx < ib; xx++) acc[xx] += 1.f / (float)SUB;
                if (ib <= width) acc[ib] += (b - (float)ib) / (float)SUB;
            }
        }
        for (int x = 0; x < width; x++)
            if (acc[x] > 0.f) plot(px0 + x, y, c, cov255(acc[x]));
    }
}

void gfx_shadow(int x, int y, int w, int h, int r)
{
    for (int i = 8; i >= 1; i -= 2)
        gfx_rrect(x - i, y - i / 2 + 4, w + 2 * i, h + 2 * i, r + i, GRGBA(0, 0, 0, 16));
}

/* ============================================================== text */

enum { FI_POP_REG, FI_POP_MED, FI_POP_BOLD, FI_LIB_REG, FI_LIB_BOLD, FI_COUNT };
static stbtt_fontinfo s_fi[FI_COUNT];
static bool s_fonts_ok;

static void fonts_init(void)
{
    if (s_fonts_ok) return;
    const unsigned char *d[FI_COUNT] = { font_poppins_regular, font_poppins_medium, font_poppins_bold,
                                         font_fallback_regular, font_fallback_bold };
    for (int i = 0; i < FI_COUNT; i++) stbtt_InitFont(&s_fi[i], d[i], 0);
    s_fonts_ok = true;
}

typedef struct {
    uint32_t key;                 /* 0 = free */
    int16_t  xoff, yoff;
    uint16_t w, h;
    float    adv;
    uint8_t *bmp;
} glyph;

#define CACHE_N 2048
static glyph s_cache[CACHE_N];
static int   s_cache_used;

static void cache_flush(void)
{
    for (int i = 0; i < CACHE_N; i++) {
        if (s_cache[i].bmp) stbtt_FreeBitmap(s_cache[i].bmp, NULL);
        s_cache[i].bmp = NULL;
        s_cache[i].key = 0;
    }
    s_cache_used = 0;
}

static int pick_font(int weight, uint32_t cp)
{
    int primary = weight == GW_BOLD ? FI_POP_BOLD : (weight == GW_MED ? FI_POP_MED : FI_POP_REG);
    int fallback = weight == GW_BOLD ? FI_LIB_BOLD : FI_LIB_REG;
    if (stbtt_FindGlyphIndex(&s_fi[primary], (int)cp)) return primary;
    if (stbtt_FindGlyphIndex(&s_fi[fallback], (int)cp)) return fallback;
    return -1;
}

static const glyph *get_glyph(uint32_t cp, int weight, int size)
{
    fonts_init();
    if (cp < 0x20 || cp == 0x7F) cp = ' ';
    int fi = pick_font(weight, cp);
    if (fi < 0) { cp = '?'; fi = pick_font(weight, cp); if (fi < 0) fi = FI_POP_REG; }
    uint32_t key = (cp << 11) | ((uint32_t)(fi + 1) << 8) | (uint32_t)(size & 0xFF);
    if (cp >= (1u << 21)) key = 0x7FFFFFFFu;
    unsigned h = (key * 2654435761u) >> 21;
    for (unsigned probe = 0; probe < CACHE_N; probe++) {
        glyph *g = &s_cache[(h + probe) % CACHE_N];
        if (g->key == key) return g;
        if (g->key == 0) {
            if (s_cache_used > CACHE_N * 3 / 4) { cache_flush(); return get_glyph(cp, weight, size); }
            stbtt_fontinfo *f = &s_fi[fi];
            float scale = stbtt_ScaleForMappingEmToPixels(f, (float)size);
            if (fi >= FI_LIB_REG) scale *= 1.07f;                    /* Liberation looks a little smaller than Poppins at the same size */
            int gi = stbtt_FindGlyphIndex(f, (int)cp);
            int adv, lsb, w = 0, hh = 0, xo = 0, yo = 0;
            stbtt_GetGlyphHMetrics(f, gi, &adv, &lsb);
            g->bmp = (cp == ' ') ? NULL : stbtt_GetGlyphBitmap(f, scale, scale, gi, &w, &hh, &xo, &yo);
            g->key = key;
            g->w = (uint16_t)(g->bmp ? w : 0);
            g->h = (uint16_t)(g->bmp ? hh : 0);
            g->xoff = (int16_t)xo;
            g->yoff = (int16_t)yo;
            g->adv = (float)adv * scale;
            s_cache_used++;
            return g;
        }
    }
    return &s_cache[0];
}

static uint32_t utf8_next(const char **pp)
{
    const unsigned char *p = (const unsigned char *)*pp;
    uint32_t c = *p;
    if (c < 0x80) { *pp += c ? 1 : 0; return c; }
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (n == 0 || c > 0xF4) { *pp += 1; return 0xFFFD; }
    uint32_t cp = c & (0x3F >> n);
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *pp += i; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *pp += n + 1;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return 0xFFFD;
    return cp;
}

size_t gfx_utf8_prev(const char *s, size_t len)
{
    if (len == 0) return 0;
    size_t i = len - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80 && len - i < 4) i--;
    return len - i;
}

static float text_width_f(int size, int weight, const char *s, const char *end)
{
    float w = 0;
    while (*s && (!end || s < end)) {
        uint32_t cp = utf8_next(&s);
        w += get_glyph(cp, weight, size)->adv;
    }
    return w;
}

int gfx_text_width(int size, int weight, const char *s)
{
    return (int)ceilf(text_width_f(size, weight, s, NULL));
}

static void draw_run(int x, int cy, int size, int weight, gcol c, const char *s, const char *end)
{
    float pen = (float)x;
    int baseline = cy + (int)(0.35f * (float)size + 0.5f);
    while (*s && (!end || s < end)) {
        uint32_t cp = utf8_next(&s);
        const glyph *g = get_glyph(cp, weight, size);
        if (g->bmp) {
            int gx = (int)floorf(pen + 0.5f) + g->xoff, gy = baseline + g->yoff;
            for (int yy = 0; yy < g->h; yy++) {
                const uint8_t *row = g->bmp + yy * g->w;
                for (int xx = 0; xx < g->w; xx++)
                    if (row[xx]) plot(gx + xx, gy + yy, c, row[xx]);
            }
        }
        pen += g->adv;
    }
}

int gfx_text(int x, int cy, int size, int weight, gcol c, int align, const char *s)
{
    if (!s) return 0;
    int w = gfx_text_width(size, weight, s);
    int left = align == GA_CENTER ? x - w / 2 : (align == GA_RIGHT ? x - w : x);
    draw_run(left, cy, size, weight, c, s, NULL);
    rec_add(left, cy, s, strlen(s));
    return w;
}

int gfx_text_fit(int x, int cy, int maxw, int size, int weight, gcol c, int align, const char *s)
{
    if (!s) return 0;
    int w = gfx_text_width(size, weight, s);
    if (w <= maxw) return gfx_text(x, cy, size, weight, c, align, s);

    static const char dots[] = "\xE2\x80\xA6";                  /* U+2026 */
    float room = (float)maxw - text_width_f(size, weight, dots, NULL);
    const char *p = s, *cut = s;
    float acc = 0;
    while (*p) {
        const char *q = p;
        uint32_t cp = utf8_next(&q);
        float a = get_glyph(cp, weight, size)->adv;
        if (acc + a > room) break;
        acc += a;
        p = q;
        cut = p;
    }
    int total = (int)ceilf(acc + text_width_f(size, weight, dots, NULL));
    int left = align == GA_CENTER ? x - total / 2 : (align == GA_RIGHT ? x - total : x);
    draw_run(left, cy, size, weight, c, s, cut);
    draw_run(left + (int)ceilf(acc), cy, size, weight, c, dots, NULL);
    rec_add(left, cy, s, strlen(s));
    return total;
}

int gfx_wrap(const char *s, int size, int weight, int maxw, gfx_line_t *out, int max_lines)
{
    int n = 0;
    const char *p = s;
    while (*p && n < max_lines) {
        const char *line = p, *last_break = NULL;
        float w = 0;
        const char *q = p;
        const char *stop = NULL;                                   /* where this line ends (exclusive) */
        const char *next = NULL;                                   /* where the next line starts */
        while (*q) {
            const char *before = q;
            uint32_t cp = utf8_next(&q);
            if (cp == '\n') { stop = before; next = q; break; }
            float a = get_glyph(cp, weight, size)->adv;
            if (cp == ' ') last_break = before;
            if (w + a > (float)maxw && before > line) {
                if (last_break && last_break > line) { stop = last_break; next = last_break + 1; }
                else { stop = before; next = before; }
                break;
            }
            w += a;
        }
        if (!stop) { stop = q; next = q; }
        out[n].p = line;
        out[n].len = (int)(stop - line);
        n++;
        p = next;
        if (p == line && !*p) break;
    }
    return n;
}
