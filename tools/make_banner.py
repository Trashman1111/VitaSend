#!/usr/bin/env python3
# Author: trashman (VitaSend)
"""
Makes docs/images/banner.png (1280x640: README header and GitHub "social preview") from the screenshots in docs/screens/.
Needs: pip install pillow     Run: python3 tools/make_banner.py
"""
import os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
SCREENS = os.path.join(HERE, "..", "docs", "screens")
OUT = os.path.join(HERE, "..", "docs", "images", "banner.png")
W, H = 1280, 640
TEAL = (38, 166, 154)

def font(size, bold=False):
    for p in ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" if bold else "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
              "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(p, size)
        except OSError:
            pass
    return ImageFont.load_default()

def gradient(w, h, top, bottom):
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / (h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
    return img

def logo(d, cx, cy, s, t, color):
    """The two-arrow logo (same geometry as IC_LOGO in src/gfx_icons.c)."""
    segs = [(-0.34, -0.14, 0.34, -0.14), (0.34, -0.14, 0.16, -0.31), (0.34, -0.14, 0.16, 0.03),
            (0.34, 0.17, -0.34, 0.17), (-0.34, 0.17, -0.16, 0.0), (-0.34, 0.17, -0.16, 0.34)]
    for x0, y0, x1, y1 in segs:
        p0 = (cx + x0 * s, cy + y0 * s); p1 = (cx + x1 * s, cy + y1 * s)
        d.line([p0, p1], fill=color, width=int(t))
        for (px, py) in (p0, p1):
            d.ellipse([px - t / 2, py - t / 2, px + t / 2, py + t / 2], fill=color)

def rounded(img, radius):
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, img.size[0] - 1, img.size[1] - 1], radius, fill=255)
    out = img.convert("RGBA"); out.putalpha(mask)
    return out

def card(shot, width, canvas, pos, angle=0):
    """Pastes a screenshot as a rounded 'screen' with a soft shadow."""
    im = Image.open(os.path.join(SCREENS, shot)).convert("RGB")
    h = int(im.size[1] * width / im.size[0])
    im = rounded(im.resize((width, h), Image.LANCZOS), 18)
    pad = 40
    sh = Image.new("RGBA", (width + 2 * pad, h + 2 * pad), (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle([pad, pad + 10, pad + width, pad + h + 10], 18, fill=(0, 0, 0, 170))
    sh = sh.filter(ImageFilter.GaussianBlur(16))
    frame = Image.new("RGBA", im.size, (0, 0, 0, 0))
    ImageDraw.Draw(frame).rounded_rectangle([0, 0, im.size[0] - 1, im.size[1] - 1], 18, outline=(255, 255, 255, 60), width=2)
    layer = Image.new("RGBA", sh.size, (0, 0, 0, 0))
    layer.alpha_composite(sh)
    layer.alpha_composite(im, (pad, pad))
    layer.alpha_composite(frame, (pad, pad))
    if angle:
        layer = layer.rotate(angle, resample=Image.BICUBIC, expand=True)
    canvas.alpha_composite(layer, (pos[0] - pad, pos[1] - pad))

S = 2                                                   # draw twice as big, then shrink: smooth edges
bg = gradient(W * S, H * S, (11, 28, 42), (10, 62, 80)).convert("RGBA")
d = ImageDraw.Draw(bg)
for i, r in enumerate(range(160, 2400, 230)):           # the waves of the LiveArea background
    col = (22 + i * 3, 92 + i * 5, 112 + i * 4)
    d.arc([160 * S - r, 560 * S - r, 160 * S + r, 560 * S + r], -62, 62, fill=col, width=5 * S)
canvas = bg.resize((W, H), Image.LANCZOS)

# right: the screenshots
card("send.png", 520, canvas, (730, 318), angle=0)
card("receive.png", 540, canvas, (690, 64), angle=0)

# left: logo, name, tagline
d = ImageDraw.Draw(canvas)
d.rounded_rectangle([70, 90, 190, 210], 34, fill=TEAL)
big = Image.new("RGBA", (120 * 4, 120 * 4), (0, 0, 0, 0))
logo(ImageDraw.Draw(big), 60 * 4, 60 * 4, 100 * 4, 100 * 4 * 0.085 * 1.05, (255, 255, 255, 255))
canvas.alpha_composite(big.resize((120, 120), Image.LANCZOS), (70, 90))
d.text((70, 250), "VitaSend", font=font(84, True), fill=(255, 255, 255))
d.text((74, 352), "Send and receive files on your PS Vita", font=font(28), fill=(214, 232, 236))
d.text((74, 392), "with LocalSend - no cloud, no cable.", font=font(28), fill=(214, 232, 236))
x = 74
for label in ("Files", "Folders", "Text", ".vpk installer"):
    f = font(22, True)
    w = int(d.textlength(label, font=f)) + 36
    d.rounded_rectangle([x, 480, x + w, 524], 22, fill=(38, 166, 154, 255))
    d.text((x + 18, 490), label, font=f, fill=(255, 255, 255))
    x += w + 14
d.text((74, 570), "native homebrew for PS Vita  -  free and open source  -  GPL-3.0", font=font(17), fill=(150, 190, 198))

os.makedirs(os.path.dirname(OUT), exist_ok=True)
canvas.convert("RGB").save(OUT, optimize=True)
print("wrote", os.path.abspath(OUT))
