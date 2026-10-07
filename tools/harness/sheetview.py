"""Whole-sheet views of an extrapolate.py output: painted pixels in color, uncolored in grays, grid every 16 px.
    python3 sheetview.py OUT_DIR SHEET_DIR [scale] [x0 y0 x1 y1 sheet [src]]  (src: tint by where colors came from)"""
import sys, glob, os, struct, numpy as np
from PIL import Image, ImageDraw, ImageFont
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vbp import U
out, sdir = sys.argv[1], sys.argv[2]
scale = int(sys.argv[3]) if len(sys.argv) > 3 else 3
reg = list(map(int, sys.argv[4:9])) if len(sys.argv) > 8 else None
font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 11)
autos = sorted(glob.glob(os.path.join(out, "* tas auto *.png")))
todos = sorted(glob.glob(os.path.join(sdir, "*.png")))
for si, (ap, tp) in enumerate(zip(autos, todos)):
    if reg and si != reg[4]:
        continue
    b = open(tp[:-4] + ".tiles", "rb").read(); w, h = struct.unpack_from("<II", b, 8)
    t = np.frombuffer(b, dtype="<u8", count=w * h, offset=16).reshape(h, w)
    valid = (t >> U(63)) == U(1); pix = ((t >> U(41)) & U(3)).astype(int)
    auto = np.asarray(Image.open(ap).convert("RGB"))[1::3, 1::3].copy()
    shown = np.asarray(Image.open(tp).convert("RGB"))[1::3, 1::3]
    st = np.load(os.path.join(out, "state.npz"))
    unc = st["need%d" % si] & valid & (pix > 0)
    if len(sys.argv) > 9 and sys.argv[9] == "src":  # tint by where the color came from
        src = st["src%d" % si]
        for k, c in {1: (255, 255, 255), 2: (60, 200, 255), 3: (255, 160, 0), 4: (0, 255, 0), 5: (0, 160, 0), 6: (255, 0, 255)}.items():
            auto[src == k] = (auto[src == k] * 0.4 + np.array(c) * 0.6).astype(np.uint8)
    img = auto.copy()
    img[~valid] = (14, 14, 18)
    for v, c in {1: (90, 90, 110), 2: (160, 160, 180), 3: (235, 235, 245)}.items():
        img[unc & (pix == v)] = c
    x0, y0, x1, y1 = (reg[:4] if reg else (0, 0, w, h))
    img = img[y0:y1, x0:x1]
    hh, ww = img.shape[:2]
    pad = 30
    big = Image.fromarray(img).resize((ww * scale, hh * scale), Image.NEAREST)
    cv = Image.new("RGB", (ww * scale + pad, hh * scale + pad), (0, 0, 0)); cv.paste(big, (pad, pad))
    d = ImageDraw.Draw(cv)
    step = 16 if scale <= 4 else 8
    for X in range((x0 + step - 1) // step * step, x1 + 1, step):
        px = pad + (X - x0) * scale
        d.line([(px, pad), (px, pad + hh * scale)], fill=(150, 40, 40) if X % 32 == 0 else (50, 50, 120))
        if X % (32 if scale <= 3 else step) == 0:
            d.text((px + 1, 2), str(X), fill=(255, 255, 255), font=font)
    for Y in range((y0 + step - 1) // step * step, y1 + 1, step):
        py = pad + (Y - y0) * scale
        d.line([(pad, py), (pad + ww * scale, py)], fill=(150, 40, 40) if Y % 32 == 0 else (50, 50, 120))
        if Y % (32 if scale <= 3 else step) == 0:
            d.text((1, py + 1), str(Y), fill=(255, 255, 255), font=font)
    name = "sheet_%02d.png" % si if not reg else "sheet_%02d_%d_%d.png" % (si, x0, y0)
    cv.save(os.path.join(out, name))
    print(name, cv.size, "uncolored", int(unc.sum()))
