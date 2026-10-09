"""Draws the service art for the game hub (gfx/svc_*.png, gfx/icon_*.png,
gfx/cover_*.png): original ink-wash scenes in Kasumi's Japanese style, each
touched with its service's colour, not their logos. Run from the repository
root:

    python tools/gen_service_art.py [out_dir]

Needs Pillow and numpy. Everything is drawn at 2x (or 4x) and scaled down.

The scenes and covers in gfx/ are now paintings brought in with
tools/import_assets.py; by default this only draws the emblems, and
--scenes draws the procedural stand-ins as well.

    GeForce NOW  雲  mountains in mist under the moon, bands of suyari-gasumi
    Xbox         竹  a bamboo grove at night
    Steam Link   湯  a hot spring by a house, steam rising, a stone lantern lit
"""
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = "gfx"
W, H = 1024, 512
FONT_BOLD = r"C:\Windows\Fonts\segoeuib.ttf"
FONT_LIGHT = r"C:\Windows\Fonts\segoeuisl.ttf"
WASHI = (238, 234, 226)


def mask(size=(W, H)):
    im = Image.new("L", size, 0)
    return im, ImageDraw.Draw(im)


def arr(im):
    return np.asarray(im, np.float32) / 255.0


def to_im(a):
    return Image.fromarray((np.clip(a, 0, 1) * 255).astype(np.uint8))


def blur(im, r):
    return im.filter(ImageFilter.GaussianBlur(r))


def col(c):
    return np.array(c, np.float32) / 255.0


def add(base, m, color, strength=1.0):
    a = m if isinstance(m, np.ndarray) else arr(m)
    base += a[..., None] * col(color)[None, None, :] * strength


def over(base, m, color, alpha=1.0):
    a = (m if isinstance(m, np.ndarray) else arr(m))[..., None] * alpha
    base[:] = base * (1 - a) + col(color)[None, None, :] * a


def vgrad(top, bottom, h=H, w=W):
    t = np.linspace(0, 1, h, dtype=np.float32)[:, None, None]
    return np.broadcast_to(col(top) * (1 - t) + col(bottom) * t, (h, w, 3)).copy()


def radial(cx, cy, rx, ry=None, h=H, w=W):
    ry = ry or rx
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    return np.clip(1 - np.sqrt(((xx - cx) / rx) ** 2 + ((yy - cy) / ry) ** 2), 0, 1)


def noise2(cells_x, cells_y, seed, h=H, w=W):
    rng = np.random.default_rng(seed)
    small = Image.fromarray((rng.random((cells_y, cells_x)) * 255).astype(np.uint8))
    return arr(small.resize((w, h), Image.BICUBIC))


def ridge(seed, base, amp, w=W, octaves=5, peak=None):
    """A mountain line: heights in pixels from the top for each column."""
    rng = np.random.default_rng(seed)
    x = np.linspace(0, 1, w)
    y = np.zeros(w)
    for o in range(octaves):
        n = 4 * 2 ** o
        pts = rng.random(n + 3)
        xs = np.linspace(0, 1, n + 3)
        y += np.interp(x, xs, pts) * amp / (1.8 ** o)
    if peak:
        px, height, width = peak
        y += height * np.exp(-((x - px) / width) ** 2) * (1 - 0.15 * np.abs(x - px) / width)
    return base - y


def fill_below(line, h=H, w=W):
    yy = np.mgrid[0:h, 0:w][0].astype(np.float32)
    return np.clip(yy - line[None, :] + 1, 0, 1)


def washi_grain(base, seed, amount=0.035):
    n = noise2(256, 128, seed) * 0.6 + noise2(512, 256, seed + 1) * 0.4
    base *= 1 - amount + amount * 2 * n[..., None]


def moon(base, cx, cy, r, tint, strength=1.0):
    add(base, radial(cx, cy, r * 3.2) ** 2, tint, 0.35 * strength)
    disc = np.clip((1 - np.sqrt(((np.mgrid[0:H, 0:W][1] - cx) / r) ** 2 + ((np.mgrid[0:H, 0:W][0] - cy) / r) ** 2)) * r / 2,
                   0, 1)
    over(base, disc, WASHI, 0.82 * strength)


