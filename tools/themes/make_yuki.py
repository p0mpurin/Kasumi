#!/usr/bin/env python3
"""A drawn wallpaper for the Yuki (snow) theme, made from scratch so it is ours.

    python tools/themes/make_yuki.py

Writes resources/yuki-pattern.png (1280x960): 雪輪 snow rings and 雪華
snowflakes, after the Edo-period kimono patterns, on a winter-night indigo,
with a few sprigs of 南天 nanten (red winter berries) for the season.

The theme ships a photo of ice instead (resources/themes/yuki.jpg: the top
4:3 of the source, red x0.80 and green x0.92 for a cool tint). To use this
pattern, copy it to resources/themes/yuki.png in place of the photo and run
`python tools/themes/make_themes.py yuki`. Needs Pillow.
"""
import math
import os
import random

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, 'resources', 'yuki-pattern.png')
W, H = 1280, 960
SS = 2  # drawn at twice the size, then shrunk: smooth edges

ICE = (206, 222, 238)
FROST = (150, 174, 204)
SLATE = (92, 114, 146)
BERRY = (196, 48, 58)
LEAF = (52, 84, 66)


def ring_outline(cx, cy, r, steps=360):
    """雪輪: a circle with six soft lobes and six notches between them."""
    pts = []
    for i in range(steps):
        a = 2 * math.pi * i / steps
        lobe = abs(math.sin(3 * a)) ** 0.45
        rr = r * (0.84 + 0.16 * lobe)
        pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    return pts


def snow_ring(d, cx, cy, r, color, filled, rot=0.0):
    outer = ring_outline(cx, cy, r)
    if rot:
        outer = [(cx + (x - cx) * math.cos(rot) - (y - cy) * math.sin(rot),
                  cy + (x - cx) * math.sin(rot) + (y - cy) * math.cos(rot)) for x, y in outer]
    if filled:
        d.polygon(outer, fill=color)
    else:
        d.line(outer + [outer[0]], fill=color, width=max(2, int(r * 0.07)), joint='curve')


def snowflake(d, cx, cy, size, color, rot):
    """雪華: six arms, each with two pairs of side branches."""
    w = max(2, int(size * 0.075))
    for k in range(6):
        a = rot + k * math.pi / 3
        ex, ey = cx + size * math.cos(a), cy + size * math.sin(a)
        d.line([(cx, cy), (ex, ey)], fill=color, width=w)
        for at, length in ((0.42, 0.34), (0.68, 0.24)):
            bx, by = cx + size * at * math.cos(a), cy + size * at * math.sin(a)
            for side in (-1, 1):
                b = a + side * math.pi / 3
                d.line([(bx, by), (bx + size * length * math.cos(b), by + size * length * math.sin(b))],
                       fill=color, width=max(1, w - 1))
        d.ellipse([ex - w, ey - w, ex + w, ey + w], fill=color)
    hexagon = [(cx + size * 0.16 * math.cos(rot + k * math.pi / 3), cy + size * 0.16 * math.sin(rot + k * math.pi / 3))
               for k in range(6)]
    d.polygon(hexagon, fill=color)


def nanten(d, cx, cy, s, rng):
    """A sprig of nanten: a few pointed leaves and a cluster of berries."""
    for k in range(5):
        a = -math.pi / 2 + (k - 2) * 0.55 + rng.uniform(-0.1, 0.1)
        length = s * rng.uniform(0.9, 1.3)
        tip = (cx + length * math.cos(a), cy + length * math.sin(a))
        mid = (cx + length * 0.5 * math.cos(a), cy + length * 0.5 * math.sin(a))
        n = (-math.sin(a) * s * 0.16, math.cos(a) * s * 0.16)
        d.polygon([(cx, cy), (mid[0] + n[0], mid[1] + n[1]), tip, (mid[0] - n[0], mid[1] - n[1])], fill=LEAF)
    for _ in range(9):
        a = rng.uniform(0, 2 * math.pi)
        dist = rng.uniform(0, s * 0.42)
        bx, by = cx + dist * math.cos(a), cy + s * 0.25 + dist * math.sin(a)
        br = s * rng.uniform(0.09, 0.13)
        d.ellipse([bx - br, by - br, bx + br, by + br], fill=BERRY)
        d.ellipse([bx - br * 0.35, by - br * 0.55, bx - br * 0.05, by - br * 0.25], fill=(236, 140, 140))


def main():
    rng = random.Random(1225)
    w, h = W * SS, H * SS
    img = Image.new('RGB', (w, h))
    px = img.load()
    # Winter night: indigo, a little lighter towards the top.
    for y in range(h):
        t = y / h
        row = (int(16 + 10 * (1 - t)), int(26 + 12 * (1 - t)), int(46 + 18 * (1 - t)))
        for x in range(w):
            px[x, y] = row
    d = ImageDraw.Draw(img, 'RGBA')

    # Back layer: large, faint snow rings, partly off the edges.
    for _ in range(14):
        r = rng.uniform(150, 330) * SS
        snow_ring(d, rng.uniform(-0.1, 1.1) * w, rng.uniform(-0.1, 1.1) * h, r, SLATE + (70,), rng.random() < 0.5,
                  rng.uniform(0, math.pi))
    img = img.filter(ImageFilter.GaussianBlur(3 * SS))
    d = ImageDraw.Draw(img, 'RGBA')

    # Middle layer: snow rings, some holding a snowflake.
    placed = []
    for _ in range(26):
        for _attempt in range(40):
            r = rng.uniform(55, 150) * SS
            cx, cy = rng.uniform(0, w), rng.uniform(0, h)
            if all(math.hypot(cx - x, cy - y) > (r + rr) * 0.9 for x, y, rr in placed):
                break
        placed.append((cx, cy, r))
        rot = rng.uniform(0, math.pi)
        if rng.random() < 0.45:
            snow_ring(d, cx, cy, r, FROST + (150,), True, rot)
            snowflake(d, cx, cy, r * 0.62, (24, 36, 60, 220), rot)
        else:
            snow_ring(d, cx, cy, r, ICE + (170,), False, rot)
            if rng.random() < 0.6:
                snowflake(d, cx, cy, r * 0.55, ICE + (150,), rot)

    # Loose snowflakes between them.
    for _ in range(40):
        snowflake(d, rng.uniform(0, w), rng.uniform(0, h), rng.uniform(14, 40) * SS,
                  ICE + (rng.randint(90, 180),), rng.uniform(0, math.pi))

    # Nanten in three corners of the pattern.
    for cx, cy in ((0.16, 0.78), (0.83, 0.22), (0.62, 0.86)):
        nanten(d, cx * w, cy * h, 70 * SS, rng)

    # Falling snow.
    for _ in range(900):
        x, y = rng.uniform(0, w), rng.uniform(0, h)
        r = rng.uniform(1.0, 3.6) * SS
        d.ellipse([x - r, y - r, x + r, y + r], fill=(235, 242, 250, rng.randint(70, 200)))

    img = img.resize((W, H), Image.LANCZOS)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    img.save(OUT)
    print('wrote', OUT)


if __name__ == '__main__':
    main()
