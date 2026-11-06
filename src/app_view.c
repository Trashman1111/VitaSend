/*
 * app_view.c - the viewer for pictures and text files (opened with X in the Files tab)
 *
 * One file, or several ticked files as a queue that can be paged through with L / R (or LEFT / RIGHT on a picture).
 *
 *   pictures  JPEG, PNG, BMP, GIF (first frame) - decoded with stb_image (public domain), turned upright by the EXIF
 *             orientation of a JPEG, and scaled once to fit the screen. Big pictures are refused rather than risking the
 *             Vita's memory (see PIC_MAX_*).
 *   text      .txt, .md, .log, .json, source files ... the first 128 KB, wrapped to the screen width, scrolled with UP/DOWN,
 *             LEFT/RIGHT (a page) or a finger.
 *
 * Loading takes a moment for a big picture, so the screen first shows "Loading..." and the work is done by the next frame.
 *
 * Author: trashman (VitaSend)
 */
#include "app_int.h"

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD                       /* plain C: the same code runs on the Vita and on the PC tests */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#pragma GCC diagnostic pop

#define VIEW_TOP     44                    /* the title bar */
#define VIEW_AREA_H  (HINT_Y - VIEW_TOP)   /* where a picture may be shown */
#define TXT_ROWS     18
#define TXT_STEP     24
#define TXT_MAX      (128 * 1024)          /* bytes of a text file that are shown */
#define TXT_SEG      1200                  /* a line without line breaks is cut into pieces of at most this many bytes */
#define TXT_LINES    30000
#define PIC_MAX_BASE   (10u * 1000 * 1000) /* pixels: baseline JPEG (3 bytes each while decoding) */
#define PIC_MAX_PROG   (2500u * 1000)      /* progressive JPEG needs about 8 bytes per pixel */
#define PIC_MAX_OTHER  (6u * 1000 * 1000)  /* PNG / BMP / GIF (4 bytes each) */

enum { VK_NONE, VK_IMAGE, VK_TEXT };

static const char k_image_ext[] = " jpg jpeg png bmp gif ";
static const char k_text_ext[]  = " txt md log json xml ini cfg conf csv lua c h cpp hpp py js html htm yml yaml toml sh bat nfo srt css ";

int view_kind(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1] || strlen(dot + 1) > 6) return VK_NONE;
    char key[10];
    snprintf(key, sizeof key, " %s ", dot + 1);
    for (char *c = key; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
    if (strstr(k_image_ext, key)) return VK_IMAGE;
    if (strstr(k_text_ext, key)) return VK_TEXT;
    return VK_NONE;
}

/* ---- what is open ---- */

typedef enum { VS_LOADING, VS_IMAGE, VS_TEXT, VS_ERROR } view_state;

static char       s_vp[MAX_SEL][PATH_CAP];
static int        s_vn, s_vi;
static screen_id  s_vret = SC_FILES;
static view_state s_vs;
static bool       s_vshown;                  /* the "Loading..." picture has been on screen once: now load */
static char       s_verr[160];

static uint32_t  *s_vimg;                    /* the picture as shown (s_vdw x s_vdh) */
static int        s_vdw, s_vdh, s_viw, s_vih;    /* shown size, size of the file's picture */
static char      *s_vtxt;
static gfx_line_t *s_vlines;
static int        s_vnl, s_vtop;
static bool       s_vcut;                    /* only the first part of a long text is shown */

bool view_loading(void) { return s_vs == VS_LOADING; }

static void view_free(void)
{
    free(s_vimg);   s_vimg = NULL;
    free(s_vtxt);   s_vtxt = NULL;
    free(s_vlines); s_vlines = NULL;
    s_vnl = s_vtop = 0;
    s_vcut = false;
}

void view_open(const char *const *paths, int n, int start, screen_id ret)
{
    if (n <= 0) return;
    if (n > MAX_SEL) n = MAX_SEL;
    view_free();
    for (int i = 0; i < n; i++) ls_strlcpy(s_vp[i], paths[i], PATH_CAP);
    s_vn = n;
    s_vi = start >= 0 && start < n ? start : 0;
    s_vret = ret;
    s_vs = VS_LOADING;
    s_vshown = false;
    go(SC_VIEW);
}

static void view_close(void)
{
    view_free();
    s_vs = VS_LOADING;
    go(s_vret);
}

