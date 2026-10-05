"""Do both eyes show the same thing in the same colors?

usage: symmetry.py ROM PACK|auto [STATE] [FRAMES] [EVERY]

For every right-eye pixel the left eye shows too - the same tile pixel
(character slot, pixel, palette, sprite or not) on the same row, nearest
within 64 pixels - compare the colors both eyes give it. Those must be
identical: that's the "same point, same color" rule. Right-eye pixels with no
such counterpart (a per-eye picture, or cut off by the screen edge) are
counted apart: their colors can only be judged by region.
"""
import sys, random
import numpy as np
from vbp import VB, tag_fields

rom, pack = sys.argv[1], sys.argv[2]
state = sys.argv[3] if len(sys.argv) > 3 else "-"
frames = int(sys.argv[4]) if len(sys.argv) > 4 else 300
every = int(sys.argv[5]) if len(sys.argv) > 5 else 30
vb = VB(rom)
if state != "-":
    vb.load_state(state)
if pack == "auto":
    vb.auto_colors(True, True)
else:
    vb.load_pack(pack)
rnd = random.Random(7)
cur = ""
tot = dict(shared=0, differ=0, unshared=0)
for k in range(frames):
    if k % 10 == 0:
        cur = rnd.choice(["", "l", "r", "A", "B", "u", "d"])
    vb.run(cur, 1)
    if k % every:
        continue
    left, right = vb.render()
    shade = vb.raw()[:, :, 3] & 3
    off = shade.shape[1] - 384
    t = vb.tags()
    key = [(t[e] & np.uint64((1 << 20) - 1)).astype(np.int64) for e in (0, 1)]  # char, pixel, palette, sprite
    lit = [(t[0] != 0) & (shade[:, :384] > 0), (t[1] != 0) & (shade[:, off:off + 384] > 0)]
    differ = shared = unshared = 0
    for y in range(224):
        at = {}
        for x in np.nonzero(lit[0][y])[0]:
            at.setdefault(int(key[0][y, x]), []).append(int(x))
        for x in np.nonzero(lit[1][y])[0]:
            xs = at.get(int(key[1][y, x]))
            if not xs:
                unshared += 1
                continue
            xl = min(xs, key=lambda v: abs(v - x))
            if abs(xl - x) > 64:
                unshared += 1
                continue
            shared += 1
            differ += bool((left[y, xl] != right[y, x]).any())
    tot["shared"] += shared; tot["differ"] += differ; tot["unshared"] += unshared
    print("frame %4d: same point shown by both eyes %6d px, different color %5d (%.2f%%); right-eye-only content %6d px" % (
        k, shared, differ, 100.0 * differ / max(1, shared), unshared))
print("total:", tot, "different colors %.3f%%" % (100.0 * tot["differ"] / max(1, tot["shared"])))
