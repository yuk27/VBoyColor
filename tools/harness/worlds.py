"""How a game draws its 3D: the VIP's 32 worlds (layers) at one frame.

usage: worlds.py ROM [STATE] [SCRIPT]     e.g. worlds.py gp.vb gp_table "_*60"

Per world: which eyes draw it (L/R), its type, BG map, and the parallax
registers. A pixel of a normal world comes from map x = MX -/+ MP + (x - (GX -/+ GP))
in the left/right eye (H-bias adds a per-row, per-eye offset from its
parameter table; sprites use JX -/+ JP). So:
  - a world both eyes draw (LR) shows the same tile pixels in both eyes,
    shifted: coloring by tile is the same in both eyes by construction;
  - an L-only world next to an R-only one is a per-eye pair: either a copy of
    the same tiles at another map position (Mario Clash: ~100% shared tiles)
    or two different pictures, a stereo pair (Galactic Pinball: 0-30%).
"""
import sys
import numpy as np
from vbp import VB, tag_fields

def s11(v):
    v = int(v) & 0x7FF
    return v - 0x800 if v & 0x400 else v

def s9(v):
    v = int(v) & 0x1FF
    return v - 0x200 if v & 0x100 else v

rom = sys.argv[1]
vb = VB(rom)
if len(sys.argv) > 2 and sys.argv[2] != "-":
    vb.load_state(sys.argv[2])
vb.script(sys.argv[3] if len(sys.argv) > 3 else "_*2")
w = vb.worlds()
f = tag_fields(vb.tags())
raw = vb.raw()
shade = [raw[:, :384, 3] & 3, raw[:, raw.shape[1] - 384:, 3] & 3]
L, R = {k: v[0] for k, v in f.items()}, {k: v[1] for k, v in f.items()}
listed = []
for i in range(31, -1, -1):
    a = int(w[i][0])
    if a & 0x40:
        print("w%-2d END" % i)
        break
    eyes = ("L" if a & 0x8000 else "-") + ("R" if a & 0x4000 else "-")
    if eyes == "--":
        continue
    kind = ["normal", "hbias", "affine", "OBJ"][(a >> 12) & 3]
    px = [int((e["drawn"] & (e["world"] == i)).sum()) for e in (L, R)]
    print("w%-2d %s %-6s map %-2d GX %4d GP %4d GY %4d MX %4d MP %4d MY %4d  %3dx%-3d  pixels L %6d R %6d" % (
        i, eyes, kind, a & 15, s11(w[i][1]), s9(w[i][2]), s11(w[i][3]), s11(w[i][4]), s9(w[i][5]), s11(w[i][6]),
        s11(w[i][7]) + 1, (int(w[i][8]) & 0x3FF) + 1, px[0], px[1]))
    listed.append((i, eyes))
# Per-eye pairs: an L-only and an R-only world next to each other.
i = 0
while i + 1 < len(listed):
    (a, ea), (b, eb) = listed[i], listed[i + 1]
    if {ea, eb} != {"L-", "-R"}:
        i += 1
        continue
    i += 2
    if True:
        lw, rw = (a, b) if ea == "L-" else (b, a)
        lc = set(L["char"][L["drawn"] & (L["world"] == lw)].tolist())
        rc = R["char"][R["drawn"] & (R["world"] == rw)]
        share = 100.0 * np.isin(rc, list(lc)).mean() if rc.size else 0.0
        # One shift for the whole picture? (a pre-shifted copy - tiles redrawn, so not shared - lines up exactly)
        lm = np.where(L["drawn"] & (L["world"] == lw), shade[0], 0).astype(np.int8)
        rm = np.where(R["drawn"] & (R["world"] == rw), shade[1], 0).astype(np.int8)
        best = (0.0, 0)
        lit = max(1, int((rm > 0).sum()))
        for dx in range(-64, 65):
            a_ = rm[:, max(0, -dx):384 - max(0, dx)]
            b_ = lm[:, max(0, dx):384 - max(0, -dx)]
            same = float(((a_ == b_) & (a_ > 0)).sum()) / lit
            if same > best[0]:
                best = (same, dx)
        verdict = ("the same picture, shifted %+d px (exact pixel correspondence)" % best[1] if best[0] > 0.9 else
                   "two different pictures - a stereo pair (best single shift %+d px matches only %.0f%%)" % (best[1], 100 * best[0]))
        print("pair L w%d / R w%d: %.0f%% of the right picture's pixels use tiles the left one uses; %s" % (lw, rw, share, verdict))
