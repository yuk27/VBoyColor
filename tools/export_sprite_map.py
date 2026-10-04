"""Export the full sprite map of Mario's Tennis (Virtual Boy) straight from the ROM.

Mario's Tennis keeps almost all of its graphics compressed, so a plain tile
viewer only shows noise. This tool decompresses everything the game itself
decompresses and lays it out three ways:

1. "<game> - sprite map.png": one overview of everything, labelled -
   every character's animation frames (near and far court), the screens
   (title, menus, bracket, results, court backgrounds) and every tile set in
   the order the game loads it into character memory.
2. "paint sheets/": the same graphics cut into 3x paint sheets with a .tiles
   file each, so a painted copy imports into a color pack (see "Color packs"
   in README.md). They import like F6 tile sheets: they fill in whatever the
   screen paintings don't color. Tiles that only exist outside the assembled
   sprites and screens are collected on "loose tiles" sheets, so every tile in
   the ROM can be painted somewhere.
3. "character sheets/": one sheet per player with every animation frame the
   color pack doesn't fully color yet (near court, then far court), in the
   shades the court shows them. Unlike tile sheets, these import like screen
   paintings: each frame is a figure of its own and the sheet's frames are one
   character, so its colors stay its own on the tiles players share
   (--characters-only writes just these).

If the ROM has a color pack next to it ("<rom>.vbcp"), everything it already
colors is drawn in its colors, so what's left to paint stands out.

What the game stores, and how it's read here (all reverse engineered from the
ROM's own code; addresses are ROM offsets):

- LZSS (routine at 0x135C): 00 00, size - 1 (big-endian 16 bit), then flag
  bytes (LSB first, 1 = literal byte) and 2-byte matches: distance =
  b1 << 4 | b0 >> 4, length = (b0 & 15) + 3. Used for tile sets and BG maps.
- Word repeat (routine at 0x38E4): per 16 halfwords one flag halfword (MSB
  first, 1 = repeat the previous halfword). Used for the near-court player
  frames, which are streamed into character memory one frame at a time.
- Per-character records at 0x7BC50 (12 bytes each: parts tile set, frame
  stream base, far-court tile set), layout atlases at 0x7BC10 (BG maps; frame
  n's 16x16-cell layout is at cell (16 * (n & 3), 16 * (n >> 2))), far-court
  layout maps at 0x7BC30 (8 x 8-cell layouts, 8 per row), and per-character
  frame tables at 0x7C438 (animation -> frame) / 0x7C43C (frame -> stream
  offset in halfwords).

Tile ids match the emulator's tile tracker (FNV-1a hash of the tile's 16
bytes - see core/emu/vbgo_tiletrack.c), so the ROM's tiles and the tiles the
emulator sees on screen are the same keys.

    python tools/export_sprite_map.py "roms/Mario's Tennis (Japan, USA).vb"

Options: --out DIR (default: "sprite map" next to the ROM), --scale N (paint
sheet upscale, default 3), --overview-scale N (default 2), --pack FILE (a
.vbcp to pre-color with; default "<rom>.vbcp" if it exists), --no-pack.

Requires numpy and Pillow.
"""
import argparse
import struct
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

# Paint sheet size in VB pixels: the screen's width, a bit taller than the
# screen - the importer treats anything that isn't exactly a screen capture
# (384x224) as a tile sheet, which only fills in what screen paintings don't
# color.
W, H = 384, 240
SIDECAR_MAGIC = b"VBGOTIL1"
PALETTE_MAGIC = b"VBGOPAL2"  # + background and 3 shades (RGB) + brightness level (255 = not a screen)

# The four shades as the F10 references show them (Multicolor "Ember" at the
# game's brightness), so these sheets look like the captures next to them.
# The sidecars list them too, so the importer knows unpainted pixels.
PALETTE = np.array([(8, 3, 0), (149, 34, 5), (217, 125, 23), (228, 216, 171)], dtype=np.uint8)
CHARACTERS = ["Mario", "Luigi", "Princess", "Yoshi", "Toad", "Koopa", "Donkey Jr"]


# ---------------------------------------------------------------- ROM + decoders