def mist_band(base, y, thickness, color, alpha, seed):
    """A soft horizontal band of mist, broken up by noise."""
    yy = np.mgrid[0:H, 0:W][0].astype(np.float32)
    band = np.exp(-((yy - y) / thickness) ** 2)
    n = noise2(10, 4, seed) * 0.7 + noise2(30, 8, seed + 7) * 0.3
    over(base, band * np.clip(n * 1.6 - 0.2, 0, 1), color, alpha)


def suyari(base, rects, color, alpha):
    """Suyari-gasumi: long bands of mist, as on painted screens: soft, with
    ends that fade and a brushed texture."""
    xx = np.mgrid[0:H, 0:W][1].astype(np.float32)
    texture = np.clip(noise2(80, 6, 90) * 0.5 + 0.6, 0, 1)
    for i, (x0, y0, x1, y1) in enumerate(rects):
        m, d = mask()
        d.rounded_rectangle((x0, y0, x1, y1), radius=(y1 - y0) / 2, fill=255)
        span = max(x1 - x0, 1.0)
        ends = np.clip(np.minimum((xx - x0) / (span * 0.25), (x1 - xx) / (span * 0.25)), 0, 1)
        over(base, arr(blur(m, 5)) * ends * texture, color, alpha)


def vignette(base, amount=0.55):
    v = radial(W / 2, H / 2, W * 0.78, H * 0.9)
    base *= (1 - amount) + amount * np.clip(v * 1.7, 0, 1)[..., None]


def save(base, name, size=(512, 256)):
    im = Image.fromarray((np.clip(base, 0, 1) * 255).astype(np.uint8), "RGB")
    im.resize(size, Image.LANCZOS).save(os.path.join(OUT, name))
    print("wrote", name)


# ---- GeForce NOW 雲: layered mountains, a moon, golden-green mist bands ----

def art_gfn():
    green = (141, 179, 106)   # matcha
    gold = (216, 196, 120)
    base = vgrad((14, 16, 12), (4, 5, 4))
    add(base, radial(W * 0.62, H * 0.38, 600, 300), green, 0.10)
    moon(base, W * 0.70, H * 0.26, 46, (230, 236, 200))
    layers = [
        (ridge(3, H * 0.62, 110, peak=(0.36, 190, 0.16)), (70, 78, 66)),
        (ridge(5, H * 0.72, 90), (42, 48, 40)),
        (ridge(8, H * 0.83, 70), (20, 24, 19)),
        (ridge(13, H * 0.95, 55), (6, 7, 6)),
    ]
    for i, (line, shade) in enumerate(layers):
        m = fill_below(line)
        # Each ridge fades into mist at its foot (ink wash).
        yy = np.mgrid[0:H, 0:W][0].astype(np.float32)
        depth = np.clip((yy - line[None, :]) / 140.0, 0, 1)
        fade = 1 - 0.55 * depth if i < 3 else np.ones_like(depth)
        over(base, m * fade, shade, 1.0)
        if i < 3:
            mist_band(base, line.mean() + 60, 34, (120, 130, 112), 0.35, 20 + i)
    # The snow on the high peak: a lighter wash near its top.
    top = layers[0][0]
    yy = np.mgrid[0:H, 0:W][0].astype(np.float32)
    snow = fill_below(top) * np.clip(1 - (yy - top[None, :]) / 26.0, 0, 1) * (top[None, :] < H * 0.42)
    over(base, arr(blur(to_im(snow), 2)), WASHI, 0.55)
    suyari(base, [(-120, H * 0.46, W * 0.46, H * 0.46 + 20), (W * 0.50, H * 0.58, W + 120, H * 0.58 + 16),
                  (W * 0.12, H * 0.82, W * 0.70, H * 0.82 + 14)], gold, 0.30)
    add(base, radial(W * 0.5, H * 0.6, 520, 120), green, 0.08)
    washi_grain(base, 1)
    vignette(base, 0.5)
    save(base, "svc_gfn.png")


# ---- Xbox 竹: a bamboo grove at night ----

