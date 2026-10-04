"""Turn a loose color mockup of a screen into a color-pack painting.

A painting for a color pack (see "Color packs" in README.md) has to line up pixel
for pixel with an F10 capture. A mockup usually doesn't: it may be a different
size, shifted, or partly redrawn. This tool moves the mockup's colors onto the
capture's exact pixels:

1. Global fit: scale and offset from the lit-content bounding boxes (the mockup
   should show the whole screen).
2. Per layer: every layer (world) of the capture gets its own offset into the
   mockup, found by matching brightness - games scroll layers independently
   (parallax), so a mockup made at another moment has each layer shifted
   differently. Layers that match poorly (heavily redrawn or dithered) use the
   offset the well-matching layers agree on.
3. Colors are read at the middle of every pixel and snapped to a small palette
   (k-means) of the mockup's own colors; then every tile pixel takes the color
   it shows most often on its layer, so repeated tiles come out identical (and
   a tile the game reuses on another layer can keep different colors there).

Works best when the capture shows the same moment as the mockup (same scroll
positions and animation frames) - then the offsets are all ~0 and the result
follows the mockup pixel for pixel, except where the mockup was redrawn.

The output is a normal 3x painting: touch it up in any editor, then put it next
to the capture's .tiles file in roms/colorpacks/<rom name>/ (same base name).

    python tools/mockup_to_painting.py mockup.png "Game 001.tiles" "Game 001.png" "Game 001 painted.png"

Options: --colors N (palette size, default 40), --max-shift N (largest layer
offset searched, in VB pixels; default 200 for wide layers, 24 for small ones -
use 2 when capture and mockup show the same moment).

Requires numpy and Pillow.
"""
import argparse

import numpy as np
from PIL import Image

