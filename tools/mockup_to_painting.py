"""Turn a loose color mockup of a screen into a color-pack painting.

A painting for a color pack (see "Color packs" in README.md) has to line up pixel
for pixel with an F10 capture. A mockup usually doesn't: it may be a different
size, slightly shifted, or partly redrawn. This tool transfers the mockup's
colors onto the capture's exact pixels:

1. Global fit: scale and offset from the lit-content bounding boxes (the mockup
   should show the whole screen).
2. Local fit: every 8x8 block searches a small shift (+-4 VB pixels) that best
   correlates the mockup's brightness with the capture's, and the shift field is
   median-smoothed so stray blocks follow their neighbors.
3. Blocks that still match poorly (redrawn areas) are colored by brightness rank
   instead: each Virtual Boy shade takes the local mockup color of the same
   brightness, so the area keeps the mockup's hues on the game's own shapes.
4. The colors are snapped to a small palette (k-means) to remove blur in-betweens.

The output is a normal 3x painting: touch it up in any editor, then put it next
to the capture's .tiles file in roms/colorpacks/<rom name>/ (same base name).

    python tools/mockup_to_painting.py mockup.png "Game 001.tiles" "Game 001.png" "Game 001 painted.png"

Requires numpy and Pillow.
"""
import argparse

import numpy as np
from PIL import Image

W, H = 384, 224