class Rom:
    def __init__(self, data):
        self.data = data
        self.mask = len(data) - 1

    def off(self, address):
        """CPU address (0x07xxxxxx or a 0xFFFxxxxx mirror) -> file offset."""
        return address & self.mask

    def u16(self, off):
        return struct.unpack_from("<H", self.data, off)[0]

    def u32(self, off):
        return struct.unpack_from("<I", self.data, off)[0]

    def ptr(self, off):
        return self.off(self.u32(off))

    def title(self):
        return self.data[-0x220:-0x220 + 20].decode("shift_jis", "replace").strip()

    def lzss(self, off):
        """The game's LZSS decoder (0x135C), byte for byte. Distance 0 and
        references before the start read the destination, which the game
        clears first - zeros."""
        d = self.data
        if d[off] | d[off + 1]:
            raise ValueError("no LZSS stream at 0x%X" % off)
        p = off + 4
        left = total = (d[off + 2] << 8 | d[off + 3]) + 1
        out = bytearray()
        while True:
            flags = d[p]
            p += 1
            for _ in range(8):
                b = d[p]
                p += 1
                if flags & 1:
                    out.append(b)
                    left -= 1
                else:
                    dist = d[p] << 4 | b >> 4
                    p += 1
                    n = (b & 15) + 3
                    left -= n
                    if left < 0:
                        n += left
                    src = len(out) - dist
                    if dist == 0 or src < 0:
                        zeros = n if dist == 0 else -src
                        out.extend(bytes(zeros))
                        n -= zeros
                        src = 0
                    for i in range(max(n, 0)):
                        out.append(out[src + i])
                flags >>= 1
                if left < 0:
                    return bytes(out[:total])
            if left < 0:
                return bytes(out[:total])

    def word_repeat(self, off, nbytes=0x400):
        """The game's frame-stream decoder (0x38E4). Returns (data, end offset)."""
        out = []
        p = off
        prev = 0
        for _ in range(nbytes // 32):
            flags = self.u16(p)
            p += 2
            for _ in range(16):
                if not flags & 0x8000:
                    prev = self.u16(p)
                    p += 2
                out.append(prev)
                flags = (flags << 1) & 0xFFFF
        return struct.pack("<%dH" % len(out), *out), p


# ---------------------------------------------------------------- tiles


def fnv1a(tile16):
    """Same hash as vbgo_tiletrack.c's HashChar (over the 8 rows' bytes, low byte first)."""
    h = 2166136261
    for b in tile16:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


class TileCache:
    """Tile bytes -> (8x8 pixel values, hash)."""

    def __init__(self):
        self.cache = {}

    def get(self, tile16):
        hit = self.cache.get(tile16)
        if hit is None:
            rows = struct.unpack("<8H", tile16)
            px = np.array([[(r >> (2 * x)) & 3 for x in range(8)] for r in rows], dtype=np.uint8)
            hit = (px, fnv1a(tile16))
            self.cache[tile16] = hit
        return hit


TILES = TileCache()


class PackColors:
    """A color pack's own tile colors (the first section of a .vbcp - same in
    every version, see TileColorPack::Serialize): hash -> (mask, 64 x RGB)."""

    def __init__(self, data=None):
        self.tiles = {}
        if not data:
            return
        if data[:7] != b"VBGOCP0" or len(data) < 12:
            raise ValueError("not a color pack (.vbcp)")
        count = struct.unpack_from("<I", data, 8)[0]
        off = 12
        for _ in range(count):
            h, mask = struct.unpack_from("<IQ", data, off)
            rgb = np.frombuffer(data, np.uint8, 192, off + 12).reshape(64, 3)
            self.tiles[h] = (mask, rgb)
            off += 4 + 8 + 192

    def paint(self, pix, rec):
        """pix/rec -> RGB image: the pack's colors where it has them, the
        sheet palette elsewhere. Also returns how many shown pixels it colored."""
        rgb = PALETTE[pix].copy()
        if not self.tiles:
            return rgb, 0
        colored = 0
        ys, xs = np.nonzero((rec >> np.uint64(63)) & np.uint64(1))
        for y, x in zip(ys.tolist(), xs.tolist()):
            r = int(rec[y, x])
            tile = self.tiles.get(r & 0xFFFFFFFF)
            if tile is None:
                continue
            i = ((r >> 35) & 7) * 8 + ((r >> 32) & 7)
            if tile[0] >> i & 1:
                rgb[y, x] = tile[1][i]
                colored += 1
        return rgb, colored


def record(h, sub_x, sub_y, obj, pixel, char, world=0, palette=0):
    """One 64-bit entry of a .tiles file (field layout: core/emu/vbgo_tiletrack.h)."""
    return (h | sub_x << 32 | sub_y << 35 | (palette & 3) << 38 | obj << 40 | pixel << 41 | (world & 31) << 43 |
            (char & 0x7FF) << 48 | 1 << 63)


class Item:
    """A rendered graphic: pixel values 0-3 plus one tile record per pixel."""

    def __init__(self, pix, rec, label=""):
        self.pix, self.rec, self.label = pix, rec, label

    @property
    def h(self):
        return self.pix.shape[0]

    @property
    def w(self):
        return self.pix.shape[1]

    def key(self):
        # Same picture, same tiles -> same item (the debug-only char field may differ).
        return self.pix.tobytes() + (self.rec & np.uint64(~(0x7FF << 48) & 0xFFFFFFFFFFFFFFFF)).tobytes()

    def cropped(self, margin=0):
        ys, xs = np.nonzero(self.pix)
        if len(ys) == 0:
            return None
        y0, y1 = max(ys.min() - margin, 0), min(ys.max() + 1 + margin, self.h)
        x0, x1 = max(xs.min() - margin, 0), min(xs.max() + 1 + margin, self.w)
        return Item(self.pix[y0:y1, x0:x1], self.rec[y0:y1, x0:x1], self.label)

    def tile_hashes(self):
        valid = (self.rec >> 63) & 1 == 1
        return set((self.rec[valid] & 0xFFFFFFFF).tolist())


def render_cells(cells, chr_ram, loaded, obj=0):
    """Draws a BG map (rows of 16-bit cells: char 0-10, V flip 12, H flip 13)
    from a 2048-character memory image. Characters nothing loaded are skipped."""
    rows, cols = cells.shape
    pix = np.zeros((rows * 8, cols * 8), np.uint8)
    rec = np.zeros((rows * 8, cols * 8), np.uint64)
    sub = np.arange(8)
    for cy in range(rows):
        for cx in range(cols):
            v = int(cells[cy, cx])
            ch = v & 0x7FF
            if not loaded[ch]:
                continue
            tile = chr_ram[ch * 16:ch * 16 + 16]
            px, h = TILES.get(tile)
            if not px.any():
                continue
            sx = sub[::-1] if v & 0x2000 else sub  # screen column -> tile column
            sy = sub[::-1] if v & 0x1000 else sub
            vals = px[np.ix_(sy, sx)]
            r = np.zeros((8, 8), np.uint64)
            for yy in range(8):
                for xx in range(8):
                    p = int(vals[yy, xx])
                    if p:
                        r[yy, xx] = record(h, int(sx[xx]), int(sy[yy]), obj, p, ch)
            pix[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8] = vals
            rec[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8] = r
    return Item(pix, rec)


def tile_grid(tiles, cols, gap=1):
    """Tiles (16-byte strings, None = skip a slot) in rows of `cols`, `gap` pixels apart."""
    n = len(tiles)
    rows = (n + cols - 1) // cols
    pitch = 8 + gap
    pix = np.zeros((rows * pitch - gap, min(n, cols) * pitch - gap), np.uint8)
    rec = np.zeros(pix.shape, np.uint64)
    for i, t in enumerate(tiles):
        if t is None:
            continue
        px, h = TILES.get(t)
        y, x = (i // cols) * pitch, (i % cols) * pitch
        pix[y:y + 8, x:x + 8] = px
        for yy in range(8):
            for xx in range(8):
                if px[yy, xx]:
                    rec[y + yy, x + xx] = record(h, xx, yy, 0, int(px[yy, xx]), 0)
    return Item(pix, rec)


class CharMemory:
    """The VIP's 2048 characters (32 KB at 0x78000), built up load by load."""

    def __init__(self):
        self.ram = bytearray(0x8000)
        self.loaded = np.zeros(2048, bool)

    def load(self, address, data):
        off = address - 0x78000
        self.ram[off:off + len(data)] = data
        self.loaded[off // 16:(off + len(data) + 15) // 16] = True
        return self

    def copy(self, src, dst, size):
        s, d = src - 0x78000, dst - 0x78000
        self.ram[d:d + size] = self.ram[s:s + size]
        self.loaded[d // 16:(d + size) // 16] = self.loaded[s // 16:(s + size) // 16]
        return self

    def render(self, cells, obj=0):
        return render_cells(cells, bytes(self.ram), self.loaded, obj)


def cells_of(data, width=64):
    a = np.frombuffer(data[:len(data) // (2 * width) * 2 * width], dtype="<u2")
    return a.reshape(-1, width)


# ---------------------------------------------------------------- Mario's Tennis


def characters(rom):
    """Per character: (name, near-court frames, far-court frames) as Items."""
    result = []
    for c, name in enumerate(CHARACTERS):
        rec = 0x7BC50 + c * 12
        parts = rom.lzss(rom.ptr(rec))  # -> char 512 (0x7A000), 192 tiles
        stream_base = rom.ptr(rec + 4)
        far_tiles = rom.lzss(rom.ptr(rec + 8))  # -> char 1024 (0x7C000)
        atlas = cells_of(rom.lzss(rom.ptr(0x7BC10 + c * 4)))
        far_map = cells_of(rom.lzss(rom.ptr(0x7BC30 + c * 4)))
        anim_table, frame_table = rom.ptr(0x7C438 + c * 8), rom.ptr(0x7C43C + c * 8)

        near = []
        for n in range((frame_table - anim_table) // 2):
            frame = rom.u16(anim_table + 2 * n)
            tiles, _ = rom.word_repeat(stream_base + 2 * rom.u16(frame_table + 2 * frame))
            mem = CharMemory().load(0x7A000, parts).load(0x7AC00, tiles)
            y, x = 16 * (n >> 2), 16 * (n & 3)
            if y + 16 > atlas.shape[0]:
                break
            item = mem.render(atlas[y:y + 16, x:x + 16], obj=1).cropped()
            if item:
                item.label = "%s %d" % (name, n)
                near.append(item)

        far = []
        mem = CharMemory().load(0x7A000, parts).load(0x7C000, far_tiles)
        for y in range(0, far_map.shape[0] - 7, 8):
            for x in range(0, 64, 8):
                item = mem.render(far_map[y:y + 8, x:x + 8], obj=1).cropped()
                if item:
                    item.label = "%s far %d" % (name, (y // 8) * 8 + x // 8)
                    far.append(item)
        result.append((name, near, far))
    return result


def screens(rom):
    """BG maps drawn with the tiles their screen loads (taken from the loader
    routines' own LZSS calls). Returns (label, Item) pairs."""
    lz = rom.lzss
    raw_ball = rom.data[0x16200:0x16220]  # 2 uncompressed tiles copied to char 2046
    font = lz(0x1FB4C)

    def mem(*loads):
        m = CharMemory()
        for address, data in loads:
            m.load(address, data)
        return m

    boot = mem((0x78000, lz(0x1EFAA)), (0x79000, lz(0x1EFAA)), (0x7FFE0, raw_ball))
    title = mem((0x78000, font), (0x79000, lz(0x1BA5A)), (0x7D000, lz(0x1CF42)), (0x7FFE0, raw_ball))
    # The mode menu copies the font's chars 256-367 down to char 0 (routine
    # around 0x10E90), then loads its own set on top.
    menu = mem((0x78000, font)).copy(0x79000, 0x78000, 0x700).load(0x78700, lz(0x19812))
    bracket = mem((0x78000, font), (0x78700, lz(0x1EAAC)), (0x79000, lz(0x166C2)), (0x7A000, lz(0x172E4)))
    results = mem((0x78000, font), (0x7B000, lz(0x18AAC)), (0x7BA00, lz(0x1EAAC)), (0x7C000, lz(0x172E4)))
    out = [
        ("Warning and options", boot.render(cells_of(lz(0x1F3E2)))),
        ("Title logo", title.render(cells_of(lz(0x1A7FA)))),
        ("Title Mario", title.render(cells_of(lz(0x1B0D8)))),
        ("Court from above", title.render(cells_of(lz(0x1F620)))),
        ("Mode menu", menu.render(cells_of(lz(0x18FDA)))),
        ("Tournament bracket", bracket.render(cells_of(lz(0x16220)))),
        ("Character faces", bracket.render(cells_of(lz(0x16A28)))),
        ("Results", results.render(cells_of(lz(0x183FE)))),
    ]
    # Court backgrounds: 7 courts, 24 map rows each in 0x72AD4, each drawn with
    # its own 96-tile set from 0x73EDA - the match loader copies court k's rows
    # and tiles into place (routines around 0x21F0 and 0x2350).
    court_map = cells_of(lz(0x72AD4))
    staging = lz(0x73EDA)
    for k in range(len(staging) // 0x600):
        m = mem((0x78000, font), (0x7A000, staging)).copy(0x7A000 + k * 0x600, 0x79A00, 0x600)
        out.append(("Court background %d" % (k + 1), m.render(court_map[24 * k:24 * k + 24])))
    return out


def tile_sets(rom):
    """Every tile set in the ROM, as the game loads it: (label, [16-byte tiles])."""
    sets = [
        (0x1FB4C, "Font, HUD and court"),
        (0x1EFAA, "Boot screens"),
        (0x1BA5A, "Title screen"),
        (0x1CF42, "Title Mario"),
        (0x19812, "Mode menu"),
        (0x1EAAC, "Menu frames"),
        (0x166C2, "Tournament"),
        (0x172E4, "Faces and bracket"),
        (0x18AAC, "Results"),
        (0x20A9A, "Change ends / service"),
        (0x73EDA, "Court backgrounds"),
    ]
    for c, name in enumerate(CHARACTERS):
        rec = 0x7BC50 + c * 12
        sets.append((rom.ptr(rec), "%s parts" % name))
        sets.append((rom.ptr(rec + 8), "%s far court" % name))
    out = []
    for off, label in sets:
        data = rom.lzss(off)
        out.append(("%s (0x%05X)" % (label, off), [data[i:i + 16] for i in range(0, len(data) - 15, 16)]))
    out.append(("Ball (raw, 0x16200)", [rom.data[0x16200:0x16210], rom.data[0x16210:0x16220]]))
    return out


# ---------------------------------------------------------------- splitting + packing


def split_objects(item, max_w=W - 4, max_h=H - 4, join=8):
    """Splits a big rendered map into its separate objects (content further
    than `join` pixels apart), each cut to fit on a sheet."""
    occ = item.pix > 0
    # Coarse grid of 8x8 blocks, grown by `join` so nearby parts stay together.
    gh, gw = (item.h + 7) // 8, (item.w + 7) // 8
    grid = np.zeros((gh, gw), bool)
    for y in range(gh):
        for x in range(gw):
            grid[y, x] = occ[y * 8:y * 8 + 8, x * 8:x * 8 + 8].any()
    r = max(join // 8, 0)
    grown = grid.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            grown |= np.roll(np.roll(grid, dy, 0), dx, 1)
    labels = -np.ones((gh, gw), int)
    pieces = []
    for y in range(gh):
        for x in range(gw):
            if grid[y, x] and labels[y, x] < 0:
                stack = [(y, x)]
                labels[y, x] = len(pieces)
                cells = []
                while stack:
                    cy, cx = stack.pop()
                    cells.append((cy, cx))
                    for ny in range(cy - 1, cy + 2):
                        for nx in range(cx - 1, cx + 2):
                            if 0 <= ny < gh and 0 <= nx < gw and grown[ny, nx] and labels[ny, nx] < 0:
                                labels[ny, nx] = len(pieces)
                                stack.append((ny, nx))
                ys = [c[0] for c in cells]
                xs = [c[1] for c in cells]
                pieces.append((min(ys) * 8, max(ys) * 8 + 8, min(xs) * 8, max(xs) * 8 + 8))
    out = []
    for y0, y1, x0, x1 in pieces:
        for ty in range(y0, y1, max_h - max_h % 8):
            for tx in range(x0, x1, max_w - max_w % 8):
                sub = Item(item.pix[ty:min(ty + max_h - max_h % 8, y1), tx:min(tx + max_w - max_w % 8, x1)],
                           item.rec[ty:min(ty + max_h - max_h % 8, y1), tx:min(tx + max_w - max_w % 8, x1)],
                           item.label).cropped()
                if sub:
                    out.append(sub)
    return out


def pack(items, width, gap, max_height=None):
    """Row-by-row packing, keeping the items' order. Returns pages of
    (item, x, y) lists, each with its used height."""
    pages, page, x, y, row_h = [], [], 0, 0, 0
    for it in items:
        if x and x + it.w > width:
            x, y, row_h = 0, y + row_h + gap, 0
        if max_height and y + it.h > max_height and page:
            pages.append((page, y - gap if x == 0 else y + row_h))
            page, x, y, row_h = [], 0, 0, 0
        page.append((it, x, y))
        x += it.w + gap
        row_h = max(row_h, it.h)
    if page:
        pages.append((page, y + row_h))
    return pages


def dedupe(items):
    seen, out = set(), []
    for it in items:
        k = it.key()
        if k not in seen:
            seen.add(k)
            out.append(it)
    return out


# ---------------------------------------------------------------- output


def write_sheet(path_png, placed, scale, colors):
    """A paint sheet (scaled) plus its .tiles sidecar - the same format as an
    F10 capture (Emulator::WriteCapture), at tile sheet size. Returns the
    tiles on it and (pixels the pack colors, pixels shown)."""
    pix = np.zeros((H, W), np.uint8)
    rec = np.zeros((H, W), np.uint64)
    for it, x, y in placed:
        m = it.pix > 0
        pix[y:y + it.h, x:x + it.w][m] = it.pix[m]
        rec[y:y + it.h, x:x + it.w][m] = it.rec[m]
    rgb, colored = colors.paint(pix, rec)
    Image.fromarray(rgb).resize((W * scale, H * scale), Image.NEAREST).save(path_png)

    used = {}
    for it, _, _ in placed:
        for t in it.tile_hashes():
            used[t] = None
    dictionary = bytearray()
    for tile16, (_, h) in TILES.cache.items():
        if h in used and used[h] is None:
            used[h] = tile16
    count = 0
    for h, tile16 in used.items():
        if tile16 is not None:
            dictionary += struct.pack("<I", h) + tile16
            count += 1
    with open(path_png.with_suffix(".tiles"), "wb") as f:
        f.write(SIDECAR_MAGIC + struct.pack("<II", W, H))
        f.write(rec.astype("<u8").tobytes())
        f.write(struct.pack("<I", count) + bytes(dictionary))
        f.write(PALETTE_MAGIC + PALETTE.tobytes() + bytes([255]))
    return set(used), (colored, int((pix > 0).sum()))


# ---------------------------------------------------------------- character sheets

# Where the game draws its players: backgrounds on these layers (the far
# player on 26, the near one on 21 or 22), in palette 0, which the game sets
# up inverted - value 1 is the lightest shade, 3 the darkest.
NEAR_WORLD, FAR_WORLD, PLAYER_PALETTE = 21, 26, 0
SHOWN_ORDER = np.array([0, 3, 2, 1], np.uint8)
SHOWN_MAGIC = b"VBGOSHW1"
FIGURES_MAGIC = b"VBGOFIG1"


def as_player(item, world):
    """The item's records as the game draws a player: background, on `world`, palette 0."""
    rec = item.rec.copy()
    valid = (rec >> np.uint64(63)) & np.uint64(1) == 1
    keep = ~((np.uint64(1) << np.uint64(40)) | (np.uint64(31) << np.uint64(43)) | (np.uint64(3) << np.uint64(38)))
    rec[valid] = (rec[valid] & keep) | (np.uint64(world) << np.uint64(43)) | (np.uint64(PLAYER_PALETTE) << np.uint64(38))
    return Item(item.pix, rec, item.label)


def uncolored_pixels(item, colors):
    if not colors.tiles:
        return int((item.pix > 0).sum())
    n = 0
    ys, xs = np.nonzero(item.pix)
    for y, x in zip(ys.tolist(), xs.tolist()):
        r = int(item.rec[y, x])
        tile = colors.tiles.get(r & 0xFFFFFFFF)
        i = ((r >> 35) & 7) * 8 + ((r >> 32) & 7)
        if tile is None or not (tile[0] >> i & 1):
            n += 1
    return n


def write_character_sheet(path_png, name, near, far, colors, scale, min_uncolored=12):
    """One paint sheet per character: every animation frame the pack doesn't
    fully color yet (near court, then far court), each one a figure of its own
    (VBGOFIG1 - the importer treats each as if it had been captured on a
    screen of its own, so the character's colors stay its own on tiles other
    characters share). Shades are shown as the game shows them on the court;
    what the pack already colors is drawn in its colors, and the sidecar
    records exactly what was shown (VBGOSHW1), so pixels left as they are
    don't count as painted. Returns (frames on the sheet, frames in all)."""
    sections = []
    for title, items, world in (("near court", near, NEAR_WORLD), ("far court", far, FAR_WORLD)):
        todo = [as_player(it, world) for it in dedupe(items) if uncolored_pixels(it, colors) >= min_uncolored]
        if todo:
            sections.append((title, todo))
    if not sections:
        return 0, len(near) + len(far)
    width, gap, label_h, head_h = 640, 6, 8, 14
    for _, todo in sections:   # room for each frame's number under it
        for it in todo:
            it.pix = np.pad(it.pix, ((0, label_h), (0, 0)))
            it.rec = np.pad(it.rec, ((0, label_h), (0, 0)))
    layout, y = [], 4
    for title, todo in sections:
        (placed, height), = pack(todo, width - 8, gap)
        layout.append((title, y, [(it, x + 4, y + head_h + yy) for it, x, yy in placed]))
        y += head_h + height + 12
    h = y
    pix = np.zeros((h, width), np.uint8)
    rec = np.zeros((h, width), np.uint64)
    fig = np.zeros((h, width), np.uint16)
    n = 0
    for _, _, placed in layout:
        for it, x, yy in placed:
            n += 1
            m = it.pix > 0
            pix[yy:yy + it.h, x:x + it.w][m] = it.pix[m]
            rec[yy:yy + it.h, x:x + it.w][m] = it.rec[m]
            fig[yy:yy + it.h, x:x + it.w][m] = n
    shown = PALETTE[SHOWN_ORDER[pix]]
    if colors.tiles:
        ys, xs = np.nonzero(pix)
        for y_, x_ in zip(ys.tolist(), xs.tolist()):
            r = int(rec[y_, x_])
            tile = colors.tiles.get(r & 0xFFFFFFFF)
            i = ((r >> 35) & 7) * 8 + ((r >> 32) & 7)
            if tile is not None and tile[0] >> i & 1:
                shown[y_, x_] = tile[1][i]
    img = Image.fromarray(shown).resize((width * scale, h * scale), Image.NEAREST)
    draw = ImageDraw.Draw(img)
    big, small = font(8 * scale), font(5 * scale)
    draw.text((4 * scale, 2 * scale), name, fill=(255, 200, 120), font=big)
    for title, y0, placed in layout:
        draw.text((80 * scale, y0 * scale), title, fill=(255, 200, 120), font=small)
        for it, x, yy in placed:
            draw.text((x * scale, (yy + it.h - label_h + 1) * scale), it.label.split()[-1], fill=(150, 150, 150), font=small)
    img.save(path_png)

    used = {}
    for _, _, placed in layout:
        for it, _, _ in placed:
            for t in it.tile_hashes():
                used[t] = None
    for tile16, (_, th) in TILES.cache.items():
        if th in used and used[th] is None:
            used[th] = tile16
    dictionary = b"".join(struct.pack("<I", th) + t16 for th, t16 in used.items() if t16 is not None)
    count = sum(1 for t16 in used.values() if t16 is not None)
    with open(path_png.with_suffix(".tiles"), "wb") as f:
        f.write(SIDECAR_MAGIC + struct.pack("<II", width, h))
        f.write(rec.astype("<u8").tobytes())
        f.write(struct.pack("<I", count) + dictionary)
        f.write(PALETTE_MAGIC + PALETTE.tobytes() + bytes([255]))
        f.write(SHOWN_MAGIC + np.ascontiguousarray(shown).tobytes())
        f.write(FIGURES_MAGIC + fig.astype("<u2").tobytes())
    return n, len(dedupe(near)) + len(dedupe(far))


def font(size):
    for name in ("DejaVuSans-Bold.ttf", "arialbd.ttf", "Arial Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("rom", help="Mario's Tennis (Japan, USA).vb")
    ap.add_argument("--out", help='output folder (default: "sprite map" next to the ROM)')
    ap.add_argument("--scale", type=int, default=3, help="paint sheet upscale (default 3, like F10 captures)")
    ap.add_argument("--overview-scale", type=int, default=2, help="sprite map upscale (default 2)")
    ap.add_argument("--pack", help='color pack to pre-color with (default: "<rom>.vbcp" if it exists)')
    ap.add_argument("--no-pack", action="store_true", help="draw everything in the plain palette")
    ap.add_argument("--characters-only", action="store_true",
                    help="only write the character sheets (one per character, what the pack doesn't color yet)")
    args = ap.parse_args()

    rom_path = Path(args.rom)
    rom = Rom(rom_path.read_bytes())
    if not rom.title().startswith("Mario's Tennis"):
        sys.exit("This ROM's header says \"%s\" - this tool only knows Mario's Tennis." % rom.title())
    game = "Mario's Tennis"
    out = Path(args.out) if args.out else rom_path.parent / "sprite map"
    sheets_dir = out / "paint sheets"
    sheets_dir.mkdir(parents=True, exist_ok=True)
    earlier = {p.name for p in sheets_dir.glob("%s [0-9][0-9][0-9] *" % game)}
    started = time.time() - 1

    pack_path = None if args.no_pack else Path(args.pack) if args.pack else rom_path.with_suffix(".vbcp")
    colors = PackColors()
    if pack_path and (args.pack or pack_path.exists()):
        colors = PackColors(pack_path.read_bytes())
        print("Pre-coloring with %s (%d tiles)" % (pack_path.name, len(colors.tiles)))

    print("Decoding characters...")
    chars = characters(rom)

    # ---- character sheets: one per character, only the frames still to paint
    char_dir = out / "character sheets"
    char_dir.mkdir(parents=True, exist_ok=True)
    for name, near, far in chars:
        n, total = write_character_sheet(char_dir / ("%s %s - to paint.png" % (game, name)), name, near, far,
                                         colors, args.scale)
        print("%s: %d of %d frames still to paint" % (name, n, total))
    if args.characters_only:
        return
    print("Decoding screens...")
    scr = screens(rom)
    sets = tile_sets(rom)

    # ---- paint sheets
    sheet_no = 0
    painted_hashes = set()
    coverage = []  # (sheet name, pixels the pack colors, pixels shown)

    def write(path, placed):
        nonlocal painted_hashes
        hashes, (colored, shown) = write_sheet(path, placed, args.scale, colors)
        painted_hashes |= hashes
        coverage.append((path.stem, colored, shown))

    def emit(name, items, gap=3):
        nonlocal sheet_no
        pages = pack(items, W - 2, gap, H - 2)
        for i, (placed, _) in enumerate(pages):
            sheet_no += 1
            suffix = " %d" % (i + 1) if len(pages) > 1 else ""
            path = sheets_dir / ("%s %03d %s%s.png" % (game, sheet_no, name, suffix))
            write(path, [(it, x + 1, y + 1) for it, x, y in placed])
        return len(pages)

    summary = []
    for name, near, far in chars:
        near_u, far_u = dedupe(near), dedupe(far)
        summary.append("%s: %d near-court frames (%d unique), %d far-court frames" % (name, len(near), len(near_u), len(far_u)))
        emit("%s near court" % name, near_u)
        emit("%s far court" % name, far_u)
    for label, item in scr:
        pieces = dedupe(split_objects(item))
        if pieces:
            emit(label, pieces)

    # Tiles not on any sheet yet (beyond the assembled sprites and screens).
    loose, seen = [], set(painted_hashes)
    for label, tiles in sets:
        for t in tiles:
            px, h = TILES.get(t)
            if px.any() and h not in seen:
                seen.add(h)
                loose.append(t)
    per_sheet = ((W - 2) // 9) * ((H - 2) // 9)
    for i in range(0, len(loose), per_sheet):
        item = tile_grid(loose[i:i + per_sheet], (W - 2) // 9)
        sheet_no += 1
        path = sheets_dir / ("%s %03d loose tiles %d.png" % (game, sheet_no, i // per_sheet + 1))
        write(path, [(item, 1, 1)])

    # ---- overview
    s = args.overview_scale
    page_w = 1536
    sections = []  # (title, [(item, x, y)], height)
    for name, near, far in chars:
        for title, items in (("%s - near court" % name, near), ("%s - far court" % name, far)):
            (placed, height), = pack(dedupe(items), page_w, 6)
            sections.append((title, placed, height))
    for label, item in scr:
        c = item.cropped()
        if c is not None:
            if c.w > page_w:
                c = Item(c.pix[:, :page_w], c.rec[:, :page_w], c.label)
            sections.append((label, [(c, 0, 0)], c.h))
    grids = []
    for label, tiles in sets:
        g = tile_grid(tiles, 16)
        g.label = label
        grids.append(g)
    # Tile sets side by side with their labels above them.
    label_h = 14
    for g in grids:
        g.pix = np.pad(g.pix, ((label_h, 0), (0, 0)))
        g.rec = np.pad(g.rec, ((label_h, 0), (0, 0)))
    (placed, height), = pack(grids, page_w, 12)
    sections.append(("Tile sets, in character memory order (16 per row)", placed, height))

    header_h = 22
    total_h = sum(header_h + h + 16 for _, _, h in sections) + 8
    canvas = np.zeros((total_h, page_w), np.uint8)
    canvas_rec = np.zeros((total_h, page_w), np.uint64)
    y = 8
    heads = []
    for title, placed, height in sections:
        heads.append((title, y))
        y += header_h
        for it, x, yy in placed:
            m = it.pix > 0
            canvas[y + yy:y + yy + it.h, x:x + it.w][m] = it.pix[m]
            canvas_rec[y + yy:y + yy + it.h, x:x + it.w][m] = it.rec[m]
        y += height + 16
    img = Image.fromarray(colors.paint(canvas, canvas_rec)[0]).resize((page_w * s, total_h * s), Image.NEAREST)
    draw = ImageDraw.Draw(img)
    big, small = font(9 * s), font(6 * s)
    for (title, yy), (_, placed, _) in zip(heads, sections):
        draw.text((4 * s, yy * s), title, fill=(255, 200, 120), font=big)
        if title.startswith("Tile sets"):  # each grid's label sits in the padding above it
            for it, x, y2 in placed:
                draw.text(((x + 1) * s, (yy + header_h + y2) * s), it.label, fill=(255, 200, 120), font=small)
    overview = out / ("%s - sprite map.png" % game)
    img.save(overview)

    progress = ""
    if colors.tiles:
        done = [name for name, c, n in coverage if n and c == n]
        lines = ["%5.1f%%  %s" % (100.0 * c / n if n else 100.0, name) for name, c, n in coverage]
        progress = PROGRESS % {"pack": pack_path.name, "done": len(done), "sheets": sheet_no, "lines": "\n".join(lines)}
    (out / "README.txt").write_text(README % {"game": game, "sheets": sheet_no} + progress, encoding="utf-8")
    print("\n".join(summary))
    print("%d screens, %d tile sets, %d loose tiles" % (len(scr), len(sets), len(loose)))
    if colors.tiles:
        print("%d of %d sheets already fully colored by the pack (see README.txt)" % (len(done), sheet_no))
    print("Wrote %s and %d paint sheets to %s" % (overview.name, sheet_no, sheets_dir))
    stale = sorted(earlier - {p.name for p in sheets_dir.glob("%s [0-9][0-9][0-9] *" % game)
                              if p.stat().st_mtime >= started})
    if stale:
        print("Left over from an earlier run (not part of this export - delete them): %s" % ", ".join(stale))


README = """%(game)s - full sprite map (exported from the ROM by tools/export_sprite_map.py)

"%(game)s - sprite map.png"
    Everything at a glance: every character's animation frames (near and far
    court), the screens (title, menus, bracket, results, the 7 court
    backgrounds) and every tile set the game loads.

paint sheets/  (%(sheets)d sheets)
    The same graphics as paint-ready pairs, like F6 tile sheets:
      <name>.png    3x sheet in the Ember palette - paint this
      <name>.tiles  which tile and pixel every sheet pixel is - keep it next to the PNG
    Painting rules are the same as for captures (keep the size, paint over the
    pixels, save as PNG; pixels left in the Ember colors don't count).
    Painting a tile once is enough - it gets that color everywhere the game
    draws it. Anything the color pack already colors is drawn in its colors.

    "loose tiles" sheets hold every tile that isn't part of an assembled sprite
    or screen above (mostly font and HUD pieces), so every tile in the ROM can
    be painted somewhere.

To use a painted sheet: copy the PNG and its .tiles file into
roms/colorpacks/Mario's Tennis (Japan, USA)/ and reload the color pack (F11 in
the desktop build, or reload the ROM). Like F6 sheets, they only fill in:
where a screen painting colors a tile, the screen painting wins (it shows the
tile in context). Running the export again replaces the sheets here, so paint
copies (or move painted sheets into the pack folder first).
"""

PROGRESS = """
Progress against %(pack)s: %(done)d of %(sheets)d sheets fully colored already.
Share of each sheet's pixels the pack colors:
%(lines)s
"""


if __name__ == "__main__":
    main()
