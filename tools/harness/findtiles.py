"""python3 findtiles.py ROM MOVIE GROUPS.json N OUT_PREFIX - first frames where each of the top N groups' tiles show; saves a frame per group."""
import sys, os, json
os.environ.setdefault("VBP_OPTS", "vb_opposite_directions=enabled,vb_cpu_emulation=accurate")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from PIL import Image
from vbp import VB, lib, U
from tas import movie_frames
rom, movie, gj, n, out = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5]
pack = sys.argv[6] if len(sys.argv) > 6 else None
G = json.load(open(gj))[:n]
want = {int(g["tile"], 16): g["group"] for g in G}
found = {}
vb = VB(rom, pack=pack); vb.auto_colors(True)
for i, m in enumerate(movie_frames(movie)):
    lib.vbp_run(m, 1)
    if i % 3:
        continue
    t = vb.records(0)
    hs = set(np.unique((t & U(0xFFFFFFFF)).astype(np.int64)).tolist()) & set(want)
    new = [h for h in hs if want[h] not in found]
    if new:
        l, r = vb.render()
        for h in new:
            found[want[h]] = i
            Image.fromarray(l).save("%s_g%03d_f%d.png" % (out, want[h], i))
    if len(found) == len(want):
        break
print(sorted(found.items()))
