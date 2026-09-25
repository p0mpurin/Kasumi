"""Generate placeholder Kasumi CIA banner and icon until final art replaces them."""

from pathlib import Path
import wave

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parent.parent
RESOURCES = ROOT / "resources"
FONT = Path("C:/Windows/Fonts/arialbd.ttf")


def fit_font(draw: ImageDraw.ImageDraw, text: str, maximum: int, width: int):
    size = maximum
    while size > 8:
        font = ImageFont.truetype(str(FONT), size)
        box = draw.textbbox((0, 0), text, font=font)
        if box[2] - box[0] <= width:
            return font
        size -= 1
    return ImageFont.load_default()


def centered(draw: ImageDraw.ImageDraw, image, y: int, text: str, font, fill):
    box = draw.textbbox((0, 0), text, font=font)
    x = (image.width - (box[2] - box[0])) // 2
    draw.text((x, y), text, font=font, fill=fill)


def make_banner():
    image = Image.new("RGB", (256, 128), "#000000")
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((8, 8, 247, 119), radius=18, outline="#7EBEA5", width=4)
    title = fit_font(draw, "Kasumi", 34, 224)
    subtitle = fit_font(draw, "Cloud gaming for New 3DS", 18, 224)
    centered(draw, image, 29, "Kasumi", title, "#EEEAE2")
    centered(draw, image, 77, "Cloud gaming for New 3DS", subtitle, "#7EBEA5")
    image.save(RESOURCES / "banner.png", optimize=True)


def make_icon():
    image = Image.new("RGB", (48, 48), "#000000")
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((2, 2, 45, 45), radius=9, outline="#7EBEA5", width=3)
    font = fit_font(draw, "K", 22, 38)
    box = draw.textbbox((0, 0), "K", font=font)
    x = (image.width - (box[2] - box[0])) // 2
    y = (image.height - (box[3] - box[1])) // 2 - box[1]
    draw.text((x, y), "K", font=font, fill="#EEEAE2")
    image.save(RESOURCES / "icon.png", optimize=True)


def make_audio():
    with wave.open(str(RESOURCES / "banner.wav"), "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(22050)
        output.writeframes(b"\0\0\0\0" * 11025)


if __name__ == "__main__":
    RESOURCES.mkdir(exist_ok=True)
    # The final banner and icon art is committed; only fill in missing files.
    if not (RESOURCES / "banner.png").exists():
        make_banner()
    if not (RESOURCES / "icon.png").exists():
        make_icon()
    make_audio()
