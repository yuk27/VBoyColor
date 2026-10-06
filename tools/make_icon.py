"""Makes VBoy Color's app icon from the logo (assets/logo/vboycolor-logo.png,
Juan's artwork): the logo on a white rounded tile, in every size the
platforms use. Pillow; run from the repo root after changing the logo:

    python tools/make_icon.py

assets/icon/vboycolor-1024.png      the icon at full size
platform/desktop/vboycolor.ico      Windows exe + window icon (16-256 px)
assets/runtime/icon.png             window icon on Linux (64 px)
android/app/res/mipmap-*/ic_launcher.png   Quest app icon

The menu header uses assets/runtime/logo/vboycolor_header.png: the logo's
two words side by side (see header()).
"""
import os
from PIL import Image, ImageDraw

LOGO = "assets/logo/vboycolor-logo.png"
S = 1024


def content(img):
    """The logo cropped to what's drawn (alpha > 0)."""
    return img.crop(img.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox())


def icon():
    logo = content(Image.open(LOGO).convert("RGBA"))
    tile = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(tile).rounded_rectangle([0, 0, S - 1, S - 1], radius=200, fill=(255, 255, 255, 255))
    inner = int(S * 0.8)
    scale = min(inner / logo.width, inner / logo.height)
    logo = logo.resize((round(logo.width * scale), round(logo.height * scale)), Image.LANCZOS)
    tile.alpha_composite(logo, ((S - logo.width) // 2, (S - logo.height) // 2))
    return tile


def header():
    """"VBOY" and "COLOR" side by side, 280 px tall - the two rows of the
    logo, split where nothing is drawn between them."""
    img = Image.open(LOGO).convert("RGBA")
    alpha = img.getchannel("A")
    rows = [any(alpha.getpixel((x, y)) > 8 for x in range(0, img.width, 2)) for y in range(img.height)]
    words, start = [], None
    for y, filled in enumerate(rows + [False]):
        if filled and start is None:
            start = y
        elif not filled and start is not None:
            words.append(content(img.crop((0, start, img.width, y))))
            start = None
    vboy, color = words[0], words[-1]
    h = 280
    v = vboy.resize((round(vboy.width * h / vboy.height), h), Image.LANCZOS)
    ch = round(h * 0.8)
    c = color.resize((round(color.width * ch / color.height), ch), Image.LANCZOS)
    gap = round(h * 0.22)
    out = Image.new("RGBA", (v.width + gap + c.width, h), (0, 0, 0, 0))
    out.alpha_composite(v, (0, 0))
    out.alpha_composite(c, (v.width + gap, (h - ch) // 2))
    return out


def main():
    img = icon()
    os.makedirs("assets/icon", exist_ok=True)
    img.save("assets/icon/vboycolor-1024.png")
    img.save("platform/desktop/vboycolor.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    img.resize((64, 64), Image.LANCZOS).save("assets/runtime/icon.png")
    for name, size in {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}.items():
        os.makedirs("android/app/res/mipmap-" + name, exist_ok=True)
        img.resize((size, size), Image.LANCZOS).save("android/app/res/mipmap-%s/ic_launcher.png" % name)
    os.makedirs("assets/runtime/logo", exist_ok=True)
    header().save("assets/runtime/logo/vboycolor_header.png", optimize=True)


if __name__ == "__main__":
    main()
