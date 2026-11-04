#!/usr/bin/env python3
# Author: trashman (VitaSend)
"""
Regenerates the bubble / LiveArea images in sce_sys/.   (Already done - the PNGs are in the project.)
Needs: pip install pillow
The PS Vita wants 8-bit paletted PNGs, which is what this writes.
"""
import math, os
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sce_sys")
os.makedirs(os.path.join(OUT, "livearea", "contents"), exist_ok=True)

def gradient(w, h, top, bottom):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = c
    return img

def waves(d, cx, cy, radii, width, color, start=-50, end=50):
    for r in radii:
        d.arc([cx - r, cy - r, cx + r, cy + r], start, end, fill=color, width=width)

def save8(img, path):
    img.convert("RGB").quantize(colors=128, method=Image.MEDIANCUT, dither=Image.NONE).save(path, optimize=True)

def font(size, bold=False):
    for p in ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" if bold else "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
              "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(p, size)
        except OSError:
            pass
    return ImageFont.load_default()

TEAL = (38, 166, 154)

def logo(d, cx, cy, s, t, color, S=1):
    """The in-app logo (two arrows, same geometry as IC_LOGO in src/gfx_icons.c); s = icon size, t = line width."""
    segs = [(-0.34, -0.14, 0.34, -0.14), (0.34, -0.14, 0.16, -0.31), (0.34, -0.14, 0.16, 0.03),
            (0.34, 0.17, -0.34, 0.17), (-0.34, 0.17, -0.16, 0.0), (-0.34, 0.17, -0.16, 0.34)]
    for x0, y0, x1, y1 in segs:
        p0 = (cx + x0 * s, cy + y0 * s); p1 = (cx + x1 * s, cy + y1 * s)
        d.line([p0, p1], fill=color, width=int(t))
        for (px, py) in (p0, p1):
            d.ellipse([px - t / 2, py - t / 2, px + t / 2, py + t / 2], fill=color)

# ---- icon0.png 128x128 : the bubble on the home screen = the logo from the top left of the app
S = 4                                              # draw 4x bigger, then shrink for smooth edges
img = Image.new("RGB", (128 * S, 128 * S), TEAL)
d = ImageDraw.Draw(img)
logo(d, 64 * S, 64 * S, 104 * S, 104 * S * 0.085 * 1.05, (255, 255, 255))
img = img.resize((128, 128), Image.LANCZOS)
save8(img, os.path.join(OUT, "icon0.png"))

# ---- livearea/contents/bg0.png 840x500
img = gradient(840 * 2, 500 * 2, (12, 30, 44), (10, 58, 78))
d = ImageDraw.Draw(img)
for i, r in enumerate(range(120, 1500, 150)):
    waves(d, 150, 500, [r], 6, (22 + i * 3, 92 + i * 5, 112 + i * 4), -60, 60)
img = img.resize((840, 500), Image.LANCZOS)
save8(img, os.path.join(OUT, "livearea", "contents", "bg0.png"))

# ---- livearea/contents/startup.png 280x158 : the "start" button
img = Image.new("RGB", (280 * 4, 158 * 4), TEAL)
d = ImageDraw.Draw(img)
# Everything sits in the upper part: the system draws its own "start" marker over the lower edge of the button.
# The two text lines are centred vertically on the logo (same centre line).
CY = 66
logo(d, 78 * 4, CY * 4, 88 * 4, 88 * 4 * 0.085 * 1.05, (255, 255, 255))
f = font(29 * 4, True)
d.text((142 * 4, (CY - 16) * 4), "Vita", font=f, fill=(255, 255, 255), anchor="lm")
d.text((142 * 4, (CY + 16) * 4), "Send", font=f, fill=(255, 255, 255), anchor="lm")
img = img.resize((280, 158), Image.LANCZOS)
save8(img, os.path.join(OUT, "livearea", "contents", "startup.png"))

with open(os.path.join(OUT, "livearea", "contents", "template.xml"), "w") as f:
    f.write('''<?xml version="1.0" encoding="utf-8" standalone="yes"?>
<livearea style="a1" format-ver="01.00" content-rev="1">
  <livearea-background>
    <image>bg0.png</image>
  </livearea-background>
  <gate>
    <startup-image>startup.png</startup-image>
  </gate>
</livearea>
''')
print("assets written to", os.path.abspath(OUT))
