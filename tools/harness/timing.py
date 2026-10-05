"""Milliseconds per emulated frame for coloring both eyes (colorize + pack/auto paint + whatever else the renderer does).

usage: timing.py ROM PACK|auto [STATE] [FRAMES]
"""
import sys, time
import numpy as np
from vbp import VB, lib

rom, pack = sys.argv[1], sys.argv[2]
state = sys.argv[3] if len(sys.argv) > 3 else "-"
frames = int(sys.argv[4]) if len(sys.argv) > 4 else 200
vb = VB(rom)
if state != "-":
    vb.load_state(state)
if pack == "auto":
    vb.auto_colors(True, True)
else:
    vb.load_pack(pack)
vb.run("", 1)  # (a frame first: the width is known from then on)
w = lib.vbp_width()
img = np.zeros((224, w, 3), np.uint8)
ms = []
for k in range(frames):
    vb.run("" if (k // 20) % 2 else "r", 1)
    t0 = time.perf_counter()
    lib.vbp_render_full(img.ctypes.data, 1)
    ms.append((time.perf_counter() - t0) * 1000)
ms = np.array(ms[10:])
print("mean %.2f ms  p95 %.2f  max %.2f" % (ms.mean(), np.percentile(ms, 95), ms.max()))