def bamboo_stalk(d, x, lean, width, top, bottom, value, node_gap, rng):
    pts = []
    steps = 24
    for i in range(steps + 1):
        t = i / steps
        y = bottom + (top - bottom) * t
        pts.append((x + lean * t, y))
    for i in range(steps):
        (x0, y0), (x1, y1) = pts[i], pts[i + 1]
        d.polygon(((x0 - width / 2, y0), (x0 + width / 2, y0), (x1 + width / 2, y1), (x1 - width / 2, y1)), fill=value)
    y = bottom - rng.uniform(10, node_gap)
    nodes = []
    while y > top:
        t = (y - bottom) / (top - bottom)
        nodes.append((x + lean * t, y))
        y -= node_gap * rng.uniform(0.85, 1.15)
    return nodes


def leaf(d, x, y, angle, length, width, value):
    a = math.radians(angle)
    ux, uy = math.cos(a), math.sin(a)
    px, py = -uy, ux
    tip = (x + ux * length, y + uy * length)
    mid = (x + ux * length * 0.35, y + uy * length * 0.35)
    d.polygon(((x, y), (mid[0] + px * width, mid[1] + py * width), tip, (mid[0] - px * width, mid[1] - py * width)),
              fill=value)


def art_xbox():
    rng = np.random.default_rng(21)
    green = (70, 160, 80)
    base = vgrad((8, 16, 10), (3, 6, 4))
    add(base, radial(W * 0.66, H * 0.30, 520, 320), green, 0.16)
    moon(base, W * 0.66, H * 0.27, 52, (220, 240, 214), 0.9)
    # Three depths of bamboo: far (pale, blurred), middle, near (dark, sharp).
    for depth, (count, value, blur_r, wmin, wmax, shade) in enumerate(
            ((18, 255, 6, 10, 16, (52, 92, 58)), (12, 255, 2, 16, 24, (24, 50, 28)), (6, 255, 0, 26, 38, (6, 12, 7)))):
        m, d = mask()
        leaves, ld = mask()
        for _ in range(count):
            x = rng.uniform(-40, W + 40)
            if depth == 2 and W * 0.5 < x < W * 0.8:
                x = rng.choice([rng.uniform(-40, W * 0.45), rng.uniform(W * 0.82, W + 40)])
            width = rng.uniform(wmin, wmax)
            nodes = bamboo_stalk(d, x, rng.uniform(-30, 30), width, -40, H + 20, value, rng.uniform(70, 120), rng)
            for nx, ny in nodes:
                d.line((nx - width / 2 - 2, ny, nx + width / 2 + 2, ny), fill=0, width=3)
                if rng.random() < 0.45:
                    side = 1 if rng.random() < 0.5 else -1
                    for k in range(int(rng.integers(2, 5))):
                        ang = (20 + rng.uniform(-15, 35)) if side > 0 else (160 - rng.uniform(-15, 35))
                        leaf(ld, nx + side * width / 2, ny + k * 6, ang, rng.uniform(50, 95), rng.uniform(5, 9), 255)
        if blur_r:
            m, leaves = blur(m, blur_r), blur(leaves, blur_r)
        over(base, m, shade, 0.95)
        over(base, leaves, shade, 0.9)
        if depth < 2:
            mist_band(base, H * (0.72 + 0.1 * depth), 60, (90, 130, 96), 0.35, 40 + depth)
    # Moonlight along the near stalks' edges is too busy; a low mist instead.
    mist_band(base, H * 0.95, 50, (60, 96, 66), 0.4, 50)
    washi_grain(base, 2)
    vignette(base, 0.5)
    save(base, "svc_xbox.png")


# ---- Steam Link 湯: a hot spring by a house, steam rising, a lantern ----

