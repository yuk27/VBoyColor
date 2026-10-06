"""ctypes wrapper around the harness library (libvbp.so, see CMakeLists.txt):
run a Virtual Boy game headless with the app's own core, tile tracker and
coloring code.

    VBP_LIB    path of libvbp.so (default: build/harness/libvbp.so in the repo)
    VBP_STATES folder for save states (default: ./states)

Buttons: a string of letters - A B S(tart) s(elect) u d l r L R - held for
the frames run.
"""
import ctypes as C
import os
import pickle
import struct

import numpy as np
from PIL import Image

_HERE = os.path.dirname(os.path.abspath(__file__))
lib = C.CDLL(os.environ.get("VBP_LIB", os.path.join(_HERE, "..", "..", "build", "harness", "libvbp.so")))
for name, args, res in [
    ("vbp_init", [C.c_char_p], C.c_int), ("vbp_track", [C.c_int], None), ("vbp_run", [C.c_uint32, C.c_int], None),
    ("vbp_time", [C.c_uint32, C.c_int], C.c_double), ("vbp_width", [], C.c_int), ("vbp_raw", [C.c_void_p], None),
    ("vbp_tiles", [C.c_void_p], None), ("vbp_eyetags", [C.c_void_p], C.c_int), ("vbp_worlds", [C.c_void_p], None),
    ("vbp_tile_rows", [C.c_uint32, C.c_void_p], C.c_int), ("vbp_chr", [C.c_void_p], C.c_int),
    ("vbp_state_size", [], C.c_size_t), ("vbp_save", [C.c_void_p, C.c_size_t], C.c_int), ("vbp_load", [C.c_void_p, C.c_size_t], C.c_int),
    ("vbp_pack_load", [C.c_void_p, C.c_size_t], C.c_int), ("vbp_palette", [C.c_void_p], None), ("vbp_auto", [C.c_int, C.c_int], None),
    ("vbp_render", [C.c_void_p, C.c_int, C.c_void_p], None), ("vbp_render_full", [C.c_void_p, C.c_int], C.c_int),
    ("vbp_worldinfo", [C.c_void_p], None), ("vbp_right_own", [C.c_void_p], C.c_int),
    ("vbp_block_disparity", [C.c_uint, C.c_int, C.c_void_p], None), ("vbp_time_paint", [C.c_int], C.c_double),
    ("vbp_capture_eye", [C.c_uint, C.c_void_p, C.c_void_p], C.c_int), ("vbp_import_begin", [], None),
    ("vbp_import_add", [C.c_void_p, C.c_int, C.c_int, C.c_void_p, C.c_size_t], C.c_int), ("vbp_import_finish", [], C.c_int),
    ("vbp_paint_bytes", [C.c_void_p], C.c_size_t), ("vbp_import_stats", [C.c_void_p], None),
    ("vbp_capture", [C.c_void_p, C.c_void_p], C.c_int), ("vbp_lookup", [C.c_uint32, C.c_uint, C.c_uint, C.c_uint, C.c_void_p], C.c_int),
    ("vbp_fill_mode", [C.c_int], None), ("vbp_wram", [], C.c_void_p)]:
    f = getattr(lib, name)
    f.argtypes = args
    f.restype = res

BTN = {"B": 0, "s": 2, "S": 3, "u": 4, "d": 5, "l": 6, "r": 7, "A": 8, "L": 10, "R": 11}
# The capture palette of the app's F10 / to-do pages (Ember): shades 0-3.
EMBER = np.array([[0.03, 0.01, 0.00], [0.65, 0.15, 0.02], [0.95, 0.55, 0.10], [1.00, 0.95, 0.75]], np.float32)
U = np.uint64


def mask(buttons):
    m = 0
    for c in buttons:
        m |= 1 << BTN[c]
    return m


