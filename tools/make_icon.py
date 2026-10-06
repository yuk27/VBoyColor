"""Draws VBoy Color's app icon (an original design: two lenses - the
original's red and VBoy Color's colors - over the title's color stripe) and
writes every size the platforms use. Pillow; run from the repo root:

    python tools/make_icon.py

assets/icon/vboycolor-1024.png      the source image
platform/desktop/vboycolor.ico      Windows exe + window icon (16-256 px)
assets/runtime/icon.png             window icon on Linux (64 px)
android/app/res/mipmap-*/ic_launcher.png   Quest app icon
"""
import os
from PIL import Image, ImageDraw

S = 1024
# The title's colors (the menu header's COLOR letters - AutoColors' ramps,
# lifted a little toward white).
COLORS = [(98, 113, 190), (69, 162, 166), (127, 181, 88), (213, 147, 83), (239, 108, 76)]
BG = (18, 19, 27)
RED = (226, 30, 24)


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def gradient(t):
    t = min(max(t, 0.0), 1.0) * (len(COLORS) - 1)
    i = min(int(t), len(COLORS) - 2)
    return lerp(COLORS[i], COLORS[i + 1], t - i)


def draw():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, S - 1, S - 1], radius=200, fill=BG)
    # Two lenses side by side, like a stereo pair: left red, right in color.
    lw, lh, gap, top = 370, 330, 44, 260
    left_x = (S - 2 * lw - gap) // 2
    right_x = left_x + lw + gap
    for x0, painter in ((left_x, "red"), (right_x, "color")):
        mask = Image.new("L", (S, S), 0)
        ImageDraw.Draw(mask).rounded_rectangle([x0, top, x0 + lw, top + lh], radius=130, fill=255)
        layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        ld = ImageDraw.Draw(layer)
        for y in range(top, top + lh + 1):
            if painter == "red":
                # scanlines, a nod to the hardware's line-by-line display
                c = (150, 18, 14) if (y - top) % 36 >= 26 else RED
                ld.line([(x0, y), (x0 + lw, y)], fill=c + (255,))
            else:
                for x in range(x0, x0 + lw + 1):
                    layer.putpixel((x, y), gradient((x - x0) / lw * 0.7 + (y - top) / lh * 0.3) + (255,))
        img.paste(layer, (0, 0), mask)
    # The stripe in the title's colors.
    seg = (2 * lw + gap) / 5
    for i, c in enumerate(COLORS):
        x0 = left_x + seg * i
        d.rectangle([x0, top + lh + 90, x0 + seg + 1, top + lh + 122], fill=c)
    return img


def main():
    img = draw()
    os.makedirs("assets/icon", exist_ok=True)
    img.save("assets/icon/vboycolor-1024.png")
    img.save("platform/desktop/vboycolor.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    img.resize((64, 64), Image.LANCZOS).save("assets/runtime/icon.png")
    for name, size in {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}.items():
        os.makedirs("android/app/res/mipmap-" + name, exist_ok=True)
        img.resize((size, size), Image.LANCZOS).save("android/app/res/mipmap-%s/ic_launcher.png" % name)


if __name__ == "__main__":
    main()
