/*
 * gfx_icons.c - icons and PlayStation button symbols, drawn from lines and shapes (no image files).
 *
 * Author: trashman (VitaSend)
 */
#include "gfx.h"

#include <math.h>
#include <stdio.h>

static void arc(float cx, float cy, float r, float a0, float a1, float t, gcol c)
{
    const int steps = 10;
    float px = cx + r * cosf(a0), py = cy + r * sinf(a0);
    for (int i = 1; i <= steps; i++) {
        float a = a0 + (a1 - a0) * (float)i / (float)steps;
        float x = cx + r * cosf(a), y = cy + r * sinf(a);
        gfx_line(px, py, x, y, t, c);
        px = x;
        py = y;
    }
}

static gcol faint(gcol c, int alpha)         /* same colour, scaled alpha */
{
    unsigned a = ((c >> 24) * (unsigned)alpha) / 255;
    return (c & 0x00FFFFFFu) | (a << 24);
}

void gfx_icon(int icon, float cx, float cy, float s, gcol c)
{
    float t = s * 0.085f;
    if (t < 1.6f) t = 1.6f;
#define L(x0, y0, x1, y1) gfx_line(cx + (x0) * s, cy + (y0) * s, cx + (x1) * s, cy + (y1) * s, t, c)
#define RR(x0, y0, w, h, r) gfx_rrect_outline((int)floorf(cx + (x0) * s + 0.5f), (int)floorf(cy + (y0) * s + 0.5f), \
                                              (int)floorf((w) * s + 0.5f), (int)floorf((h) * s + 0.5f), (int)((r) * s + 0.5f), (int)(t + 0.5f), c)
    switch (icon) {
    case IC_LOGO:
        L(-0.34f, -0.14f, 0.34f, -0.14f); L(0.34f, -0.14f, 0.16f, -0.31f); L(0.34f, -0.14f, 0.16f, 0.03f);
        L(0.34f, 0.17f, -0.34f, 0.17f);   L(-0.34f, 0.17f, -0.16f, 0.0f);  L(-0.34f, 0.17f, -0.16f, 0.34f);
        break;
    case IC_RECEIVE:
        L(0, -0.40f, 0, 0.14f); L(-0.20f, -0.06f, 0, 0.14f); L(0.20f, -0.06f, 0, 0.14f);
        L(-0.38f, 0.16f, -0.38f, 0.38f); L(-0.38f, 0.38f, 0.38f, 0.38f); L(0.38f, 0.38f, 0.38f, 0.16f);
        break;
    case IC_SEND:
        L(0, 0.14f, 0, -0.40f); L(-0.20f, -0.20f, 0, -0.40f); L(0.20f, -0.20f, 0, -0.40f);
        L(-0.38f, 0.16f, -0.38f, 0.38f); L(-0.38f, 0.38f, 0.38f, 0.38f); L(0.38f, 0.38f, 0.38f, 0.16f);
        break;
    case IC_HISTORY:
        gfx_ring(cx, cy, 0.40f * s, t, c);
        L(0, 0, 0, -0.22f); L(0, 0, 0.17f, 0.09f);
        break;
    case IC_SETTINGS:
        gfx_ring(cx, cy, 0.27f * s, t, c);
        gfx_circle(cx, cy, 0.09f * s, c);
        for (int i = 0; i < 8; i++) {
            float a = (float)i * 0.785398f;
            gfx_line(cx + 0.33f * s * cosf(a), cy + 0.33f * s * sinf(a), cx + 0.45f * s * cosf(a), cy + 0.45f * s * sinf(a), t * 1.5f, c);
        }
        break;
    case IC_STAR:
    case IC_STAR_OUTLINE: {
        float pts[20];
        for (int i = 0; i < 10; i++) {
            float r = (i & 1) ? 0.20f : 0.48f, a = -1.570796f + (float)i * 0.628319f;
            pts[2 * i] = cx + r * s * cosf(a);
            pts[2 * i + 1] = cy + 0.03f * s + r * s * sinf(a);
        }
        if (icon == IC_STAR) {
            gfx_poly(pts, 10, c);
        } else {
            for (int i = 0; i < 10; i++) {
                int j = (i + 1) % 10;
                gfx_line(pts[2 * i], pts[2 * i + 1], pts[2 * j], pts[2 * j + 1], t, c);
            }
        }
        break;
    }
    case IC_FOLDER: {
        float p[14] = { -0.44f, -0.30f, -0.12f, -0.30f, -0.02f, -0.18f, 0.44f, -0.18f, 0.44f, 0.34f, -0.44f, 0.34f };
        float q[12];
        for (int i = 0; i < 6; i++) { q[2 * i] = cx + p[2 * i] * s; q[2 * i + 1] = cy + p[2 * i + 1] * s; }
        gfx_poly(q, 6, faint(c, 70));
        for (int i = 0; i < 6; i++) {
            int j = (i + 1) % 6;
            gfx_line(q[2 * i], q[2 * i + 1], q[2 * j], q[2 * j + 1], t, c);
        }
        break;
    }
    case IC_FILE: {
        float p[10] = { -0.30f, -0.44f, 0.12f, -0.44f, 0.32f, -0.24f, 0.32f, 0.44f, -0.30f, 0.44f };
        float q[10];
        for (int i = 0; i < 5; i++) { q[2 * i] = cx + p[2 * i] * s; q[2 * i + 1] = cy + p[2 * i + 1] * s; }
        gfx_poly(q, 5, faint(c, 60));
        for (int i = 0; i < 5; i++) {
            int j = (i + 1) % 5;
            gfx_line(q[2 * i], q[2 * i + 1], q[2 * j], q[2 * j + 1], t, c);
        }
        L(0.12f, -0.44f, 0.12f, -0.24f); L(0.12f, -0.24f, 0.32f, -0.24f);
        break;
    }
    case IC_PACKAGE: {
        float p[8] = { -0.40f, -0.34f, 0.40f, -0.34f, 0.40f, 0.42f, -0.40f, 0.42f };
        float q[8];
        for (int i = 0; i < 4; i++) { q[2 * i] = cx + p[2 * i] * s; q[2 * i + 1] = cy + p[2 * i + 1] * s; }
        gfx_poly(q, 4, faint(c, 60));
        for (int i = 0; i < 4; i++) {
            int j = (i + 1) % 4;
            gfx_line(q[2 * i], q[2 * i + 1], q[2 * j], q[2 * j + 1], t, c);
        }
        L(-0.40f, -0.06f, 0.40f, -0.06f);
        L(0.0f, -0.34f, 0.0f, -0.06f);
        L(-0.14f, 0.16f, 0.14f, 0.16f);
        break;
    }
    case IC_CHECK:
        gfx_line(cx - 0.38f * s, cy + 0.02f * s, cx - 0.12f * s, cy + 0.28f * s, t * 1.3f, c);
        gfx_line(cx - 0.12f * s, cy + 0.28f * s, cx + 0.40f * s, cy - 0.28f * s, t * 1.3f, c);
        break;
    case IC_CLOSE:
        L(-0.30f, -0.30f, 0.30f, 0.30f); L(-0.30f, 0.30f, 0.30f, -0.30f);
        break;
    case IC_PLUS:
        L(-0.36f, 0, 0.36f, 0); L(0, -0.36f, 0, 0.36f);
        break;
    case IC_PHONE:
        RR(-0.24f, -0.44f, 0.48f, 0.88f, 0.09f);
        L(-0.07f, 0.32f, 0.07f, 0.32f);
        break;
    case IC_DESKTOP:
        RR(-0.45f, -0.42f, 0.90f, 0.58f, 0.07f);
        L(0, 0.16f, 0, 0.34f); L(-0.20f, 0.38f, 0.20f, 0.38f);
        break;
    case IC_LAPTOP:
        RR(-0.33f, -0.38f, 0.66f, 0.46f, 0.05f);
        L(-0.47f, 0.22f, 0.47f, 0.22f);
        break;
    case IC_WEB:
        gfx_ring(cx, cy, 0.40f * s, t, c);
        L(-0.40f, 0, 0.40f, 0); L(0, -0.40f, 0, 0.40f); L(-0.33f, -0.2f, 0.33f, -0.2f); L(-0.33f, 0.2f, 0.33f, 0.2f);
        break;
    case IC_SERVER:
        RR(-0.40f, -0.40f, 0.80f, 0.34f, 0.06f); RR(-0.40f, 0.06f, 0.80f, 0.34f, 0.06f);
        gfx_circle(cx - 0.26f * s, cy - 0.23f * s, 0.045f * s, c); gfx_circle(cx - 0.26f * s, cy + 0.23f * s, 0.045f * s, c);
        break;
    case IC_VITA: {
        /* PS Vita front: wide rounded body, big screen between D-pad + left stick and face buttons + right stick */
        float tb = t * 0.72f, tt = s * 0.032f;
        if (tt < 1.1f) tt = 1.1f;
        int bx0 = (int)floorf(cx - 0.50f * s + 0.5f), by0 = (int)floorf(cy - 0.25f * s + 0.5f);
        gfx_rrect_outline(bx0, by0, (int)floorf(1.0f * s + 0.5f), (int)floorf(0.50f * s + 0.5f), (int)(0.17f * s + 0.5f), (int)(tb + 0.5f), c);
        int sx0 = (int)floorf(cx - 0.215f * s + 0.5f), sy0 = (int)floorf(cy - 0.17f * s + 0.5f);
        gfx_rrect(sx0, sy0, (int)floorf(0.43f * s + 0.5f), (int)floorf(0.34f * s + 0.5f), 2, faint(c, 90));
        /* left: D-pad above, analog stick below */
        float lx = cx - 0.355f * s, rx = cx + 0.355f * s;
        gfx_line(lx, cy - 0.17f * s, lx, cy - 0.03f * s, tt * 1.7f, c);
        gfx_line(lx - 0.07f * s, cy - 0.10f * s, lx + 0.07f * s, cy - 0.10f * s, tt * 1.7f, c);
        gfx_ring(lx, cy + 0.11f * s, 0.05f * s, tt, c);
        /* right: face buttons above, analog stick below */
        float by = cy - 0.10f * s, d = 0.055f * s, r = 0.022f * s + 0.35f;
        gfx_circle(rx, by - d, r, c); gfx_circle(rx, by + d, r, c);
        gfx_circle(rx - d, by, r, c); gfx_circle(rx + d, by, r, c);
        gfx_ring(rx, cy + 0.11f * s, 0.05f * s, tt, c);
        break;
    }
    case IC_MESSAGE:
        RR(-0.44f, -0.38f, 0.88f, 0.58f, 0.10f);
        L(-0.20f, 0.20f, -0.30f, 0.40f); L(-0.30f, 0.40f, 0.0f, 0.20f);
        L(-0.24f, -0.17f, 0.24f, -0.17f); L(-0.24f, 0.0f, 0.10f, 0.0f);
        break;
    case IC_ARROW_DOWN:
        L(0, -0.36f, 0, 0.36f); L(-0.22f, 0.14f, 0, 0.36f); L(0.22f, 0.14f, 0, 0.36f);
        break;
    case IC_ARROW_UP:
        L(0, 0.36f, 0, -0.36f); L(-0.22f, -0.14f, 0, -0.36f); L(0.22f, -0.14f, 0, -0.36f);
        break;
    case IC_ARROW_LEFT:
        L(0.36f, 0, -0.36f, 0); L(-0.14f, -0.22f, -0.36f, 0); L(-0.14f, 0.22f, -0.36f, 0);
        break;
    case IC_BACK:
        L(0.14f, -0.34f, -0.18f, 0); L(-0.18f, 0, 0.14f, 0.34f);
        break;
    case IC_WIFI:
        gfx_circle(cx, cy + 0.32f * s, 0.07f * s + 0.5f, c);
        arc(cx, cy + 0.32f * s, 0.28f * s, -2.356f, -0.785f, t, c);
        arc(cx, cy + 0.32f * s, 0.52f * s, -2.356f, -0.785f, t, c);
        arc(cx, cy + 0.32f * s, 0.76f * s, -2.356f, -0.785f, t, c);
        break;
    case IC_WIFI_OFF:                                   /* the Wi-Fi symbol with a slash through it */
        gfx_circle(cx, cy + 0.32f * s, 0.07f * s + 0.5f, c);
        arc(cx, cy + 0.32f * s, 0.28f * s, -2.356f, -0.785f, t, c);
        arc(cx, cy + 0.32f * s, 0.52f * s, -2.356f, -0.785f, t, c);
        arc(cx, cy + 0.32f * s, 0.76f * s, -2.356f, -0.785f, t, c);
        L(-0.46f, -0.46f, 0.46f, 0.44f);
        break;
    case IC_REFRESH:
        arc(cx, cy, 0.34f * s, -1.2f, 3.6f, t, c);
        L(0.14f, -0.40f, 0.28f, -0.18f); L(0.28f, -0.18f, 0.02f, -0.12f);
        break;
    default:
        break;
    }