def art_steam():
    ai = (106, 140, 200)       # indigo
    blue = (102, 168, 220)
    warm = (232, 180, 110)
    base = vgrad((10, 14, 26), (4, 6, 12))
    add(base, radial(W * 0.30, H * 0.22, 560, 300), ai, 0.16)
    moon(base, W * 0.24, H * 0.22, 40, (214, 226, 246), 0.9)
    # Far hills.
    hill = ridge(31, H * 0.50, 80)
    over(base, fill_below(hill), (24, 32, 52), 1.0)
    mist_band(base, H * 0.55, 30, (70, 88, 120), 0.35, 60)
    # The house: a gabled minka roof and a warm window, on the right.
    m, d = mask()
    hx, hy = W * 0.62, H * 0.47
    d.polygon(((hx - 40, hy + 40), (hx + 120, hy - 50), (hx + 290, hy + 40)), fill=255)
    d.rectangle((hx - 10, hy + 38, hx + 260, hy + 130), fill=255)
    over(base, m, (8, 10, 18), 1.0)
    win, d = mask()
    d.rectangle((hx + 40, hy + 64, hx + 120, hy + 108), fill=255)
    d.rectangle((hx + 150, hy + 64, hx + 230, hy + 108), fill=255)
    add(base, blur(win, 14), warm, 0.6)
    over(base, win, (240, 198, 128), 0.85)
    lattice, d = mask()
    for xg in range(int(hx + 40), int(hx + 240), 20):
        d.line((xg, hy + 64, xg, hy + 108), fill=255, width=3)
    over(base, lattice, (40, 30, 20), 0.6)
    # The pool and its rocks, the moon in the water.
    pool, d = mask()
    d.ellipse((W * 0.06, H * 0.70, W * 0.70, H * 0.98), fill=255)
    over(base, pool, (14, 24, 44), 1.0)
    shimmer = noise2(60, 6, 70) * arr(pool)
    add(base, np.clip(shimmer - 0.55, 0, 1) * 1.6, blue, 0.35)
    refl = radial(W * 0.24, H * 0.84, 46, 16) * arr(pool)
    add(base, refl, (200, 214, 240), 0.55)
    rocks, d = mask()
    rng = np.random.default_rng(8)
    for _ in range(26):
        a = rng.uniform(0, math.pi * 2)
        cx = W * 0.38 + math.cos(a) * W * 0.33
        cy = H * 0.84 + math.sin(a) * H * 0.15
        rx, ry = rng.uniform(26, 60), rng.uniform(14, 30)
        d.ellipse((cx - rx, cy - ry, cx + rx, cy + ry), fill=255)
    over(base, blur(rocks, 1), (6, 8, 12), 1.0)
    # A stone lantern (tōrō), lit, at the pool's edge.
    lx, ly = W * 0.80, H * 0.98
    t, d = mask()
    d.rectangle((lx - 34, ly - 20, lx + 34, ly), fill=255)
    d.rectangle((lx - 12, ly - 120, lx + 12, ly - 20), fill=255)
    d.rectangle((lx - 30, ly - 132, lx + 30, ly - 120), fill=255)
    d.rectangle((lx - 24, ly - 186, lx + 24, ly - 132), fill=255)
    d.polygon(((lx - 62, ly - 186), (lx + 62, ly - 186), (lx + 20, ly - 214), (lx - 20, ly - 214)), fill=255)
    d.ellipse((lx - 9, ly - 232, lx + 9, ly - 210), fill=255)
    over(base, t, (10, 12, 16), 1.0)
    glow, d = mask()
    d.rectangle((lx - 14, ly - 176, lx + 14, ly - 142), fill=255)
    add(base, blur(glow, 22), warm, 0.9)
    over(base, glow, (250, 214, 150), 0.9)
    # Steam rising from the water.
    n = noise2(16, 6, 81) * 0.6 + noise2(40, 14, 82) * 0.4
    yy = np.mgrid[0:H, 0:W][0].astype(np.float32)
    rise = np.clip((H * 0.86 - yy) / (H * 0.5), 0, 1) * np.clip((yy - H * 0.20) / (H * 0.2), 0, 1)
    across = radial(W * 0.38, H * 0.65, W * 0.36, H * 0.6)
    steam = np.clip((n - 0.40) * 2.4, 0, 1) * rise * np.clip(across * 1.6, 0, 1)
    over(base, arr(blur(to_im(steam), 6)), (190, 206, 232), 0.42)
    washi_grain(base, 3)
    vignette(base, 0.5)
    save(base, "svc_steam.png")


# ---- Emblems: white on transparent, tinted when drawn ----

def save_icon(m, name, size=64):
    m = m.resize((size, size), Image.LANCZOS)
    im = Image.new("RGBA", (size, size), (255, 255, 255, 0))
    im.putalpha(m)
    im.save(os.path.join(OUT, name))
    print("wrote", name)


