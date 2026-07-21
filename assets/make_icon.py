#!/usr/bin/env python3
"""Generates the app icon (a mini RTA in the app's dark theme) at
assets/icon-1024.png + icon-256.png + icon.ico. Run from the repo root:
    python3 assets/make_icon.py
then rebuild macos/AppIcon.icns with:
    iconutil -c icns <iconset dir>   (see build steps in this file)
"""
from PIL import Image, ImageDraw

S = 1024
# macOS icon grid: artwork occupies ~824/1024 with transparent margin.
M = 100                      # margin
R = 185                      # corner radius
BG_TOP, BG_BOT = (27, 32, 43), (14, 16, 22)          # #1b202b -> #0e1016
BORDER = (42, 47, 61)                                 # #2a2f3d
BAR, BAR_TOP = (47, 191, 155), (92, 224, 189)         # #2fbf9b / #5ce0bd
PEAK = (224, 192, 92)                                 # #e0c05c

img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
draw = ImageDraw.Draw(img)

# Vertical gradient inside a rounded-rect mask.
grad = Image.new("RGBA", (S, S))
gd = ImageDraw.Draw(grad)
for y in range(M, S - M):
    t = (y - M) / (S - 2 * M)
    c = tuple(int(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOT))
    gd.line([(M, y), (S - M, y)], fill=c + (255,))
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle([M, M, S - M, S - M], radius=R, fill=255)
img.paste(grad, (0, 0), mask)
draw.rounded_rectangle([M + 3, M + 3, S - M - 3, S - M - 3], radius=R - 3,
                       outline=BORDER + (255,), width=6)

# Mini RTA: 11 bars with a pink-noise-ish shape peaking left of center.
heights = [0.30, 0.44, 0.58, 0.70, 0.60, 0.82, 0.96, 0.74, 0.54, 0.40, 0.27]
x0, x1 = 178, S - 178
base = 806
maxh = 470
n = len(heights)
slot = (x1 - x0) / n
bw = slot * 0.72
for i, h in enumerate(heights):
    bx = x0 + i * slot + (slot - bw) / 2
    top = base - h * maxh
    draw.rounded_rectangle([bx, top, bx + bw, base], radius=10,
                           fill=BAR + (255,))
    draw.rounded_rectangle([bx, top, bx + bw, top + 16], radius=8,
                           fill=BAR_TOP + (255,))
# Peak-hold ticks above the three tallest bars.
for i in (5, 6, 7):
    bx = x0 + i * slot + (slot - bw) / 2
    top = base - heights[i] * maxh - 52
    draw.rounded_rectangle([bx, top, bx + bw, top + 16], radius=8,
                           fill=PEAK + (255,))

img.save("assets/icon-1024.png")
img.resize((256, 256), Image.LANCZOS).save("assets/icon-256.png")
img.resize((256, 256), Image.LANCZOS).save(
    "assets/icon.ico",
    sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128),
           (256, 256)])
print("wrote assets/icon-1024.png, icon-256.png, icon.ico")
