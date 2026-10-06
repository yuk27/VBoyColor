"""Milliseconds per emulated frame for coloring both eyes, as the app's
UploadFrame does it: colorize + the renderer's paint (best of a few runs per
frame - the machine's noise isn't the renderer's).

usage: timing.py ROM PACK|auto [STATE] [FRAMES]
"""
import sys

import numpy as np
from vbp import VB

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
    vb.auto_colors(True, False)  # the app's default mode (Auto) with the pack
ms = []
for k in range(frames):
    vb.run("" if (k // 20) % 2 else "r", 1)
    ms.append(vb.paint_ms(3))
ms = np.array(ms[10:])
print("mean %.2f ms  p95 %.2f  max %.2f" % (ms.mean(), np.percentile(ms, 95), ms.max()))
