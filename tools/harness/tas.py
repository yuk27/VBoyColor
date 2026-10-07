"""Play a TASVideos / BizHawk movie (.bk2) with the app's core, and collect
everything a color pack doesn't color yet along the way - like the app's F7,
over a whole run - as paint sheets (PNG + .tiles, ready to paint and import).

    python3 tas.py ROM.vb MOVIE.bk2 OUT_DIR [--pack PACK.vbcp] [--all] [--every N] [--frames N]

  --pack    the pack to check against (default: none - everything is new)
  --all     backgrounds too (default: sprites only - characters, enemies,
            items; backgrounds are left to the palette)
  --every   look at every Nth frame (default 2)
  --frames  also save a picture of the run every N frames (to check it plays
            right: a run that drifts off shows the player dying or stuck)

BizHawk's Virtual Boy core ("Virtual Boyee") is Mednafen's, like Beetle VB.
Runs stay in sync with two of the core's options set as BizHawk has them:
accurate CPU emulation and both directions of a D-pad allowed at once (TASes
press them). Checked with Virtual Boy Wario Land "Best Ending" (tasvideos.org
4560M, 61515 frames): it plays to the end.
"""
import argparse
import ctypes as C
import io
import os
import struct
import sys
import zipfile

os.environ["VBP_OPTS"] = "vb_opposite_directions=enabled,vb_cpu_emulation=accurate"

import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

from vbp import VB, lib  # noqa: E402

# BizHawk's VB input columns, in order, as libretro joypad ids (Beetle VB's
# mapping - see its libretro.cpp): left pad U D L R, right pad U D L R, B, A,
# L, R, Select, Start, Power.
COLUMNS = [4, 5, 6, 7, 12, 14, 13, 15, 0, 8, 10, 11, 2, 3, None]

lib.vbp_collect_frame.argtypes = [C.c_int]
lib.vbp_collect_frame.restype = C.c_int
lib.vbp_collect_stats.argtypes = [C.c_int]
lib.vbp_collect_stats.restype = C.c_long
lib.vbp_collect_take.restype = C.c_int
lib.vbp_sheet.argtypes = [C.c_int, C.c_void_p, C.c_void_p]


def movie_frames(path):
    """The movie's frames as libretro button masks."""
    with zipfile.ZipFile(path) as z:
        log = z.read("Input Log.txt").decode("utf-8", "replace")
    for line in io.StringIO(log):
        if not line.startswith("|") or len(line) < 17:
            continue
        mask = 0
        for column, ch in enumerate(line[1:16]):
            if ch != "." and COLUMNS[column] is not None:
                mask |= 1 << COLUMNS[column]
        yield mask


def write_sheet(base, rgb, records, palette):
    """A paint sheet as the app writes F7's: 3x PNG + VBGOTIL2 sidecar (records, no cells, no tile
    dictionary, the colors unpainted pixels show, and what every pixel showed)."""
    Image.fromarray(np.repeat(np.repeat(rgb, 3, 0), 3, 1)).save(base + ".png")
    with open(base + ".tiles", "wb") as f:
        f.write(b"VBGOTIL2" + struct.pack("<II", 384, 224) + records.astype("<u8").tobytes() +
                np.zeros(384 * 224, "<u4").tobytes() + struct.pack("<I", 0) +
                b"VBGOPAL2" + bytes(c for color in palette for c in color) + bytes([255]) +
                b"VBGOSHW1" + rgb.tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("movie")
    ap.add_argument("out")
    ap.add_argument("--pack")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--every", type=int, default=2)
    ap.add_argument("--frames", type=int, default=0)
    a = ap.parse_args()

    vb = VB(a.rom, pack=a.pack)
    os.makedirs(a.out, exist_ok=True)
    palette = [tuple(int(c * 255 + 0.5) for c in row) for row in vb_palette()]
    name = os.path.splitext(os.path.basename(a.rom))[0]
    lib.vbp_collect_reset()
    count = 0
    for count, mask in enumerate(movie_frames(a.movie), 1):
        lib.vbp_run(mask, 1)
        if count % a.every == 0:
            lib.vbp_collect_frame(0 if a.all else 1)
        if a.frames and count % a.frames == 0:
            left, _ = vb.render()
            Image.fromarray(left).save(os.path.join(a.out, "frame %06d.png" % count))
        if count % 10000 == 0:
            print(count, "frames,", lib.vbp_collect_stats(0), "objects so far", flush=True)
    sheets = lib.vbp_collect_take()
    for i in range(sheets):
        rgb = np.zeros((224, 384, 3), np.uint8)
        records = np.zeros((224, 384), np.uint64)
        lib.vbp_sheet(i, rgb.ctypes.data, records.ctypes.data)
        write_sheet(os.path.join(a.out, "%s tas todo %03d" % (name, i + 1)), rgb, records, palette)
    print(count, "frames played,", sheets, "paint sheets in", a.out)


def vb_palette():
    """The capture palette VB() set (Ember), as 0-1 floats - unpainted pixels show in it."""
    from vbp import EMBER
    return EMBER


if __name__ == "__main__":
    sys.exit(main())