def icon_cloud():
    m, d = mask((256, 256))
    for x, y, r in ((80, 140, 52), (128, 110, 64), (180, 136, 50)):
        d.ellipse((x - r, y - r, x + r, y + r), fill=255)
    d.rounded_rectangle((34, 140, 222, 196), radius=28, fill=255)
    d.polygon(((108, 112), (108, 176), (160, 144)), fill=0)
    return m


def icon_pad():
    m, d = mask((256, 256))
    d.rounded_rectangle((26, 70, 230, 160), radius=44, fill=255)
    d.ellipse((22, 104, 104, 214), fill=255)
    d.ellipse((152, 104, 234, 214), fill=255)
    d.rectangle((60, 104, 100, 116), fill=0)
    d.rectangle((74, 90, 86, 130), fill=0)
    for x, y in ((182, 92), (200, 110), (164, 110), (182, 128)):
        d.ellipse((x - 9, y - 9, x + 9, y + 9), fill=0)
    return m


def icon_monitor():
    m, d = mask((256, 256))
    d.rounded_rectangle((22, 36, 234, 176), radius=18, outline=255, width=20)
    d.rectangle((112, 176, 144, 204), fill=255)
    d.rounded_rectangle((70, 200, 186, 222), radius=10, fill=255)
    d.polygon(((108, 76), (108, 138), (160, 107)), fill=255)
    return m


# ---- Covers for Steam Link's own entries (96x128): indigo washi, waves ----

def seigaiha(w, h, r, line_value=255):
    m = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(m)
    for row in range(-1, int(h / (r * 0.5)) + 2):
        y = row * r * 0.5
        offset = (row % 2) * r
        for x0 in range(-2, int(w / (r * 2)) + 3):
            cx = x0 * r * 2 + offset
            for k in (1.0, 0.72, 0.44):
                rr = r * k
                d.ellipse((cx - rr, y - rr, cx + rr, y + rr), outline=line_value, width=3, fill=0 if k == 1.0 else None)
    return m


def cover(name, title, icon):
    w, h = 384, 512
    base = vgrad((24, 34, 62), (8, 12, 24), h, w)
    waves = arr(seigaiha(w, h, 40)) * np.linspace(0.05, 0.6, h, dtype=np.float32)[:, None]
    add(base, waves, (106, 140, 200), 0.35)
    add(base, radial(w / 2, h * 0.36, w * 0.6, h * 0.36, h, w), (106, 140, 200), 0.25)
    m = icon.resize((200, 200), Image.LANCZOS)
    big = Image.new("L", (w, h), 0)
    big.paste(m, ((w - 200) // 2, 80))
    over(base, arr(blur(big, 10)), (20, 28, 50), 0.6)
    over(base, big, WASHI, 0.95)
    m, d = mask((w, h))
    font = ImageFont.truetype(FONT_BOLD, 48)
    small = ImageFont.truetype(FONT_LIGHT, 32)
    tw = d.textlength(title, font=font)
    d.text(((w - tw) / 2, 330), title, font=font, fill=255)
    tw = d.textlength("STEAM LINK", font=small)
    d.text(((w - tw) / 2, 398), "STEAM LINK", font=small, fill=160)
    over(base, m, WASHI, 1.0)
    im = Image.fromarray((np.clip(base, 0, 1) * 255).astype(np.uint8), "RGB")
    im.resize((96, 128), Image.LANCZOS).save(os.path.join(OUT, name))
    print("wrote", name)


if __name__ == "__main__":
    OUT = next((a for a in sys.argv[1:] if not a.startswith("--")), "gfx")
    os.makedirs(OUT, exist_ok=True)
    if "--scenes" in sys.argv:
        art_gfn()
        art_xbox()
        art_steam()
        cover("cover_bigpicture.png", "BIG PICTURE", icon_pad())
        cover("cover_desktop.png", "DESKTOP", icon_monitor())
    save_icon(icon_cloud(), "icon_gfn.png")
    save_icon(icon_pad(), "icon_xbox.png")
    save_icon(icon_monitor(), "icon_steam.png")
