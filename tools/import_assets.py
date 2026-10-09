"""Brings painted art into gfx/ at the sizes the 3DS uses. Drop PNG or JPG
files into assets/incoming/ with these names (any size; they are cropped to
the right shape from the centre) and run from the repository root:

    python tools/import_assets.py [incoming_dir]

    svc_gfn, svc_xbox, svc_steam    2:1 banners  -> 512x256 (cards and backdrops)
    cover_bigpicture, cover_desktop 3:4 covers   -> 96x128, titled here
    no_cover                        3:4          -> 96x128
    hub_backdrop                    5:3          -> 400x240 (behind the hub)
    icon_gfn, icon_xbox, icon_steam white on black -> 64x64 white with alpha

Anything missing is left as it is in gfx/. Needs Pillow.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageFont

IN = sys.argv[1] if len(sys.argv) > 1 else os.path.join("assets", "incoming")
OUT = "gfx"
FONT_BOLD = r"C:\Windows\Fonts\segoeuib.ttf"
FONT_LIGHT = r"C:\Windows\Fonts\segoeuisl.ttf"
WASHI = (238, 234, 226)


def find(name):
    for ext in (".png", ".jpg", ".jpeg", ".webp"):
        path = os.path.join(IN, name + ext)
        if os.path.exists(path):
            return path
    return None


def crop_to(im, aspect):
    """The largest centred crop of the given width/height ratio."""
    w, h = im.size
    if w / h > aspect:
        nw = int(h * aspect)
        return im.crop(((w - nw) // 2, 0, (w - nw) // 2 + nw, h))
    nh = int(w / aspect)
    return im.crop((0, (h - nh) // 2, w, (h - nh) // 2 + nh))


def shade(im, boxes):
    """Darkens soft regions (x0, y0, x1, y1 as fractions, strength) so text
    and the seal stay readable over any painting."""
    w, h = im.size
    m = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(m)
    for x0, y0, x1, y1, strength in boxes:
        d.rectangle((x0 * w, y0 * h, x1 * w, y1 * h), fill=int(255 * strength))
    m = m.filter(ImageFilter.GaussianBlur(min(w, h) * 0.12))
    black = Image.new("RGB", (w, h), (0, 0, 0))
    return Image.composite(black, im, m)


def banner(name):
    path = find(name)
    if not path:
        return
    im = crop_to(Image.open(path).convert("RGB"), 2.0)
    # Night scenes read better slightly darker and calmer on the 3DS LCD.
    im = ImageEnhance.Brightness(im).enhance(0.92)
    im = shade(im, [(0.0, 0.62, 0.65, 1.0, 0.55), (0.0, 0.0, 0.18, 0.28, 0.35)])
    im.resize((512, 256), Image.LANCZOS).save(os.path.join(OUT, name + ".png"))
    print("imported", name)


def cover(name, title):
    path = find(name)
    if not path:
        return
    im = crop_to(Image.open(path).convert("RGB"), 0.75).resize((384, 512), Image.LANCZOS)
    im = shade(im, [(0.0, 0.62, 1.0, 1.0, 0.75)])
    if title:
        d = ImageDraw.Draw(im)
        font = ImageFont.truetype(FONT_BOLD, 48)
        small = ImageFont.truetype(FONT_LIGHT, 32)
        tw = d.textlength(title, font=font)
        d.text(((384 - tw) / 2, 352), title, font=font, fill=WASHI)
        tw = d.textlength("STEAM LINK", font=small)
        d.text(((384 - tw) / 2, 420), "STEAM LINK", font=small, fill=(170, 168, 160))
    im.resize((96, 128), Image.LANCZOS).save(os.path.join(OUT, name + ".png"))
    print("imported", name)


def backdrop(name):
    path = find(name)
    if not path:
        return
    im = crop_to(Image.open(path).convert("RGB"), 400 / 240)
    im.resize((400, 240), Image.LANCZOS).save(os.path.join(OUT, name + ".png"))
    print("imported", name)


def icon(name):
    """A white emblem on black: brightness becomes alpha, the shape is
    trimmed to its bounds with a margin and centred on a 64x64 square."""
    path = find(name)
    if not path:
        return
    lum = Image.open(path).convert("L")
    # Lift the near-black background to fully clear, keep soft edges.
    lum = lum.point(lambda v: 0 if v < 40 else min(255, int((v - 40) * 255 / 175)))
    box = lum.getbbox()
    if not box:
        print("skipped", name, "(empty)")
        return
    lum = lum.crop(box)
    side = int(max(lum.size) * 1.12)
    square = Image.new("L", (side, side), 0)
    square.paste(lum, ((side - lum.size[0]) // 2, (side - lum.size[1]) // 2))
    alpha = square.resize((64, 64), Image.LANCZOS)
    out = Image.new("RGBA", (64, 64), (255, 255, 255, 0))
    out.putalpha(alpha)
    out.save(os.path.join(OUT, name + ".png"))
    print("imported", name)


if __name__ == "__main__":
    if not os.path.isdir(IN):
        sys.exit("no %s folder" % IN)
    for name in ("svc_gfn", "svc_xbox", "svc_steam"):
        banner(name)
    cover("cover_bigpicture", "BIG PICTURE")
    cover("cover_desktop", "DESKTOP")
    cover("no_cover", None)
    backdrop("hub_backdrop")
    for name in ("icon_gfn", "icon_xbox", "icon_steam"):
        icon(name)
