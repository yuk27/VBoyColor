"""python3 bgscan.py ROM MOVIE OUT.npy - every tile hash drawn on a background layer during the run (every 2nd frame)."""
import sys, os
os.environ.setdefault("VBP_OPTS", "vb_opposite_directions=enabled,vb_cpu_emulation=accurate")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from vbp import VB, lib, U
from tas import movie_frames
rom, movie, out = sys.argv[1:4]
vb = VB(rom)
bg = set()
for i, m in enumerate(movie_frames(movie)):
    lib.vbp_run(m, 1)
    if i % 2:
        continue
    for eye in (0, 1):
        t = vb.records(eye)
        v = t[((t >> U(63)) == U(1)) & (((t >> U(40)) & U(1)) == U(0))]
        bg.update(np.unique((v & U(0xFFFFFFFF)).astype(np.int64)).tolist())
np.save(out, np.array(sorted(bg), np.int64))
print(len(bg), "background tiles")