static void fail(const char *fmt, ...) LS_PRINTF(1, 2);
static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_verr, sizeof s_verr, fmt, ap);
    va_end(ap);
    s_vs = VS_ERROR;
}

/* ---- pictures ---- */

static uint16_t rd16(const unsigned char *p, bool le) { return le ? (uint16_t)(p[0] | p[1] << 8) : (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const unsigned char *p, bool le)
{
    return le ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24
              : (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
}

/* The EXIF orientation (1..8) of the first "Exif" block. */
static int exif_orientation(const unsigned char *d, size_t n)
{
    if (n < 14 || memcmp(d, "Exif\0\0", 6)) return 1;
    const unsigned char *t = d + 6;
    size_t tn = n - 6;
    bool le = t[0] == 'I';
    if (tn < 8 || (!le && t[0] != 'M')) return 1;
    uint32_t off = rd32(t + 4, le);
    if (off + 2 > tn) return 1;
    unsigned cnt = rd16(t + off, le);
    for (unsigned i = 0; i < cnt; i++) {
        size_t e = off + 2 + (size_t)i * 12;
        if (e + 12 > tn) break;
        if (rd16(t + e, le) == 0x0112) {
            unsigned v = rd16(t + e + 8, le);
            return v >= 1 && v <= 8 ? (int)v : 1;
        }
    }
    return 1;
}

/* Walks the marker segments of a JPEG up to the start of the data: the EXIF orientation, and whether it is progressive. */
static void jpeg_info(FILE *f, int *orient, bool *progressive)
{
    *orient = 1;
    *progressive = false;
    unsigned char b[4];
    if (fread(b, 1, 2, f) != 2 || b[0] != 0xFF || b[1] != 0xD8) return;
    for (int guard = 0; guard < 64; guard++) {
        int c;
        do { c = fgetc(f); } while (c != EOF && c != 0xFF);
        do { c = fgetc(f); } while (c == 0xFF);
        if (c == EOF || c == 0xDA || c == 0xD9) return;                   /* start of data / end */
        if (c == 0x01 || (c >= 0xD0 && c <= 0xD7)) continue;              /* markers without a length */
        if (fread(b, 1, 2, f) != 2) return;
        size_t len = (size_t)(b[0] << 8 | b[1]);
        if (len < 2) return;
        len -= 2;
        if (c == 0xC2 || c == 0xC6 || c == 0xCA || c == 0xCE) *progressive = true;
        if (c == 0xE1 && len <= 65535) {
            unsigned char *seg = malloc(len ? len : 1);
            if (!seg) return;
            if (fread(seg, 1, len, f) != len) { free(seg); return; }
            *orient = exif_orientation(seg, len);
            free(seg);
            continue;
        }
        if (fseek(f, (long)len, SEEK_CUR) != 0) return;
        if (c >= 0xC0 && c <= 0xCF && c != 0xC4 && c != 0xC8 && c != 0xCC) return;   /* the frame header is the last thing we need */
    }
}

/* (u, v) in the upright picture -> (x, y) in the file's picture */
static inline void orient_xy(int o, int w, int h, int u, int v, int *x, int *y)
{
    switch (o) {
    case 2: *x = w - 1 - u; *y = v; break;
    case 3: *x = w - 1 - u; *y = h - 1 - v; break;
    case 4: *x = u; *y = h - 1 - v; break;
    case 5: *x = v; *y = u; break;
    case 6: *x = v; *y = h - 1 - u; break;
    case 7: *x = w - 1 - v; *y = h - 1 - u; break;
    case 8: *x = w - 1 - v; *y = u; break;
    default: *x = u; *y = v; break;
    }
}

/* Scales the decoded picture (w x h, `comp` bytes per pixel: 3 or 4) to fit the screen, upright, into s_vimg. */
static bool scale_picture(const unsigned char *src, int w, int h, int comp, int orient)
{
    int DW = orient >= 5 ? h : w, DH = orient >= 5 ? w : h;                  /* size of the upright picture */
    float sc = (float)GFX_W / (float)DW;
    if ((float)VIEW_AREA_H / (float)DH < sc) sc = (float)VIEW_AREA_H / (float)DH;
    if (sc > 2.0f) sc = 2.0f;
    int dw = (int)((float)DW * sc), dh = (int)((float)DH * sc);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (dw > GFX_W) dw = GFX_W;
    if (dh > VIEW_AREA_H) dh = VIEW_AREA_H;
    uint32_t *out = malloc((size_t)dw * (size_t)dh * sizeof *out);
    if (!out) return false;
    const unsigned bgr = 12, bgg = 15, bgb = 18;                              /* the viewer's background */
    for (int dy = 0; dy < dh; dy++) {
        int v0 = (int)((int64_t)dy * DH / dh), v1 = (int)((int64_t)(dy + 1) * DH / dh);
        if (v1 <= v0) v1 = v0 + 1;
        if (v1 > DH) v1 = DH;
        for (int dx = 0; dx < dw; dx++) {
            int u0 = (int)((int64_t)dx * DW / dw), u1 = (int)((int64_t)(dx + 1) * DW / dw);
            if (u1 <= u0) u1 = u0 + 1;
            if (u1 > DW) u1 = DW;
            uint64_t r = 0, g = 0, b = 0, a = 0;
            unsigned cnt = 0;
            for (int v = v0; v < v1; v++) {
                for (int u = u0; u < u1; u++) {
                    int x, y;
                    orient_xy(orient, w, h, u, v, &x, &y);
                    const unsigned char *p = src + ((size_t)y * (size_t)w + (size_t)x) * (size_t)comp;
                    r += p[0]; g += p[1]; b += p[2];
                    a += comp == 4 ? p[3] : 255;
                    cnt++;
                }
            }
            unsigned R = (unsigned)(r / cnt), G = (unsigned)(g / cnt), B = (unsigned)(b / cnt), A = (unsigned)(a / cnt);
            if (A < 255) {                                                   /* transparent parts show the background */
                R = (R * A + bgr * (255 - A)) / 255;
                G = (G * A + bgg * (255 - A)) / 255;
                B = (B * A + bgb * (255 - A)) / 255;
            }
            out[(size_t)dy * (size_t)dw + (size_t)dx] = GRGB(R, G, B);
        }
    }
    s_vimg = out;
    s_vdw = dw;
    s_vdh = dh;
    return true;
}

static void load_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fail("Cannot open this file"); return; }
    const char *dot = strrchr(path, '.');
    char ext[8] = "";
    if (dot && strlen(dot) < sizeof ext) for (size_t i = 0; dot[i]; i++) ext[i] = (char)(dot[i] >= 'A' && dot[i] <= 'Z' ? dot[i] + 32 : dot[i]);
    bool jpeg = !strcmp(ext, ".jpg") || !strcmp(ext, ".jpeg");
    int orient = 1;
    bool prog = false;
    if (jpeg) { jpeg_info(f, &orient, &prog); rewind(f); }
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_file(f, &w, &h, &comp) || w <= 0 || h <= 0) {
        fclose(f);
        fail("Cannot show this picture (%s)", stbi_failure_reason() ? stbi_failure_reason() : "unknown format");
        return;
    }
    rewind(f);
    uint64_t px = (uint64_t)w * (uint64_t)h;
    uint64_t limit = jpeg ? (prog ? PIC_MAX_PROG : PIC_MAX_BASE) : PIC_MAX_OTHER;
    if (px > limit) {
        fclose(f);
        fail("This picture is too large to show on the Vita (%d x %d%s)", w, h, prog ? ", progressive" : "");
        return;
    }
    int req = jpeg ? 3 : 4, got = 0;
    unsigned char *pix = stbi_load_from_file(f, &w, &h, &got, req);
    fclose(f);
    if (!pix) { fail("Cannot show this picture (%s)", stbi_failure_reason() ? stbi_failure_reason() : "decoding failed"); return; }
    bool ok = scale_picture(pix, w, h, req, orient);
    stbi_image_free(pix);
    if (!ok) { fail("Not enough memory to show this picture"); return; }
    s_viw = orient >= 5 ? h : w;
    s_vih = orient >= 5 ? w : h;
    s_vs = VS_IMAGE;
}

