"""Makes VBoy Color's app icon from the logo (assets/logo/vboycolor-logo.png,
Juan's artwork): the logo on a white rounded tile, in every size the
platforms use. Pillow; run from the repo root after changing the logo:

    python tools/make_icon.py

assets/icon/vboycolor-1024.png      the icon at full size
platform/desktop/vboycolor.ico      Windows exe + window icon (16-256 px)
assets/runtime/icon.png             window icon on Linux (64 px)
android/app/res/mipmap-*/ic_launcher.png   Quest app icon
assets/steam/*.png                  Steam library artwork, for adding the
                                    app to Steam as a non-Steam game (the
                                    Steam Frame): capsule, wide capsule,
                                    hero, logo and icon

The menu header uses assets/runtime/logo/vboycolor_header.png: the logo's
two words side by side (see header()).
"""
import os
from PIL import Image, ImageDraw, ImageFilter

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


# The COLOR letters' colors, left to right (sampled from the logo).
LETTER_COLORS = [(0xCD, 0x25, 0x39), (0x58, 0x51, 0xA6), (0xA4, 0xC6, 0x18), (0xD7, 0xB3, 0x00), (0x00, 0x9B, 0xA6)]


def fit(img, w, h):
    s = min(w / img.width, h / img.height)
    return img.resize((round(img.width * s), round(img.height * s)), Image.LANCZOS)


def steam_tile(w, h, logo_w, logo_h, stripe_h):
    """A library capsule: the icon's look - the logo on white - with the
    COLOR letters' colors along the bottom."""
    img = Image.new("RGBA", (w, h), (255, 255, 255, 255))
    logo = fit(content(Image.open(LOGO).convert("RGBA")), logo_w, logo_h)
    img.alpha_composite(logo, ((w - logo.width) // 2, (h - stripe_h - logo.height) // 2))
    draw = ImageDraw.Draw(img)
    for i, c in enumerate(LETTER_COLORS):
        draw.rectangle([round(w * i / 5), h - stripe_h, round(w * (i + 1) / 5), h], fill=c + (255,))
    return img


def steam_hero(w=3840, h=1240):
    """The library page's banner: the Virtual Boy's black with the letters'
    colors as soft diagonal bands, the left side left dark for the logo
    Steam draws over it."""
    img = Image.new("RGBA", (w, h), (12, 10, 14, 255))
    bands = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(bands)
    bw = w // 9
    for i, c in enumerate(LETTER_COLORS):
        x = int(w * 0.38) + i * bw
        draw.polygon([(x, h), (x + bw, h), (x + bw + h // 2, 0), (x + h // 2, 0)], fill=c + (150,))
    img.alpha_composite(bands.filter(ImageFilter.GaussianBlur(w // 120)))
    return img


def steam_library(icon_img):
    """Steam's library artwork, so the app added as a non-Steam game looks
    like any other game there (sizes as Steam asks for them)."""
    os.makedirs("assets/steam", exist_ok=True)
    steam_tile(600, 900, 500, 600, 60).save("assets/steam/capsule.png", optimize=True)
    steam_tile(920, 430, 760, 300, 34).save("assets/steam/header.png", optimize=True)
    steam_hero().save("assets/steam/hero.png", optimize=True)
    fit(content(Image.open(LOGO).convert("RGBA")), 1280, 720).save("assets/steam/logo.png", optimize=True)
    icon_img.resize((256, 256), Image.LANCZOS).save("assets/steam/icon.png", optimize=True)


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
    steam_library(img)


if __name__ == "__main__":
    main()
