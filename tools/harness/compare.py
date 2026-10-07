"""python3 compare2.py ROM MOVIE OLD NEW OUT EVERY [top] - one pass, both packs at each sampled frame; saves the most
changed frames old|new, plus all sampled frames' diffs ranking."""
import sys, os
os.environ.setdefault("VBP_OPTS", "vb_opposite_directions=enabled,vb_cpu_emulation=accurate")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from PIL import Image
from vbp import VB, lib
from tas import movie_frames
rom, movie, old, new, out, every = sys.argv[1:7]
every = int(every); top = int(sys.argv[7]) if len(sys.argv) > 7 else 24
ob, nb = open(old, "rb").read(), open(new, "rb").read()
vb = VB(rom); vb.auto_colors(True)
res = []
for i, m in enumerate(movie_frames(movie)):
    lib.vbp_run(m, 1)
    if i % every == every - 1:
        vb.load_pack(ob); a, _ = vb.render()
        vb.load_pack(nb); b, _ = vb.render()
        d = int(np.abs(a.astype(int) - b.astype(int)).sum())
        res.append((d, i, a.copy(), b.copy()))
res.sort(key=lambda r: -r[0])
pick = sorted(res[:top], key=lambda r: r[1])
for k in range(0, len(pick), 4):
    rows = [np.hstack([a, np.zeros((224, 8, 3), np.uint8), b]) for _, _, a, b in pick[k:k + 4]]
    Image.fromarray(np.vstack(rows)).save("%s_%02d.png" % (out, k // 4))
print("frames", len(res), "picked", [r[1] for r in pick])
