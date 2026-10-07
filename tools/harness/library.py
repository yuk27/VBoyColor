"""Pass A: play a TAS and gather every tile's 8x8 pattern (from CHR RAM) and every sprite tile's
(palette, world) as it's drawn, plus the pack's colors for those - the library extrapolation works from.
    python3 library.py ROM MOVIE PACK OUT.npz"""
import sys, os, ctypes as C
os.environ.setdefault("VBP_OPTS", "vb_opposite_directions=enabled,vb_cpu_emulation=accurate")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from vbp import VB, lib, lookup, U
from tas import movie_frames
lib.vbgo_tiletrack_hash_rows.restype = C.c_uint32
lib.vbgo_tiletrack_hash_rows.argtypes = [C.c_void_p]
rom, movie, pack, out = sys.argv[1:5]
vb = VB(rom, pack=pack)
patterns = {}          # hash -> 8x8 uint8 pixel values
rowcache = {}          # 16 bytes -> hash
sprites = {}           # hash -> {(palette, world): count}
bgs = {}
chars = np.zeros(2048 * 8, np.uint16)
def dump_chr():
    lib.vbp_chr(chars.ctypes.data)
    b = chars.tobytes()
    for s in range(2048):
        key = b[s * 16:(s + 1) * 16]
        if key in rowcache:
            continue
        rows = np.frombuffer(key, np.uint16).copy()
        h = int(lib.vbgo_tiletrack_hash_rows(rows.ctypes.data))
        rowcache[key] = h
        if h not in patterns:
            pat = np.zeros((8, 8), np.uint8)
            for y in range(8):
                for x in range(8):
                    pat[y, x] = (int(rows[y]) >> (2 * x)) & 3
            patterns[h] = pat
for i, m in enumerate(movie_frames(movie)):
    lib.vbp_run(m, 1)
    if i % 2:
        continue
    t = vb.records(0)
    d = (t >> U(63)) == U(1)
    v = t[d]
    obj = ((v >> U(40)) & U(1)).astype(bool)
    keys = np.unique((v & U(0xFFFFFFFF)) | (((v >> U(38)) & U(3)) << U(32)) | (((v >> U(43)) & U(31)) << U(34)) | (obj.astype(np.uint64) << U(39)))
    for k in keys.tolist():
        h, pal, world, o = k & 0xFFFFFFFF, (k >> 32) & 3, (k >> 34) & 31, (k >> 39) & 1
        dd = sprites if o else bgs
        e = dd.setdefault(h, {})
        e[(pal, world)] = e.get((pal, world), 0) + 1
    if i % 30 == 0:
        dump_chr()
dump_chr()
# the pack's colors for every sprite tile, in its most common palette/world: rgb per pixel, -1 unpainted
def colors_for(h, pal, world):
    col = np.full((8, 8, 3), -1, np.int16)
    for y in range(8):
        for x in range(8):
            rec = U(h) | (U(x) << U(32)) | (U(y) << U(35)) | (U(pal) << U(38)) | (U(world) << U(43)) | U(1 << 63)
            r = lookup(rec)
            if isinstance(r, tuple):
                col[y, x] = r
    return col
hs = sorted(set(sprites) | set(bgs))
pat = np.zeros((len(hs), 8, 8), np.uint8); col = np.full((len(hs), 8, 8, 3), -1, np.int16)
isobj = np.zeros(len(hs), bool); seen = np.zeros(len(hs), np.int64); has = np.zeros(len(hs), bool)
for n, h in enumerate(hs):
    e = sprites.get(h) or bgs.get(h)
    (pal, world), cnt = max(e.items(), key=lambda kv: kv[1])
    isobj[n] = h in sprites; seen[n] = sum(e.values())
    if h in patterns:
        pat[n] = patterns[h]; has[n] = True
    col[n] = colors_for(h, pal, world)
np.savez_compressed(out, hashes=np.array(hs, np.uint64), pat=pat, col=col, isobj=isobj, seen=seen, has=has)
print(len(hs), "tiles,", int(isobj.sum()), "sprite tiles,", int(has.sum()), "with patterns,", len(patterns), "patterns total")