W, H = 384, 224


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("mockup", help="color mockup of the whole screen (any size)")
    ap.add_argument("tiles", help="the capture's .tiles file")
    ap.add_argument("capture", help="the capture's PNG (any palette)")
    ap.add_argument("out", help="painting to write (3x PNG)")
    ap.add_argument("--colors", type=int, default=40, help="palette size (default 40)")
    ap.add_argument("--max-shift", type=int, default=None, help="largest layer offset searched, VB pixels")
    args = ap.parse_args()

    mock = np.array(Image.open(args.mockup).convert("RGB")).astype(np.float32)
    lum = mock.mean(-1)
    mh, mw = lum.shape
    cap = np.array(Image.open(args.capture).convert("RGB"))
    cs = cap.shape[1] // W
    cap = cap[cs // 2::cs, cs // 2::cs][:H, :W]
    ref = cap.astype(np.float32).mean(-1)
    data = open(args.tiles, "rb").read()
    if data[:8] not in (b"VBGOTIL1", b"VBGOTIL2"):  # (2 adds map cells after the records)
        raise SystemExit(f"{args.tiles}: not a VirtualBoyGo .tiles file")
    t = np.frombuffer(data, np.uint64, W * H, 16).reshape(H, W)
    drawn = ((t >> np.uint64(41)) & np.uint64(3)) != 0  # not a fill (transparent pixel of a background tile)
    lit = ((t >> np.uint64(63)) == 1) & drawn & (ref > 25)  # visible tile pixels
    world = ((t >> np.uint64(43)) & np.uint64(31)).astype(int)
    # tile pixel + layer
    key = ((t & np.uint64(0xFFFFFFFF)) << np.uint64(11)) | ((
        ((t >> np.uint64(35)) & np.uint64(7)) * np.uint64(8) + ((t >> np.uint64(32)) & np.uint64(7))) << np.uint64(5)) | \
        ((t >> np.uint64(43)) & np.uint64(31))

    # 1. global fit
    my, mx = np.nonzero(lum > 40)
    ry, rx = np.nonzero(lit)
    s = (mx.max() - mx.min() + 1) / (rx.max() - rx.min() + 1)
    ox = mx.min() - rx.min() * s
    oy = my.min() - ry.min() * s

    def sample(img, xs, ys):
        """Mockup value in the middle of VB pixel (xs, ys): median of the central 2x2 mockup pixels."""
        u = ox + (xs + 0.5) * s
        v = oy + (ys + 0.5) * s
        ok = (u >= 1) & (u < mw - 1) & (v >= 1) & (v < mh - 1)
        ui = np.clip(u.astype(int), 1, mw - 2)
        vi = np.clip(v.astype(int), 1, mh - 2)
        return np.median(np.stack([img[vi + a, ui + b] for a in (-1, 0) for b in (-1, 0)]), 0), ok

    def ncc(a, b):
        a = a - a.mean()
        b = b - b.mean()
        d = np.sqrt((a * a).sum() * (b * b).sum())
        return (a * b).sum() / d if d > 1e-6 else -1

    # 2. per-layer offsets
    offsets = {}
    rng = np.random.default_rng(0)
    for w in np.unique(world[lit]):
        ys, xs = np.nonzero(lit & (world == w))
        r = ref[ys, xs]
        if r.std() < 1:
            offsets[w] = (0.0, 0.0, 0.0)
            continue
        sel = np.arange(len(xs)) if len(xs) < 4000 else rng.choice(len(xs), 4000, replace=False)
        span = args.max_shift if args.max_shift is not None else (200 if xs.max() - xs.min() > 200 else 24)
        best = (-2.0, 0.0, 0.0)
        for dy in range(-min(10, span), min(10, span) + 1):
            for dx in range(-span, span + 1):
                val, ok = sample(lum, xs[sel] + dx, ys[sel] + dy)
                if ok.mean() >= 0.5:
                    c = ncc(r[sel][ok], val[ok])
                    if c > best[0]:
                        best = (c, float(dx), float(dy))
        c0, bx, by = best
        for fy in np.arange(-0.75, 0.76, 0.25):
            for fx in np.arange(-0.75, 0.76, 0.25):
                val, ok = sample(lum, xs + bx + fx, ys + by + fy)
                if ok.mean() >= 0.5:
                    c = ncc(r[ok], val[ok])
                    if c > best[0]:
                        best = (c, bx + fx, by + fy)
        offsets[w] = best
    good = [(dx, dy) for c, dx, dy in offsets.values() if c > 0.8]
    for w, (c, dx, dy) in sorted(offsets.items()):
        note = ""
        if c < 0.6 and good:
            dx, dy = float(np.median([g[0] for g in good])), float(np.median([g[1] for g in good]))
            offsets[w] = (c, dx, dy)
            note = " (weak match - using the common offset)"
        print(f"layer {w:2d}: offset ({dx:+.2f}, {dy:+.2f}) VB px, match {c:.2f}{note}")

    # 3. colors, palette snap, one color per tile pixel
    col = np.zeros((H, W, 3), np.float32)
    sampled = np.zeros((H, W), bool)
    for w, (c, dx, dy) in offsets.items():
        ys, xs = np.nonzero(lit & (world == w))
        val, ok = sample(mock, xs + dx, ys + dy)
        col[ys, xs] = val
        sampled[ys, xs] = ok
    pts = col[sampled]
    k = min(args.colors, len(pts))
    cent = pts[np.random.default_rng(1).choice(len(pts), k, replace=False)]
    for _ in range(25):
        lab = ((pts[:, None] - cent[None]) ** 2).sum(-1).argmin(1)
        for i in range(k):
            if (lab == i).any():
                cent[i] = pts[lab == i].mean(0)
    votes = {}
    for kk, l in zip(key[sampled].tolist(), lab.tolist()):
        v = votes.setdefault(kk, {})
        v[l] = v.get(l, 0) + 1
    choice = {kk: max(v.items(), key=lambda e: e[1])[0] for kk, v in votes.items()}

    out = cap.astype(np.float32).copy() # unlit pixels keep the capture
    ys, xs = np.nonzero(lit)
    missing = 0
    for y, x in zip(ys.tolist(), xs.tolist()):
        c = choice.get(int(key[y, x]))
        if c is None:
            missing += 1
        else:
            out[y, x] = cent[c]
    print(f"scale {s:.3f}; {len(choice)} tile pixels colored; {missing} lit pixels left as captured (outside the mockup)")
    out = out.clip(0, 255).astype(np.uint8)
    Image.fromarray(np.repeat(np.repeat(out, 3, 0), 3, 1)).save(args.out)
    print("wrote", args.out)


if __name__ == "__main__":
    main()