/* ---- text ---- */

static void load_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fail("Cannot open this file"); return; }
    unsigned char *raw = malloc(TXT_MAX + 1);
    if (!raw) { fclose(f); fail("Not enough memory"); return; }
    size_t n = fread(raw, 1, TXT_MAX + 1, f);
    fclose(f);
    if (n > TXT_MAX) { n = TXT_MAX; s_vcut = true; }
    for (size_t i = 0; i < n && i < 4096; i++)
        if (raw[i] == 0) { free(raw); fail("This is not a text file"); return; }
    if (s_vcut) while (n > 0 && (raw[n - 1] & 0xC0) == 0x80) n--;               /* do not cut a character in half */
    char *t = malloc(n * 4 + 8);
    if (!t) { free(raw); fail("Not enough memory"); return; }
    size_t k = 0, i = 0, seg = 0;
    if (n >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) i = 3;       /* UTF-8 byte order mark */
    for (; i < n; i++) {
        unsigned char c = raw[i];
        if (c == '\r') continue;
        if (c == '\n') { t[k++] = 0; seg = 0; continue; }
        if (c == '\t') { for (int s = 0; s < 4; s++) t[k++] = ' '; seg += 4; continue; }
        if (c < 0x20) c = ' ';
        if (seg >= TXT_SEG && (c & 0xC0) != 0x80) { t[k++] = 0; seg = 0; }          /* break very long lines (never inside a character) */
        t[k++] = (char)c;
        seg++;
    }
    t[k++] = 0;
    free(raw);
    size_t cap = 256;
    gfx_line_t *lines = malloc(cap * sizeof *lines);
    if (!lines) { free(t); fail("Not enough memory"); return; }
    int nl = 0;
    for (size_t pos = 0; pos < k && nl < TXT_LINES; pos += strlen(t + pos) + 1) {
        gfx_line_t tmp[64];
        int m = gfx_wrap(t + pos, 16, GW_REG, GFX_W - 100, tmp, 64);
        if (m == 0) { tmp[0].p = t + pos; tmp[0].len = 0; m = 1; }                   /* an empty line */
        for (int j = 0; j < m && nl < TXT_LINES; j++) {
            if ((size_t)nl >= cap) {
                cap *= 2;
                gfx_line_t *bigger = realloc(lines, cap * sizeof *lines);
                if (!bigger) { free(lines); free(t); fail("Not enough memory"); return; }
                lines = bigger;
            }
            lines[nl++] = tmp[j];
        }
    }
    if (nl >= TXT_LINES) s_vcut = true;
    s_vtxt = t;
    s_vlines = lines;
    s_vnl = nl;
    s_vtop = 0;
    s_vs = VS_TEXT;
}