class VB:
    def __init__(self, rom, pack=None, track=True):
        assert lib.vbp_init(rom.encode()), rom
        lib.vbp_track(1 if track else 0)
        self.palette(EMBER)
        if pack:
            self.load_pack(pack)
        self.frame = 0

    def palette(self, p):
        p = np.ascontiguousarray(p, np.float32)
        lib.vbp_palette(p.ctypes.data)

    def load_pack(self, path_or_bytes):
        b = open(path_or_bytes, "rb").read() if isinstance(path_or_bytes, str) else path_or_bytes
        return lib.vbp_pack_load((C.c_uint8 * len(b)).from_buffer_copy(b), len(b))

    def auto_colors(self, on=True, clear_pack=False):
        lib.vbp_auto(1 if on else 0, 1 if clear_pack else 0)

    def run(self, buttons="", n=1):
        lib.vbp_run(mask(buttons), n)
        self.frame += n

    def script(self, s):
        """'_*60 S*3 A*2': button letters (or _ for none) * frames."""
        for tok in s.split():
            b, n = tok.split("*")
            self.run("" if b == "_" else b, int(n))

    def raw(self):
        """The core's frame, both eyes side by side: B, G, R, tag (low 2 bits shade, high 6 brightness)."""
        a = np.zeros((224, lib.vbp_width(), 4), np.uint8)
        lib.vbp_raw(a.ctypes.data)
        return a

    def records(self, eye=0):
        """Tile records (see vbgo_tiletrack.h, VBGO_TT_*), 224x384, of one eye."""
        a = np.zeros((2, 224, 384), np.uint64)
        lib.vbp_tiles(a.ctypes.data)
        return a[eye]

    def tags(self):
        """Raw per-pixel tags of both eyes (VBGO_TAG_*), 2x224x384; 0 where nothing was drawn."""
        a = np.zeros((2, 224, 384), np.uint64)
        lib.vbp_eyetags(a.ctypes.data)
        return a

    def render(self, pack=True):
        """Both eyes as the app shows them: (left RGB, right RGB). (Run a frame first.)"""
        w = lib.vbp_width()
        img = np.zeros((224, w, 3), np.uint8)
        off = lib.vbp_render_full(img.ctypes.data, 1 if pack else 0)
        return img[:, :384].copy(), img[:, off:off + 384].copy()

    def world_info(self):
        """How the renderer classified the last painted frame's worlds: (per world: eyes (bit 0 L, bit 1 R),
        type, partner or -1, the world whose colors it takes) 32x4, and per world, per 8-row band, a pair's right
        world's disparity (right x -> left x; 0x7FFF unknown) 32x28. (render() a frame first.)"""
        a = np.zeros(128 + 32 * 28, np.int16)
        lib.vbp_worldinfo(a.ctypes.data)
        return a[:128].reshape(32, 4), a[128:].reshape(32, 28)

    def block_disparity(self, right_world, sprites=False):
        """A pair's disparity per 8x8 block of its right picture (28x48; 0x7FFF unknown) - right_world: the pair's
        right world, or the sprite world for its one-eye sprites (sprites=True)."""
        a = np.zeros((28, 48), np.int16)
        lib.vbp_block_disparity(right_world, 1 if sprites else 0, a.ctypes.data)
        return a

    def right_own(self):
        """The last painted frame's right eye: True where a right picture showed a tile of its own (what a
        right-eye capture marks for painting), 224x384; None if the frame had no per-eye pictures."""
        a = np.zeros((224, 384), np.uint8)
        return a.astype(bool) if lib.vbp_right_own(a.ctypes.data) else None

    def paint_ms(self, reps=5):
        """Milliseconds to color the current frame as the app does (colorize + paint), best of reps."""
        return lib.vbp_time_paint(reps)

    def worlds(self):
        """The 32 world attribute blocks (see worlds.py)."""
        w = np.zeros(512, np.uint16)
        lib.vbp_worlds(w.ctypes.data)
        return w.reshape(32, 16)

    def save_state(self, name):
        n = lib.vbp_state_size()
        buf = (C.c_uint8 * n)()
        assert lib.vbp_save(buf, n)
        os.makedirs(states_dir(), exist_ok=True)
        pickle.dump((bytes(buf), self.frame), open(os.path.join(states_dir(), name + ".st"), "wb"))

    def load_state(self, name):
        b, self.frame = pickle.load(open(os.path.join(states_dir(), name + ".st"), "rb"))
        assert lib.vbp_load((C.c_uint8 * len(b)).from_buffer_copy(b), len(b))


def states_dir():
    return os.environ.get("VBP_STATES", "states")


def lookup(record):
    """The loaded pack's color for one record (no map cells): rgb tuple, None (unpainted) or 'keep' (magenta)."""
    r = int(record)
    rgb = (C.c_uint8 * 3)()
    k = lib.vbp_lookup(r & 0xFFFFFFFF, (r >> 43) & 31, (r >> 38) & 3, ((r >> 35) & 7) * 8 + ((r >> 32) & 7), rgb)
    return tuple(rgb) if k == 1 else "keep" if k == -1 else None


