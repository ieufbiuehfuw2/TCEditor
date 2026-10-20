# Final icon variant: original tee without water ripples + a paintbrush leaning on it
from PIL import Image, ImageDraw
import os
from collections import deque

SS = 4
S = 256 * SS
OUT_DIR = os.path.dirname(os.path.abspath(__file__))
ACCENT = (193, 24, 47, 255)

original = Image.open(os.path.join(OUT_DIR, "original_256.png")).convert("RGBA")
original = original.resize((S, S), Image.LANCZOS)

# 1) remove the blue water ripples (hue-based), keep everything else
px = original.load()
for y in range(S):
    for x in range(S):
        r0, g0, b0, a0 = px[x, y]
        if a0 == 0:
            continue
        if b0 > r0 + 20 and b0 > g0 + 10:  # blue water
            px[x, y] = (0, 0, 0, 0)

# 2) keep only the largest connected non-transparent blob (head+rock+outline),
#    dropping orphaned outline fragments of the removed ripples
w, h = S, S
labels = [[0] * w for _ in range(h)]
sizes = {}
best_label = 0
best_size = 0
cur = 0
for sy in range(h):
    for sx in range(w):
        if labels[sy][sx] != 0 or px[sx, sy][3] == 0:
            continue
        cur += 1
        q = deque([(sx, sy)])
        labels[sy][sx] = cur
        count = 0
        while q:
            x, y = q.popleft()
            count += 1
            for nx, ny in ((x+1,y),(x-1,y),(x,y+1),(x,y-1)):
                if 0 <= nx < w and 0 <= ny < h and labels[ny][nx] == 0 and px[nx, ny][3] != 0:
                    labels[ny][nx] = cur
                    q.append((nx, ny))
        sizes[cur] = count
        if count > best_size:
            best_size = count
            best_label = cur
for y in range(h):
    for x in range(w):
        if px[x, y][3] != 0 and labels[y][x] != best_label:
            px[x, y] = (0, 0, 0, 0)

original.save(os.path.join(OUT_DIR, "tee_nowater.png"))
print("water removed, largest blob:", best_size)
