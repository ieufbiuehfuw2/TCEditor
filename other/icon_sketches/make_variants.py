# TClient icon variants: the original DDNet icon combined with editor motifs
from PIL import Image, ImageDraw
import colorsys
import os

SS = 4
S = 256 * SS  # 1024 compose space
OUT_DIR = os.path.dirname(os.path.abspath(__file__))
ACCENT = (193, 24, 47, 255)
ACCENT_SOFT = (224, 27, 51, 255)
DARK = (23, 23, 28, 255)

original = Image.open(os.path.join(OUT_DIR, "original_256.png")).convert("RGBA")
original = original.resize((S, S), Image.LANCZOS)


def canvas():
    return Image.new("RGBA", (S, S), (0, 0, 0, 0))


def rot_paste(base, layer, angle, center):
    r = layer.rotate(angle, expand=True, resample=Image.BICUBIC)
    x = int(center[0] - r.width / 2)
    y = int(center[1] - r.height / 2)
    base.alpha_composite(r, (max(0, x), max(0, y)))


def stroke(d, pts, width, color):
    for i in range(len(pts) - 1):
        d.line([pts[i], pts[i + 1]], fill=color, width=width)
    for p in (pts[0], pts[-1]):
        r = width / 2
        d.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=color)


def bezier(p0, p1, p2, n=40):
    return [((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t * t * p2[0],
             (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t * t * p2[1]) for t in [i / n for i in range(n + 1)]]


def mini_brush(scale=0.30):
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = S // 2
    d.rounded_rectangle([cx - 40, 120, cx + 40, 560], radius=34, fill=(220, 220, 225, 255))
    d.rounded_rectangle([cx - 48, 560, cx + 48, 690], radius=18, fill=(138, 138, 146, 255))
    d.polygon([(cx - 48, 690), (cx + 48, 690), (cx + 12, 900), (cx - 12, 900)], fill=(176, 176, 184, 255))
    d.polygon([(cx - 27, 830), (cx + 27, 830), (cx + 12, 900), (cx - 12, 900)], fill=ACCENT)
    return img.resize((int(S * scale), int(S * scale)), Image.LANCZOS)


def save(img, name):
    img = img.resize((512, 512), Image.LANCZOS)
    path = os.path.join(OUT_DIR, name)
    img.save(path)
    print("saved", path)


# V1: paint stroke across the head + small brush
img = canvas()
img.alpha_composite(original)
d = ImageDraw.Draw(img)
stroke(d, bezier((S * 0.16, S * 0.60), (S * 0.42, S * 0.74), (S * 0.70, S * 0.58)), 52, ACCENT)
rot_paste(img, mini_brush(0.34), -125, (S * 0.75, S * 0.52))
save(img, "v1_stroke.png")

# V2: red editor badge with a brush glyph in the corner
img = canvas()
img.alpha_composite(original)
d = ImageDraw.Draw(img)
cx, cy, r = S * 0.78, S * 0.78, S * 0.22
d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=ACCENT, outline=(15, 15, 18, 255), width=10)
badge = mini_brush(0.42).rotate(-35, expand=True, resample=Image.BICUBIC)
img.alpha_composite(badge, (int(cx - badge.width / 2), int(cy - badge.height / 2)))
save(img, "v2_badge.png")

# V3: mahlstick leaning next to the tee
img = canvas()
img.alpha_composite(original)
mahl = Image.new("RGBA", (S, S), (0, 0, 0, 0))
dm = ImageDraw.Draw(mahl)
cx = S // 2
dm.rounded_rectangle([cx - 26, 260, cx + 26, 800], radius=26, fill=(220, 220, 225, 255))
dm.ellipse([cx - 96, 110, cx + 96, 302], fill=ACCENT)
dm.ellipse([cx - 45, 150, cx + 5, 200], fill=(255, 255, 255, 190))
mahl = mahl.resize((int(S * 0.62), int(S * 0.62)), Image.LANCZOS)
rot_paste(img, mahl, -28, (S * 0.80, S * 0.40))
save(img, "v3_mahlstick.png")

# V4: recolor the tee head to the editor red, water kept blue
img = original.copy()
px = img.load()
for y in range(S):
    for x in range(S):
        r0, g0, b0, a0 = px[x, y]
        if a0 == 0:
            continue
        h, s, v = colorsys.rgb_to_hsv(r0 / 255, g0 / 255, b0 / 255)
        hue = h * 360
        if 25 <= hue <= 65 and s > 0.35:  # orange/yellow head
            nh = (hue - 25) / 40  # 0..1 across the head gradient
            r1, g1, b1 = colorsys.hsv_to_rgb(352 / 360, min(1.0, s * 1.05), v)
            px[x, y] = (int(r1 * 255), int(g1 * 255), int(b1 * 255), a0)
img_out = canvas()
img_out.alpha_composite(img)
save(img_out, "v4_recolor.png")

# V5: dark editor tile + original + stroke
img = canvas()
d = ImageDraw.Draw(img)
d.rounded_rectangle([30, 30, S - 30, S - 30], radius=210, fill=DARK, outline=(46, 46, 54, 255), width=8)
small = original.resize((int(S * 0.86), int(S * 0.86)), Image.LANCZOS)
img.alpha_composite(small, (int((S - small.width) / 2), int((S - small.height) / 2)))
d = ImageDraw.Draw(img)
stroke(d, bezier((S * 0.18, S * 0.58), (S * 0.42, S * 0.72), (S * 0.68, S * 0.56)), 46, ACCENT)
save(img, "v5_dark.png")

# contact sheet 2 rows
sheet = Image.new("RGBA", (512 * 3 + 40, 512 * 2 + 30), (10, 10, 12, 255))
names = ["v1_stroke.png", "v2_badge.png", "v3_mahlstick.png", "v4_recolor.png", "v5_dark.png", "original_256.png"]
for i, name in enumerate(names):
    icon = Image.open(os.path.join(OUT_DIR, name)).convert("RGBA")
    x = 20 + (i % 3) * (512 + 10)
    y = 10 + (i // 3) * (512 + 10)
    sheet.alpha_composite(icon, (x, y))
sheet = sheet.resize((sheet.width // 2, sheet.height // 2), Image.LANCZOS)
sheet.save(os.path.join(OUT_DIR, "variants_all.png"))
print("saved contact sheet")