static void load_current(void)
{
    view_free();
    const char *path = s_vp[s_vi];
    if (view_kind(path) == VK_IMAGE) load_image(path);
    else load_text(path);
    ls_log("viewer: %s -> %s", base_name(path), s_vs == VS_ERROR ? s_verr : (s_vs == VS_IMAGE ? "picture" : "text"));
}

/* ---- screen ---- */

static void go_to(int i)
{
    if (i < 0 || i >= s_vn || i == s_vi) return;
    view_free();
    s_vi = i;
    s_vs = VS_LOADING;
    s_vshown = false;
}

void view_draw(const xfer_status *x)
{
    (void)x;
    if (s_vs == VS_LOADING && s_vshown) load_current();       /* ("Loading..." was on the screen for a frame: now the work, then the result) */
    gfx_clear(GRGB(12, 15, 18));
    gfx_rect(0, 0, GFX_W, VIEW_TOP, C_SIDEBAR);
    gfx_rect(0, VIEW_TOP - 1, GFX_W, 1, C_BORDER);
    gfx_rect(0, HINT_Y, GFX_W, GFX_H - HINT_Y, C_SIDEBAR);
    gfx_rect(0, HINT_Y, GFX_W, 1, C_BORDER);

    const char *path = s_vp[s_vi];
    gfx_icon(IC_FILE, 28, VIEW_TOP / 2.0f, 20, C_ACCENT_HI);
    char right[96] = "";
    if (s_vn > 1) snprintf(right, sizeof right, "%d / %d", s_vi + 1, s_vn);
    if (s_vs == VS_IMAGE) {
        size_t l = strlen(right);
        snprintf(right + l, sizeof right - l, "%s%d x %d", l ? "   \xC2\xB7   " : "", s_viw, s_vih);
    } else if (s_vs == VS_TEXT && s_vcut) {
        size_t l = strlen(right);
        snprintf(right + l, sizeof right - l, "%sfirst %d KB", l ? "   \xC2\xB7   " : "", TXT_MAX / 1024);
    }
    int rw = right[0] ? gfx_text(GFX_W - 20, VIEW_TOP / 2, 14, GW_REG, C_DIM, GA_RIGHT, right) + 30 : 0;
    gfx_text_fit(52, VIEW_TOP / 2, GFX_W - 52 - rw - 20, 17, GW_MED, C_TEXT, GA_LEFT, base_name(path));

    if (s_vs == VS_LOADING) {
        gfx_text(GFX_W / 2, VIEW_TOP + VIEW_AREA_H / 2, 20, GW_MED, C_DIM, GA_CENTER, "Loading...");
        s_vshown = true;
    } else if (s_vs == VS_ERROR) {
        gfx_icon(IC_CLOSE, GFX_W / 2, VIEW_TOP + VIEW_AREA_H / 2 - 36, 36, C_BAD);
        gfx_text_fit(GFX_W / 2, VIEW_TOP + VIEW_AREA_H / 2 + 14, GFX_W - 120, 17, GW_MED, C_TEXT, GA_CENTER, s_verr);
    } else if (s_vs == VS_IMAGE && s_vimg) {
        int ox = (GFX_W - s_vdw) / 2, oy = VIEW_TOP + (VIEW_AREA_H - s_vdh) / 2;
        uint32_t *px = gfx_pixels();
        for (int y = 0; y < s_vdh; y++) memcpy(px + (size_t)(oy + y) * GFX_W + ox, s_vimg + (size_t)y * (size_t)s_vdw, (size_t)s_vdw * sizeof *px);
    } else if (s_vs == VS_TEXT) {
        if (s_vtop > s_vnl - TXT_ROWS) s_vtop = s_vnl - TXT_ROWS;
        if (s_vtop < 0) s_vtop = 0;
        for (int i = 0; i < TXT_ROWS && s_vtop + i < s_vnl; i++) {
            char buf[400];
            int len = s_vlines[s_vtop + i].len;
            if (len >= (int)sizeof buf) len = (int)sizeof buf - 1;
            memcpy(buf, s_vlines[s_vtop + i].p, (size_t)len);
            buf[len] = 0;
            gfx_text(40, VIEW_TOP + 26 + i * TXT_STEP, 16, GW_REG, C_TEXT, GA_LEFT, buf);
        }
        ui_scrollbar(GFX_W - 18, VIEW_TOP + 8, TXT_ROWS * TXT_STEP - 4, s_vtop, TXT_ROWS, s_vnl);
        ui_hit_scroll(0, VIEW_TOP, GFX_W, TXT_ROWS * TXT_STEP + 12, &s_vtop, NULL, s_vnl, TXT_ROWS, TXT_STEP);
    }
    if (s_vn > 1 && s_vs != VS_TEXT) {                      /* touch: the left / right part of the screen turns the page */
        ui_hit_keys(0, VIEW_TOP, GFX_W / 3, VIEW_AREA_H, UI_BTN_LEFT);
        ui_hit_keys(GFX_W - GFX_W / 3, VIEW_TOP, GFX_W / 3, VIEW_AREA_H, UI_BTN_RIGHT);
    }
    if (s_vs == VS_TEXT) ui_hints(s_vn > 1 ? "DUD Scroll|LR Previous / next|O Close" : "DUD Scroll|O Close");
    else ui_hints(s_vn > 1 ? "LR Previous / next|O Close" : "O Close");
}

void view_input(uint32_t p)
{
    if (p & UI_BTN_CIRCLE) { view_close(); return; }
    int step = 0;
    if (p & UI_BTN_L) step = -1;
    if (p & UI_BTN_R) step = 1;
    if (s_vs != VS_TEXT) {                                   /* (also while the next one is loading: keep turning pages) */
        if (p & UI_BTN_LEFT) step = -1;
        if (p & UI_BTN_RIGHT) step = 1;
    } else {
        if (p & UI_BTN_UP)    s_vtop--;
        if (p & UI_BTN_DOWN)  s_vtop++;
        if (p & UI_BTN_LEFT)  s_vtop -= TXT_ROWS - 1;
        if (p & UI_BTN_RIGHT) s_vtop += TXT_ROWS - 1;
    }
    if (step) go_to(s_vi + step);
}