def load_gray(path):
    im = np.array(Image.open(path).convert("L")).astype(float)
    s = im.shape[1] // W
    return im[s // 2::s, s // 2::s][:H, :W]


def load_tiles_valid(path):
    data = open(path, "rb").read()
    if data[:8] != b"VBGOTIL1":
        raise SystemExit(f"{path}: not a VirtualBoyGo .tiles file")
    t = np.frombuffer(data, np.uint64, W * H, 16).reshape(H, W)
    return (t >> np.uint64(63)) == 1


def bilinear(img, u, v):
    u = np.clip(u, 0, img.shape[1] - 1.001)
    v = np.clip(v, 0, img.shape[0] - 1.001)
    x0 = u.astype(int)
    y0 = v.astype(int)
    fx = u - x0
    fy = v - y0
    if img.ndim == 3:
        fx = fx[..., None]
        fy = fy[..., None]
    a = img[y0, x0]
    b = img[y0, x0 + 1]
    c = img[y0 + 1, x0]
    d = img[y0 + 1, x0 + 1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def ncc(a, b):
    a = a - a.mean()
    b = b - b.mean()
    den = np.sqrt((a * a).sum() * (b * b).sum())
    return (a * b).sum() / den if den > 1e-6 else -1


def median3(a):
    """3x3 median with edge padding."""
    p = np.pad(a, 1, mode="edge")
    h, w = a.shape
    return np.median(np.stack([p[y:y + h, x:x + w] for y in range(3) for x in range(3)]), 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("mockup", help="color mockup of the whole screen (any size)")
    ap.add_argument("tiles", help="the capture's .tiles file")
    ap.add_argument("capture", help="the capture's PNG (any palette)")
    ap.add_argument("out", help="painting to write (3x PNG)")
    ap.add_argument("--colors", type=int, default=32, help="palette size for the final snap (default 32)")
    args = ap.parse_args()

    mock = np.array(Image.open(args.mockup).convert("RGB")).astype(float)
    L = mock.mean(-1)
    ref = load_gray(args.capture)
    lit = load_tiles_valid(args.tiles) & (ref > 25)

    # 1. global fit from content bounds
    my, mx = np.nonzero(L > 40)
    ry, rx = np.nonzero(lit)
    s = (mx.max() - mx.min() + 1) / (rx.max() - rx.min() + 1)
    ox = mx.min() - rx.min() * s
    oy = my.min() - ry.min() * s
    ys, xs = np.mgrid[0:H, 0:W]
    U = ox + (xs + 0.5) * s
    V = oy + (ys + 0.5) * s

    # 2. local shift per 8x8 block, scored by normalized cross-correlation
    R, step = 4, 0.5
    BH, BW = H // 8, W // 8
    shifts = np.zeros((BH, BW, 2))
    scores = np.zeros((BH, BW))
    base = np.zeros((BH, BW))
    for by in range(BH):
        for bx in range(BW):
            sl = (slice(max(0, by * 8 - 4), by * 8 + 12), slice(max(0, bx * 8 - 4), bx * 8 + 12))
            r = ref[sl]
            if lit[sl].sum() < 6 or r.std() < 1:
                continue
            best = (-2, 0, 0)
            for dy in np.arange(-R, R + 0.01, step):
                for dx in np.arange(-R, R + 0.01, step):
                    c = ncc(r, bilinear(L, U[sl] + dx * s, V[sl] + dy * s))
                    if c > best[0]:
                        best = (c, dx, dy)
            shifts[by, bx] = best[1:]
            scores[by, bx] = best[0]
            base[by, bx] = ncc(r, bilinear(L, U[sl], V[sl]))
    sm = np.stack([median3(shifts[..., i]) for i in (0, 1)], -1)
    use = np.where((scores > 0.35)[..., None], shifts, sm)
    use = np.stack([median3(use[..., i]) for i in (0, 1)], -1)
    dX = np.kron(use[..., 0], np.ones((8, 8)))
    dY = np.kron(use[..., 1], np.ones((8, 8)))
    acc = [bilinear(mock, U + (dX + fx) * s, V + (dY + fy) * s)
           for fy in (-0.2, 0, 0.2) for fx in (-0.2, 0, 0.2)]
    col = np.median(np.stack(acc), 0)

    # 3. brightness-rank fallback where the mockup doesn't match pixel by pixel,
    #    or where a lit shade landed on mockup black
    shade = np.zeros(ref.shape, int)
    for i, lv in enumerate(sorted(np.unique(np.round(ref[lit])))):
        shade[np.round(ref) == lv] = min(i + 1, 3)
    lum = col.mean(-1)
    score_px = np.kron(scores, np.ones((8, 8)))
    fallback = np.zeros_like(col)
    for by in range(BH):
        for bx in range(BW):
            y0, x0 = by * 8, bx * 8
            win = (slice(max(0, y0 - 8), y0 + 16), slice(max(0, x0 - 8), x0 + 16))
            m = bilinear(mock, U[win] + dX[win] * s, V[win] + dY[win] * s).reshape(-1, 3)
            m = m[m.mean(-1) > 45]
            if len(m) < 8:
                continue
            thirds = np.array_split(m[np.argsort(m.mean(-1))], 3)   # dark / mid / light
            reps = [np.median(q, 0) for q in thirds]
            blk = (slice(y0, y0 + 8), slice(x0, x0 + 8))
            for sh in (1, 2, 3):
                fallback[blk][shade[blk] == sh] = reps[sh - 1]
    bad = lit & ((score_px < 0.6) | ((shade >= 2) & (lum < 50)))
    col[bad] = np.where(fallback[bad].sum(-1, keepdims=True) > 0, fallback[bad], col[bad])
    valid = scores[base != 0]
    print(f"scale {s:.3f}; block correlation {np.mean(base[base != 0]):.2f} -> {np.mean(valid):.2f} "
          f"after local shifts; blocks matching well: {np.mean(valid > 0.6):.0%}; "
          f"pixels colored by brightness rank: {bad.sum() / lit.sum():.0%}")

    # 4. palette snap
    rng = np.random.default_rng(1)
    pts = col[lit]
    k = min(args.colors, len(pts))
    c = pts[rng.choice(len(pts), k, replace=False)]
    for _ in range(30):
        lab = ((pts[:, None] - c[None]) ** 2).sum(-1).argmin(1)
        for i in range(k):
            if (lab == i).any():
                c[i] = pts[lab == i].mean(0)

    cap = np.array(Image.open(args.capture).convert("RGB"))
    cs = cap.shape[1] // W
    out = cap[cs // 2::cs, cs // 2::cs][:H, :W].astype(float)   # unlit pixels keep the capture
    out[lit] = c[lab]
    out = out.clip(0, 255).astype(np.uint8)
    Image.fromarray(np.repeat(np.repeat(out, 3, 0), 3, 1)).save(args.out)
    print("wrote", args.out)


if __name__ == "__main__":
    main()
