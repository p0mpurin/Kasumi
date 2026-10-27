#!/usr/bin/env python3
"""Make the Vita's LiveArea art (vita/sce_sys) from Kasumi's own art.

The Vita shows an app's icon, LiveArea background and start gate from
sce_sys/ in its VPK. They must be 8-bit (256-colour) PNGs of exact sizes:
icon0 128x128, bg 840x500, startup 280x158. Run once after changing the art:

    python tools/make_livearea.py
"""
import os

from PIL import Image

ROOT = os.path.join(os.path.dirname(__file__), "..")
OUT = os.path.join(ROOT, "vita", "sce_sys")
BLACK = (0, 0, 0, 255)


def art(name):
    return Image.open(os.path.join(ROOT, "gfx", name)).convert("RGBA")


def cover(image, width, height):
    """Scale to fill width x height, cropping the overflow evenly."""
    scale = max(width / image.width, height / image.height)
    size = (round(image.width * scale), round(image.height * scale))
    image = image.resize(size, Image.LANCZOS)
    left, top = (size[0] - width) // 2, (size[1] - height) // 2
    return image.crop((left, top, left + width, top + height))


def save(image, *path):
    target = os.path.join(OUT, *path)
    os.makedirs(os.path.dirname(target), exist_ok=True)
    flat = Image.new("RGBA", image.size, BLACK)
    flat.alpha_composite(image)
    flat.convert("RGB").quantize(colors=256, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG).save(target)
    print("wrote", os.path.relpath(target, ROOT))


def main():
    # The icon: the seal on black, as on the 3DS HOME Menu.
    icon = Image.new("RGBA", (128, 128), BLACK)
    seal = art("seal.png").resize((112, 112), Image.LANCZOS)
    icon.alpha_composite(seal, (8, 8))
    save(icon, "icon0.png")
    # LiveArea: the hub's painting behind, the hero art on the gate.
    save(cover(art("hub_backdrop.png"), 840, 500), "livearea", "contents", "bg.png")
    save(cover(art("hero.png"), 280, 158), "livearea", "contents", "startup.png")


if __name__ == "__main__":
    main()
