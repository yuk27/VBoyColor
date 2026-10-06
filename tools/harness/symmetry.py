"""Do both eyes show the same thing in the same colors?

usage: symmetry.py ROM PACK|auto [STATE] [FRAMES] [EVERY]

Shared content - layers both eyes draw and sprites: for every right-eye pixel,
the same point in the left eye - the same layer, map cell and tile pixel (a
sprite: the same OBJ and tile pixel), nearest on the same row within 64
pixels. Those must be identical: that's the "same point, same color" rule,
and it should be 0.00%.

Per-eye pairs (a left-only layer next to a right-only one, as the renderer
classifies them): a right-picture pixel of a tile the left picture also uses,
at the spot nearest to where its band's disparity puts it that the left
picture shows that tile pixel on the row (pictures repeat tiles, so this is
only a guide: a few pixels of depth inside a band can pair the wrong copy) -
colored by tile, so mostly the same, short of map-cell colors only the left
picture's cells have. The right picture's own tiles (and right-eye-only
sprites) have no point-for-point counterpart - they're colored by region from
the left picture (see ColorPackRenderer.h) and counted apart.
"""
import collections
import random
import sys

import numpy as np
from vbp import VB, tag_fields


def measure(vb, left, right):
    shade = vb.raw()[:, :, 3] & 3
    off = shade.shape[1] - 384
    t = vb.tags()
    info, disparity = vb.world_info()
    F = [{k: v[e] for k, v in tag_fields(t).items()} for e in (0, 1)]
    lit = [(t[e] != 0) & (shade[:, off * e:off * e + 384] > 0) & (F[e]["pixel"] > 0) for e in (0, 1)]

    def keys(e):
        f = F[e]
        bg = (f["world"].astype(np.int64) << 40) | (f["cell"].astype(np.int64) << 8) | f["index"]
        ob = (1 << 50) | (f["obj_no"].astype(np.int64) << 8) | f["index"]
        tile = (f["char"].astype(np.int64) << 8) | f["index"] | (f["palette"].astype(np.int64) << 6)
        return np.where(f["obj"], ob, bg), tile

    (K0, T0), (K1, T1) = keys(0), keys(1)
    partner = info[:, 2]  # (a left world's partner: its pair's right world - a pair may have several left worlds)
    rw = F[1]["world"]
    pair_right = (~F[1]["obj"]) & (partner[rw] >= 0) & ((info[rw, 0] & 3) == 2)
    st = collections.Counter()
    for y in range(224):
        at, tiles = {}, {}
        for x in np.nonzero(lit[0][y])[0]:
            at.setdefault(int(K0[y, x]), []).append(int(x))
            lw = int(F[0]["world"][y, x])
            if not F[0]["obj"][y, x] and (info[lw, 0] & 3) == 1 and partner[lw] >= 0:
                tiles.setdefault((int(partner[lw]), int(T0[y, x])), []).append(int(x))
        for x in np.nonzero(lit[1][y])[0]:
            xs = tiles.get((int(rw[y, x]), int(T1[y, x]))) if pair_right[y, x] else at.get(int(K1[y, x]))
            kind = "pair" if pair_right[y, x] else "shared"
            # (a pair: the instance nearest to where its band's disparity puts it - pictures repeat tiles)
            d = int(disparity[rw[y, x]][y // 8]) if pair_right[y, x] else 0
            target = x + (d if d != 0x7FFF else 0)
            xl = min(xs, key=lambda v: abs(v - target)) if xs else None
            if xl is None or abs(xl - target) > 64:
                st["pair_own" if pair_right[y, x] else "right_only"] += 1
                continue
            st[kind] += 1
            st[kind + "_differ"] += bool((left[y, xl] != right[y, x]).any())
    return st


if __name__ == "__main__":
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
        vb.auto_colors(True, False)  # the app's default mode (Auto) with the pack
    rnd = random.Random(7)
    cur = ""
    tot = collections.Counter()
    for k in range(frames):
        if k % 10 == 0:
            cur = rnd.choice(["", "l", "r", "A", "B", "u", "d"])
        vb.run(cur, 1)
        if k % every:
            continue
        left, right = vb.render()
        st = measure(vb, left, right)
        tot.update(st)
        print("frame %4d: shared %6d px, different %5d (%.2f%%) | pairs' shared tiles %6d px, different %5d | "
              "pairs' own tiles %6d px | right eye only %6d px" % (
                  k, st["shared"], st["shared_differ"], 100.0 * st["shared_differ"] / max(1, st["shared"]), st["pair"],
                  st["pair_differ"], st["pair_own"], st["right_only"]))
    print("total: shared content, different colors %.3f%% (%d of %d px); pairs' shared tiles %.2f%% (%d of %d px)" % (
        100.0 * tot["shared_differ"] / max(1, tot["shared"]), tot["shared_differ"], tot["shared"],
        100.0 * tot["pair_differ"] / max(1, tot["pair"]), tot["pair_differ"], tot["pair"]))
