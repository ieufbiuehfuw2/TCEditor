# TClient editor icon sketches: renders four 512x512 concept variants
# (dark rounded-square background, editor-theme red accent)
from PIL import Image, ImageDraw
import math
import os

SS = 4
S = 512 * SS

BG = (23, 23, 28, 255)
BG_BORDER = (46, 46, 54, 255)
TOOL = (217, 217, 222, 255)
TOOL_DARK = (138, 138, 146, 255)
ACCENT = (193, 24, 47, 255)
ACCENT_SOFT = (224, 27, 51, 255)
TILE = (38, 38, 46, 255)
TILE_BORDER = (58, 58, 68, 255)
OUT_DIR = os.path.dirname(os.path.abspath(__file__))


def canvas():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([60, 60, S - 60, S - 60], radius=420, fill=BG, outline=BG_BORDER, width=12)
    return img


def rot_paste(base, layer, angle, center):
    r = layer.rotate(angle, expand=True, resample=Image.BICUBIC)
    x = int(center[0] - r.width / 2)
    y = int(center[1] - r.height / 2)
    base.alpha_composite(r, (max(0, x), max(0, y)))


def brush_layer(tip_len=400):
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = S // 2
    d.rounded_rectangle([cx - 70, 250, cx + 70, 1150], radius=60, fill=TOOL)
    d.rounded_rectangle([cx - 85, 1150, cx + 85, 1380], radius=30, fill=TOOL_DARK)
    d.polygon([(cx - 85, 1380), (cx + 85, 1380), (cx + 20, 1380 + tip_len), (cx - 20, 1380 + tip_len)], fill=(176, 176, 184, 255))
    d.polygon([(cx - 48, 1380 + tip_len - 150), (cx + 48, 1380 + tip_len - 150), (cx + 20, 1380 + tip_len), (cx - 20, 1380 + tip_len)], fill=ACCENT)
    return img


def mahl_layer():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = S // 2
    d.rounded_rectangle([cx - 30, 420, cx + 30, 1720], radius=30, fill=TOOL)
    d.ellipse([cx - 115, 210, cx + 115, 440], fill=ACCENT)
    d.ellipse([cx - 55, 255, cx + 5, 315], fill=(255, 255, 255, 190))
    return img


def stroke(d, pts, width, color):
    for i in range(len(pts) - 1):
        d.line([pts[i], pts[i + 1]], fill=color, width=width)
    for p in (pts[0], pts[-1]):
        r = width / 2
        d.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=color)


def bezier(p0, p1, p2, n=40):
    return [( (1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t * t * p2[0],
              (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t * t * p2[1]) for t in [i / n for i in range(n + 1)]]


def save(img, name):
    img = img.resize((512, 512), Image.LANCZOS)
    path = os.path.join(OUT_DIR, name)
    img.save(path)
    print("saved", path)


# A: paintbrush with a paint stroke
img = canvas()
rot_paste(img, brush_layer(), -45, (S * 0.56, S * 0.44))
d = ImageDraw.Draw(img)
stroke(d, bezier((S * 0.30, S * 0.62), (S * 0.42, S * 0.82), (S * 0.74, S * 0.80)), 64, ACCENT)
d.ellipse([S * 0.68, S * 0.22, S * 0.74, S * 0.28], fill=ACCENT_SOFT)
save(img, "sketch_A_brush.png")

# B: mahlstick with a paint dot under the head
img = canvas()
rot_paste(img, mahl_layer(), -40, (S * 0.52, S * 0.48))
d = ImageDraw.Draw(img)
d.ellipse([S * 0.24, S * 0.72, S * 0.32, S * 0.80], fill=ACCENT)
d.ellipse([S * 0.66, S * 0.18, S * 0.70, S * 0.22], fill=ACCENT_SOFT)
save(img, "sketch_B_mahlstick.png")

# C: brush painting over a tile grid
img = canvas()
tile = 360
gap = 26
grid = 4 * tile + 3 * gap
ox = (S - grid) // 2
oy = (S - grid) // 2
d = ImageDraw.Draw(img)
painted = {(1, 1): ACCENT, (2, 1): ACCENT, (1, 2): ACCENT_SOFT, (2, 2): (232, 232, 236, 255)}
for gy in range(4):
    for gx in range(4):
        x0 = ox + gx * (tile + gap)
        y0 = oy + gy * (tile + gap)
        color = painted.get((gx, gy), TILE)
        d.rounded_rectangle([x0, y0, x0 + tile, y0 + tile], radius=48, fill=color, outline=TILE_BORDER, width=8)
b = brush_layer()
b = b.resize((int(S * 0.82), int(S * 0.82)), Image.LANCZOS)
rot_paste(img, b, -45, (S * 0.60, S * 0.42))
stroke(d, bezier((S * 0.33, S * 0.66), (S * 0.45, S * 0.74), (S * 0.58, S * 0.72)), 46, ACCENT)
save(img, "sketch_C_tiles.png")

# D: mahlstick and brush crossed
img = canvas()
rot_paste(img, mahl_layer(), 38, (S * 0.5, S * 0.47))
rot_paste(img, brush_layer(), -38, (S * 0.5, S * 0.5))
d = ImageDraw.Draw(img)
d.ellipse([S * 0.46, S * 0.44, S * 0.54, S * 0.52], fill=ACCENT_SOFT)
save(img, "sketch_D_crossed.png")

# contact sheet 2x2
sheet = Image.new("RGBA", (512 * 2 + 30, 512 * 2 + 30), (10, 10, 12, 255))
for i, name in enumerate(["sketch_A_brush.png", "sketch_B_mahlstick.png", "sketch_C_tiles.png", "sketch_D_crossed.png"]):
    icon = Image.open(os.path.join(OUT_DIR, name)).convert("RGBA")
    x = 10 + (i % 2) * (512 + 10)
    y = 10 + (i // 2) * (512 + 10)
    sheet.alpha_composite(icon, (x, y))
sheet.save(os.path.join(OUT_DIR, "sketch_all.png"))
print("saved contact sheet")