def import_folder(folder):
    """Import every painting (PNG + .tiles) in a folder like the app's F11; returns (paintings, tiles, pack bytes, stats)."""
    import glob
    lib.vbp_import_begin()
    n = 0
    for png in sorted(glob.glob(os.path.join(folder, "*.png"))):
        side = png[:-4] + ".tiles"
        if not os.path.exists(side):
            continue
        im = np.ascontiguousarray(np.array(Image.open(png).convert("RGB")))
        sc = open(side, "rb").read()
        n += lib.vbp_import_add(im.ctypes.data, im.shape[1], im.shape[0], (C.c_uint8 * len(sc)).from_buffer_copy(sc), len(sc))
    tiles = lib.vbp_import_finish()
    size = lib.vbp_paint_bytes(None)
    out = (C.c_uint8 * size)()
    lib.vbp_paint_bytes(out)
    st = np.zeros(14, np.uint64)
    lib.vbp_import_stats(st.ctypes.data)
    keys = ["paintings", "sheets", "tilePixels", "fromSheets", "erased", "layerPixels", "inconsistent", "merged",
            "palettePixels", "cellPixels", "fillPixels", "contextTiles", "contextGroups", "rightPixels"]
    return n, tiles, bytes(out), dict(zip(keys, (int(x) for x in st)))


def shown_palette(level):
    """The colors unpainted pixels show in a capture at that brightness level (Ember, as the app's F10)."""
    e = (EMBER * 255 + 0.5).astype(int)
    f = (max(level, 0) / 63.0) ** (1 / 2.2) if level >= 0 else 1.0
    return [tuple(int(round(e[0][c] + (e[i][c] - e[0][c]) * f)) for c in range(3)) if i else tuple(int(x) for x in e[0]) for i in range(4)]


def capture(eye=0):
    """Like the app's F10: (records with fills, map cells, brightness level) - of the left eye, or the right."""
    t = np.zeros((224, 384), np.uint64)
    c = np.zeros((224, 384), np.uint32)
    lvl = lib.vbp_capture_eye(eye, t.ctypes.data, c.ctypes.data)
    return t, c, lvl


def write_capture(base, rgb, t, cells, level, scale=3, right_own=None):
    """The app's F10 files: 3x PNG (rgb: what to paint over, or a painting) + VBGOTIL2 sidecar (records, cells, tile dictionary, palette).
    right_own: a right-eye capture (Shift+F10) - where the right eye shows a picture of its own (VB.right_own())."""
    Image.fromarray(np.repeat(np.repeat(rgb, scale, 0), scale, 1)).save(base + ".png")
    lib.vbgo_tiletrack_hash_rows.restype = C.c_uint32
    lib.vbgo_tiletrack_hash_rows.argtypes = [C.c_void_p]
    chars = np.zeros(2048 * 8, np.uint16)
    lib.vbp_chr(chars.ctypes.data)
    rows_of = {}
    for s in range(2048):
        rows = np.ascontiguousarray(chars[s * 8:(s + 1) * 8])
        rows_of.setdefault(int(lib.vbgo_tiletrack_hash_rows(rows.ctypes.data)), rows)
    hashes = sorted(set(int(h) for h in (t[(t >> U(63)) == 1] & U(0xFFFFFFFF))))
    entries = [struct.pack("<I8H", h, *[int(x) for x in rows_of[h]]) for h in hashes if h in rows_of]
    shown = shown_palette(level)
    with open(base + ".tiles", "wb") as f:
        f.write(b"VBGOTIL2" + struct.pack("<II", 384, 224) + t.astype("<u8").tobytes() + cells.astype("<u4").tobytes() +
                struct.pack("<I", len(entries)) + b"".join(entries) + b"VBGOPAL2" + bytes(c for col in shown for c in col) +
                bytes([level if 0 <= level <= 63 else 255]))
        if right_own is not None:
            f.write(b"VBGOEYE1" + np.ascontiguousarray(right_own, np.uint8).tobytes())


def tag_fields(t):
    """Raw tags (VBGO_TAG_*) -> dict of arrays."""
    return dict(char=(t & U(0x7FF)).astype(np.int32), index=((t >> U(11)) & U(63)).astype(np.int32),
                palette=((t >> U(17)) & U(3)).astype(np.int8), obj=((t >> U(19)) & U(1)).astype(bool),
                pixel=((t >> U(20)) & U(3)).astype(np.int8), world=((t >> U(22)) & U(31)).astype(np.int8),
                has_cell=((t >> U(27)) & U(1)).astype(bool), cell=((t >> U(28)) & U(0xFFFF)).astype(np.int32),
                obj_no=((t >> U(28)) & U(0x3FF)).astype(np.int32), drawn=t != 0)


def save_png(img, path, scale=1):
    Image.fromarray(np.repeat(np.repeat(img, scale, 0), scale, 1)).save(path)