#undef L
#undef RR
}

/* ============================================================== PlayStation buttons */

static gcol pad_color(int b)
{
    switch (b) {
    case PB_CROSS:    return GRGB(118, 166, 244);
    case PB_CIRCLE:   return GRGB(242, 104, 124);
    case PB_SQUARE:   return GRGB(222, 140, 220);
    case PB_TRIANGLE: return GRGB(98, 212, 164);
    default:          return GRGB(196, 204, 218);
    }
}

int gfx_pad_button_width(int b, int size)
{
    switch (b) {
    case PB_L: case PB_R: return size * 3 / 2;
    case PB_START: return size * 3;
    case PB_SELECT: return size * 3 + size / 2;
    case PB_DPAD_LR: case PB_DPAD_UD: case PB_DPAD: return size;
    default: return size;
    }
}

int gfx_pad_button(int b, int cx, int cy, int size)
{
    int w = gfx_pad_button_width(b, size);
    float r = (float)size * 0.5f;
    const char *rec = "?";
    gcol col = pad_color(b);
    gcol disc = GRGB(48, 56, 68);
    float fx = (float)cx, fy = (float)cy;
    gfx_norec(true);
    switch (b) {
    case PB_CROSS: case PB_CIRCLE: case PB_SQUARE: case PB_TRIANGLE: {
        gfx_circle(fx, fy, r, disc);
        gfx_ring(fx, fy, r - 0.8f, 1.4f, faint(col, 150));
        float k = r * 0.42f, t = r * 0.17f > 1.5f ? r * 0.17f : 1.5f;
        if (b == PB_CROSS) {
            gfx_line(fx - k, fy - k, fx + k, fy + k, t, col);
            gfx_line(fx - k, fy + k, fx + k, fy - k, t, col);
            rec = "[X]";
        } else if (b == PB_CIRCLE) {
            gfx_ring(fx, fy, k * 1.05f, t, col);
            rec = "[O]";
        } else if (b == PB_SQUARE) {
            gfx_rrect_outline(cx - (int)(k * 0.95f), cy - (int)(k * 0.95f), (int)(k * 1.9f), (int)(k * 1.9f), 1, (int)(t + 0.5f), col);
            rec = "[SQ]";
        } else {
            float p[6] = { fx, fy - k * 1.1f, fx + k * 1.1f, fy + k * 0.8f, fx - k * 1.1f, fy + k * 0.8f };
            for (int i = 0; i < 3; i++) {
                int j = (i + 1) % 3;
                gfx_line(p[2 * i], p[2 * i + 1], p[2 * j], p[2 * j + 1], t, col);
            }
            rec = "[TRI]";
        }
        break;
    }
    case PB_L: case PB_R: {
        int h = size * 3 / 4;
        gfx_rrect(cx - w / 2, cy - h / 2, w, h, h / 2, disc);
        gfx_text(cx, cy, size * 11 / 20 + 1, GW_BOLD, col, GA_CENTER, b == PB_L ? "L" : "R");
        rec = b == PB_L ? "[L]" : "[R]";
        break;
    }
    case PB_START: case PB_SELECT: {
        int h = size * 3 / 4;
        gfx_rrect(cx - w / 2, cy - h / 2, w, h, h / 2, disc);
        gfx_text(cx, cy, size * 2 / 5 + 1, GW_MED, col, GA_CENTER, b == PB_START ? "START" : "SELECT");
        rec = b == PB_START ? "[START]" : "[SEL]";
        break;
    }
    default: {
        gcol on = GRGB(214, 222, 234), off = GRGB(84, 94, 110);
        int arm = (int)(r * 0.62f), th = (int)(r * 0.62f);
        gfx_rrect(cx - (int)r, cy - th / 2, (int)(r * 2), th, th / 3, (b == PB_DPAD_UD) ? off : on);
        gfx_rrect(cx - th / 2, cy - (int)r, th, (int)(r * 2), th / 3, (b == PB_DPAD_LR) ? off : on);
        (void)arm;
        rec = b == PB_DPAD_LR ? "[LEFT/RIGHT]" : (b == PB_DPAD_UD ? "[UP/DOWN]" : "[D-PAD]");
        break;
    }
    }
    gfx_norec(false);
    gfx_record(cx - w / 2, cy, rec);
    return w;
}
