"""Color what a TAS's paint sheets (tas.py) left uncolored, from what the pack already colors plus a design.

    python3 extrapolate.py ROM PACK LIB.npz SHEET_DIR OUT_DIR [--bg BG.npy] [--design DESIGN.json] [--view 0,3]

LIB.npz: library.py's (every tile's pattern and the pack's colors along the run); BG.npy: bgscan.py's (tiles a
background layer draws too - never painted here: a pack colors a tile everywhere it shows, so painting a sprite's
tile that a wall shares would color the wall).
1. Similar tiles: an uncolored sprite tile takes the colors of the most similar tile the pack colors (same pixel
   values at >= 80% of their pixels, allowing a 1-pixel shift and flips) - the next animation frame. Not for
   near-solid tiles (they all look alike). --src-bg: from painted background tiles too.
2. Small gaps: a few uncolored pixels inside colored ones take their neighbours' colors.
3. Designs (DESIGN.json; coordinates are the sheets' own pixels, x1/y1 exclusive):
     "ramps": {"name": ["#pix1", "#pix2", "#pix3"]}            colors by the tile's pixel value (dark..light)
     "rects": [{"s": sheet, "r": [x0, y0, x1, y1], "p": ramp}]  uncolored pixels inside; later rects win.
        "over": true - also repaint what the pack colors inside (a tile shared with something else, like a
        solid fill that's a coin's elsewhere); those pixels change wherever the object's own tiles show within
        24 px (the app tells shared tiles apart the same way - context variants).
        "p": "skip" - protects the tiles inside from every other rect (another object in the way).
     "fills": [{"s": sheet, "at": [x, y], "c": "#rrggbb" or ramp}]  paint bucket (same pixel value)
     "groups": {"<tile hash hex>": {"p": ramp}}                 what's left of the group with that tile
   A ramp is a name, a list of 3 colors or one color. A tile pixel painted once takes that color wherever the
   tile shows; then step 1 again from the painted tiles (their other frames).
Writes each sheet painted ("tas auto", same .tiles - import them with the pack's paintings), state.npz (where
each pixel's color came from, for sheetview.py), groups.json + contact sheets of what's left, and with --view
close-ups of those groups with the sheet's coordinates.
"""
import argparse, glob, json, os, struct, sys
os.environ.setdefault("VBP_OPTS", "vb_opposite_directions=enabled,vb_cpu_emulation=accurate")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage
from vbp import VB, lookup, U

ap = argparse.ArgumentParser()
ap.add_argument("rom"); ap.add_argument("pack"); ap.add_argument("lib"); ap.add_argument("sheets"); ap.add_argument("out")
ap.add_argument("--design"); ap.add_argument("--min-sim", type=float, default=0.8)
ap.add_argument("--src-bg", action="store_true", help="step 1 also from painted background tiles")
ap.add_argument("--bg", help="npy of tile hashes also drawn as background (bgscan.py): never painted here")
ap.add_argument("--view", default=""); ap.add_argument("--view-all-left", type=int, default=0)
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
vb = VB(a.rom, pack=a.pack)
L = dict(np.load(a.lib))
lib_index = {int(h): i for i, h in enumerate(L["hashes"])}
design = json.load(open(a.design)) if a.design and os.path.exists(a.design) else {}
try:
    font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 14)
    small = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 11)
except Exception:
    font = small = ImageFont.load_default()

# --- the sheets -------------------------------------------------------------------------------------
sheets = []
for png in sorted(glob.glob(os.path.join(a.sheets, "*.png"))):
    side = png[:-4] + ".tiles"
    b = open(side, "rb").read()
    w, h = struct.unpack_from("<II", b, 8)
    t = np.frombuffer(b, dtype="<u8", count=w * h, offset=16).reshape(h, w).copy()
    shown = np.asarray(Image.open(png).convert("RGB"))[1::3, 1::3].copy()
    sheets.append(dict(png=png, side=side, t=t, shown=shown))

cache = {}
def pack_color(r):
    k = int(r) & ~(0x7FF << 48)
    if k not in cache:
        cache[k] = lookup(r)
    return cache[k]

# src: 0 not colored, 1 pack, 2 similar tile, 3 small gap, 4 design, 5 similar to a designed tile, 6 group ramp
for s in sheets:
    s["own"] = np.full(s["t"].shape, -1, np.int32)
for s in sheets:
    t = s["t"]; h, w = t.shape
    valid = (t >> U(63)) == U(1)
    col = np.full((h, w, 3), -1, np.int16)
    need = np.zeros((h, w), bool)
    src = np.zeros((h, w), np.uint8)
    for y, x in zip(*np.nonzero(valid)):
        c = pack_color(t[y, x])
        if c is None:
            need[y, x] = True
        elif c != "keep":
            col[y, x] = c; src[y, x] = 1
    s["valid"], s["col"], s["need"], s["src"] = valid, col, need, src
    s["hash"] = (t & U(0xFFFFFFFF)).astype(np.int64)
    s["pos"] = (((t >> U(35)) & U(7)) * 8 + ((t >> U(32)) & U(7))).astype(np.int64)
    s["pix"] = ((t >> U(41)) & U(3)).astype(np.int64)
    s["obj"] = ((t >> U(40)) & U(1)).astype(bool)
bgset = np.load(a.bg) if a.bg else np.zeros(0, np.int64)
n_bg = 0
for s in sheets:
    s["isbg"] = np.isin(s["hash"], bgset) & s["valid"]
    n_bg += int((s["need"] & s["isbg"]).sum())
    s["need"] &= ~s["isbg"]
if a.bg:
    print("left alone (tiles the backgrounds use too):", n_bg, "pixels")
total_need = sum(int(s["need"].sum()) for s in sheets)
print("uncolored pixels on the sheets:", total_need)

def left():
    return sum(int(s["need"].sum()) for s in sheets)

# --- 1. similar tiles -------------------------------------------------------------------------------
def variants(sources):
    """sources: [(pattern 8x8, colors 8x8x3)] -> arrays of every flip/shift."""
    vp, vc, vs = [], [], []
    for i, (p, c) in enumerate(sources):
        for flip in range(4):
            pp = p[:, ::-1] if flip & 1 else p
            cc = c[:, ::-1] if flip & 1 else c
            pp = pp[::-1] if flip & 2 else pp
            cc = cc[::-1] if flip & 2 else cc
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    sp = np.zeros((8, 8), np.uint8); sc = np.full((8, 8, 3), -1, np.int16)
                    ys = slice(max(dy, 0), 8 + min(dy, 0)); yd = slice(max(-dy, 0), 8 + min(-dy, 0))
                    xs = slice(max(dx, 0), 8 + min(dx, 0)); xd = slice(max(-dx, 0), 8 + min(-dx, 0))
                    sp[ys, xs] = pp[yd, xd]; sc[ys, xs] = cc[yd, xd]
                    vp.append(sp.reshape(64)); vc.append(sc.reshape(64, 3)); vs.append(i)
    return np.array(vp), np.array(vc), np.array(vs)

def low_info(p):
    nz = p[p != 0]
    return len(nz) < 6 or np.bincount(nz, minlength=4).max() >= 52

def similar(sources, source_ids, srcmark):
    """sources: [(hash, pattern, colors)]; colors uncolored sheet pixels of similar tiles."""
    if not sources:
        return 0, 0
    VP, VC, VS = variants([(p, c) for _, p, c in sources])
    VNZ = VP != 0
    own = {h: k for k, (h, _, _) in enumerate(sources)}
    targets = set()
    for s in sheets:
        targets.update(np.unique(s["hash"][s["need"] & s["obj"]]).tolist())
    transfer = {}
    for hsh in targets:
        i = lib_index.get(hsh)
        if i is None or not L["has"][i]:
            continue
        tp = L["pat"][i].reshape(64)
        if low_info(L["pat"][i]):
            continue
        tnz = tp != 0
        match = ((VP == tp) & VNZ & tnz).sum(1)
        union = (VNZ | tnz).sum(1)
        sim = match / np.maximum(union, 1)
        if hsh in own:
            sim[VS == own[hsh]] = 0
        best = int(np.argmax(sim))
        if sim[best] < a.min_sim or match[best] < 12:
            continue
        c = np.full((64, 3), -1, np.int16)
        ok = (VP[best] == tp) & tnz & (VC[best][:, 0] >= 0)
        c[ok] = VC[best][ok]
        transfer[hsh] = c
    n = 0
    for s in sheets:
        for y, x in zip(*np.nonzero(s["need"])):
            c = transfer.get(int(s["hash"][y, x]))
            if c is not None and c[s["pos"][y, x], 0] >= 0:
                s["col"][y, x] = c[s["pos"][y, x]]; s["need"][y, x] = False; s["src"][y, x] = srcmark; n += 1
    return len(transfer), n

painted = np.array([(L["col"][i] >= 0).any() and L["has"][i] for i in range(len(L["hashes"]))])
cand = np.nonzero(painted & (L["isobj"] | a.src_bg))[0]
print("similar tiles: %d tiles, %d pixels" % similar([(int(L["hashes"][i]), L["pat"][i], L["col"][i]) for i in cand],
                                                      None, 2))

# --- 2. small gaps ------------------------------------------------------------------------------------
def small_gaps(limit=10):
    filled = 0
    for s in sheets:
        lab, n = ndimage.label(s["need"])
        for k, sl in enumerate(ndimage.find_objects(lab), 1):
            comp = lab[sl] == k
            if comp.sum() > limit:
                continue
            ys, xs = np.nonzero(comp); ys = ys + sl[0].start; xs = xs + sl[1].start
            for y, x in zip(ys, xs):
                votes = {}
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        yy, xx = y + dy, x + dx
                        if 0 <= yy < s["t"].shape[0] and 0 <= xx < s["t"].shape[1] and s["col"][yy, xx, 0] >= 0 \
                                and s["valid"][yy, xx]:
                            same = s["pix"][yy, xx] == s["pix"][y, x]
                            key = tuple(int(v) for v in s["col"][yy, xx])
                            votes[key] = votes.get(key, 0) + (3 if same else 1)
                if votes:
                    s["col"][y, x] = max(votes.items(), key=lambda kv: kv[1])[0]; s["need"][y, x] = False
                    s["src"][y, x] = 3; filled += 1
    return filled
print("small gaps:", small_gaps(), "pixels")
after_auto = left()

# --- 3. designs -------------------------------------------------------------------------------------------
def hexrgb(v):
    v = v.lstrip("#"); return tuple(int(v[i:i + 2], 16) for i in (0, 2, 4))
ramps = design.get("ramps", {})
def ramp(p):
    if isinstance(p, str) and p in ramps:
        p = ramps[p]
    if isinstance(p, str):
        c = np.array(hexrgb(p), float)
        return [tuple(int(v) for v in c * 0.55), tuple(int(v) for v in c),
                tuple(int(v) for v in np.minimum(c + (255 - c) * 0.45, 255))]
    return [hexrgb(v) if isinstance(v, str) else tuple(v) for v in p]

override = {}  # (hash, pos) -> rgb
owner = {}     # (hash, pos) -> rect index (fills: 10000 + index)
# "p": "skip" rects protect what's uncolored inside them from every other rect (another object's tiles)
protect = set()
for r in design.get("rects", []):
    if r["p"] != "skip":
        continue
    s = sheets[r["s"]]; x0, y0, x1, y1 = r["r"]
    for py, px in zip(*np.nonzero(s["valid"][y0:y1, x0:x1])):
        protect.add((int(s["hash"][y0 + py, x0 + px]), int(s["pos"][y0 + py, x0 + px])))
if protect:  # (left as they are by every later step too)
    for s in sheets:
        for y, x in zip(*np.nonzero(s["need"])):
            if (int(s["hash"][y, x]), int(s["pos"][y, x])) in protect:
                s["need"][y, x] = False
for ri, r in enumerate(design.get("rects", [])):
    if r["p"] == "skip":
        continue
    s = sheets[r["s"]]; x0, y0, x1, y1 = r["r"]; rp = ramp(r["p"])
    sub = (slice(y0, y1), slice(x0, x1))
    m = s["need"][sub] & s["valid"][sub]
    for py, px in zip(*np.nonzero(m)):
        Y, X = y0 + py, x0 + px
        v = int(s["pix"][Y, X])
        if 1 <= v <= 3 and (int(s["hash"][Y, X]), int(s["pos"][Y, X])) not in protect:
            override[(int(s["hash"][Y, X]), int(s["pos"][Y, X]))] = rp[v - 1]
            owner[(int(s["hash"][Y, X]), int(s["pos"][Y, X]))] = ri
for f in design.get("fills", []):
    s = sheets[f["s"]]; x, y = f["at"]
    v = int(s["pix"][y, x])
    lab, _ = ndimage.label(s["need"] & s["valid"] & (s["pix"] == v))
    k = lab[y, x]
    if k == 0:
        print("fill at", f["s"], (x, y), "isn't an uncolored pixel"); continue
    c = f["c"]
    c = hexrgb(c) if isinstance(c, str) and c.startswith("#") else ramp(c)[v - 1]
    for py, px in zip(*np.nonzero(lab == k)):
        override[(int(s["hash"][py, px]), int(s["pos"][py, px]))] = c
# "over": true - also repaint what the pack (or steps 1-2) colored inside the rect: tiles shared with
# something else (a solid fill that's a coin's elsewhere). Those take the new colors wherever the object's
# own tiles (the rect's other tiles) show within 24 px - the way the app tells shared tiles apart (context).
n_over = 0
for r in design.get("rects", []):
    if not r.get("over") or r["p"] == "skip":
        continue
    s = sheets[r["s"]]; x0, y0, x1, y1 = r["r"]; rp = ramp(r["p"])
    sub = (slice(y0, y1), slice(x0, x1))
    m = s["valid"][sub] & np.isin(s["src"][sub], (1, 2, 3)) & (s["pix"][sub] > 0) & ~s["isbg"][sub]
    keys = {}
    for py, px in zip(*np.nonzero(m)):
        Y, X = y0 + py, x0 + px
        if (int(s["hash"][Y, X]), int(s["pos"][Y, X])) not in protect:
            keys[(int(s["hash"][Y, X]), int(s["pos"][Y, X]))] = rp[int(s["pix"][Y, X]) - 1]
    over_tiles = set(k[0] for k in keys)
    protected_tiles = set(k[0] for k in protect)
    own_m = s["valid"][sub] & (s["pix"][sub] > 0) & (s["src"][sub] != 1)  # the object's own (unpainted) tiles
    markers = np.array(sorted(set(np.unique(s["hash"][sub][own_m]).tolist()) - over_tiles - protected_tiles))
    for s2 in sheets:
        near = ndimage.maximum_filter(np.isin(s2["hash"], markers) & s2["valid"], size=49)
        cand2 = near & s2["valid"] & np.isin(s2["src"], (1, 2, 3)) & np.isin(s2["hash"], list(over_tiles)) & ~s2["isbg"]
        for y, x in zip(*np.nonzero(cand2)):
            c = keys.get((int(s2["hash"][y, x]), int(s2["pos"][y, x])))
            if c is not None:
                s2["col"][y, x] = c; s2["src"][y, x] = 4; n_over += 1
if n_over:
    print("repainted (over):", n_over, "pixels")
n4 = 0
for s in sheets:
    for y, x in zip(*np.nonzero(s["need"])):
        o = override.get((int(s["hash"][y, x]), int(s["pos"][y, x])))
        if o is not None:
            s["col"][y, x] = o; s["need"][y, x] = False; s["src"][y, x] = 4; n4 += 1
            s["own"][y, x] = owner.get((int(s["hash"][y, x]), int(s["pos"][y, x])), -1)
print("designed:", n4, "pixels")
# their other frames: similar tiles, from the designed ones (with what the pack colors on the same tiles)
if override:
    by_tile = {}
    for (hsh, pos), c in override.items():
        by_tile.setdefault(hsh, {})[pos] = c
    srcs = []
    for hsh, d in by_tile.items():
        i = lib_index.get(hsh)
        if i is None or not L["has"][i]:
            continue
        c = L["col"][i].copy()
        for pos, rgb in d.items():
            c[pos // 8, pos % 8] = rgb
        srcs.append((hsh, L["pat"][i], c))
    print("similar to designed tiles: %d tiles, %d pixels" % similar(srcs, None, 5))

# --- groups of what's left ---------------------------------------------------------------------------
def make_groups():
    crops = []
    for si, s in enumerate(sheets):
        lab, n = ndimage.label(ndimage.binary_dilation(s["need"], iterations=1, structure=np.ones((3, 3))))
        for k, sl in enumerate(ndimage.find_objects(lab), 1):
            m = (lab[sl] == k) & s["need"][sl]
            if m.sum() == 0:
                continue
            hs = np.unique(s["hash"][sl][m]).tolist()
            crops.append(dict(sheet=si, sl=sl, mask=m, need=int(m.sum()), tiles=hs))
    uses = {}
    for c in crops:
        for h2 in c["tiles"]:
            uses[h2] = uses.get(h2, 0) + 1
    parent = {}
    def find(x):
        while parent.setdefault(x, x) != x:
            parent[x] = parent[parent[x]]; x = parent[x]
        return x
    for c in crops:
        hs = [h2 for h2 in c["tiles"] if uses[h2] <= 12] or c["tiles"][:1]
        for h2 in hs[1:]:
            parent[find(h2)] = find(hs[0])
        c["key"] = hs[0]
    groups = {}
    for ci, c in enumerate(crops):
        groups.setdefault(find(c["key"]), []).append(ci)
    order = sorted(groups, key=lambda g: -sum(crops[ci]["need"] for ci in groups[g]))
    out = []
    for g in order:
        cis = groups[g]
        tiles = sorted(set(h2 for ci in cis for h2 in crops[ci]["tiles"]))
        rep = crops[max(cis, key=lambda ci: crops[ci]["need"])]
        out.append(dict(crops=[crops[ci] for ci in cis], tiles=tiles, rep=rep,
                        need=sum(crops[ci]["need"] for ci in cis)))
    return out

gdesign = {int(k, 16): v for k, v in design.get("groups", {}).items()}
if gdesign:
    n6 = 0
    for g in make_groups():
        d = next((gdesign[h2] for h2 in g["tiles"] if h2 in gdesign), None)
        if not d or d.get("skip") or "p" not in d:
            continue
        rp = ramp(d["p"])
        for c in g["crops"]:
            s = sheets[c["sheet"]]; sl = c["sl"]
            for py, px in zip(*np.nonzero(c["mask"])):
                Y, X = sl[0].start + py, sl[1].start + px
                v = int(s["pix"][Y, X])
                if 1 <= v <= 3 and s["need"][Y, X]:
                    s["col"][Y, X] = rp[v - 1]; s["need"][Y, X] = False; s["src"][Y, X] = 6; n6 += 1
    print("group ramps:", n6, "pixels")
print("small gaps:", small_gaps(), "pixels")
print("left: %d of %d (auto steps left %d)" % (left(), total_need, after_auto))

# --- outputs ------------------------------------------------------------------------------------------
for s in sheets:
    img = s["shown"].copy()
    m = s["col"][..., 0] >= 0
    img[m] = s["col"][m].astype(np.uint8)
    base = os.path.join(a.out, os.path.basename(s["png"])[:-4].replace(" tas todo ", " tas auto "))
    Image.fromarray(np.repeat(np.repeat(img, 3, 0), 3, 1)).save(base + ".png")
    open(base + ".tiles", "wb").write(open(s["side"], "rb").read())
np.savez_compressed(os.path.join(a.out, "state.npz"), **{"src%d" % i: s["src"] for i, s in enumerate(sheets)},
                    **{"need%d" % i: s["need"] for i, s in enumerate(sheets)},
                    **{"own%d" % i: s["own"] for i, s in enumerate(sheets)})

skipped = set(h2 for h2, d in gdesign.items() if d.get("skip"))
G = [g for g in make_groups() if not (set(g["tiles"]) & skipped)]
GRAY = {1: (90, 90, 110), 2: (160, 160, 180), 3: (235, 235, 245)}
def crop_img(s, sl, margin=4, mark_src=False):
    sl = (slice(max(sl[0].start - margin, 0), min(sl[0].stop + margin, s["t"].shape[0])),
          slice(max(sl[1].start - margin, 0), min(sl[1].stop + margin, s["t"].shape[1])))
    img = np.zeros((sl[0].stop - sl[0].start, sl[1].stop - sl[1].start, 3), np.uint8) + 20
    valid = s["valid"][sl]; col = s["col"][sl]; need = s["need"][sl]; pix = s["pix"][sl]
    pm = valid & (col[..., 0] >= 0)
    img[pm] = col[pm]
    for v, c in GRAY.items():
        img[valid & need & (pix == v)] = c
    return sl, img
info = []
for n, g in enumerate(G):
    rep = g["rep"]; s = sheets[rep["sheet"]]
    sl, _ = crop_img(s, rep["sl"])
    info.append(dict(group=n, tile="%08x" % g["rep"]["tiles"][0], crops=len(g["crops"]), tiles=len(g["tiles"]),
                     need=g["need"], s=rep["sheet"], r=[sl[1].start, sl[0].start, sl[1].stop, sl[0].stop]))
json.dump(info, open(os.path.join(a.out, "groups.json"), "w"), indent=1)
page, x, y, rowh, W = 0, 0, 0, 0, 1400
canvas = Image.new("RGB", (W, 1000), (12, 12, 16)); draw = ImageDraw.Draw(canvas)
def flush():
    global canvas, draw, page
    canvas.crop((0, 0, W, y + rowh + 4)).save(os.path.join(a.out, "groups_%02d.png" % page)); page += 1
    canvas = Image.new("RGB", (W, 1000), (12, 12, 16)); draw = ImageDraw.Draw(canvas)
for n, g in enumerate(G):
    s = sheets[g["rep"]["sheet"]]
    _, img = crop_img(s, g["rep"]["sl"])
    sc = 4 if max(img.shape[:2]) <= 48 else 3 if max(img.shape[:2]) <= 80 else 2
    im = Image.fromarray(img).resize((img.shape[1] * sc, img.shape[0] * sc), Image.NEAREST)
    if x + im.width + 8 > W:
        x = 0; y += rowh + 6; rowh = 0
    if y + im.height + 20 > 1000:
        flush(); x = y = rowh = 0
    canvas.paste(im, (x, y + 18)); draw.text((x + 2, y), "%d" % n, fill=(255, 210, 80), font=font)
    x += im.width + 10; rowh = max(rowh, im.height + 18)
if G:
    flush()
print(len(G), "groups left; contact sheets:", page)

# --- close-ups: a group's representative part, 8x, the sheet's coordinates (grid every 8 px) ------------
def closeup(n, g, path, scale=8):
    s = sheets[g["rep"]["sheet"]]
    sl, img = crop_img(s, g["rep"]["sl"], margin=2)
    x0, y0 = sl[1].start, sl[0].start
    h, w = img.shape[:2]
    big = Image.fromarray(img).resize((w * scale, h * scale), Image.NEAREST)
    pad = 34
    canvas = Image.new("RGB", (w * scale + pad, h * scale + pad + 18), (12, 12, 16))
    canvas.paste(big, (pad, pad + 18))
    d = ImageDraw.Draw(canvas)
    d.text((2, 0), "group %d  sheet %d  need %d" % (n, g["rep"]["sheet"], g["need"]), fill=(255, 210, 80), font=font)
    for X in range(x0, x0 + w + 1):
        if X % 4 == 0:
            px = pad + (X - x0) * scale
            d.line([(px, pad + 18), (px, pad + 18 + h * scale)], fill=(70, 70, 200) if X % 8 else (200, 60, 60), width=1)
            if X % 8 == 0:
                d.text((px + 1, 18), str(X), fill=(255, 255, 255), font=small)
    for Y in range(y0, y0 + h + 1):
        if Y % 4 == 0:
            py = pad + 18 + (Y - y0) * scale
            d.line([(pad, py), (pad + w * scale, py)], fill=(70, 70, 200) if Y % 8 else (200, 60, 60), width=1)
            if Y % 8 == 0:
                d.text((0, py + 1), str(Y), fill=(255, 255, 255), font=small)
    canvas.save(path)
views = [int(v) for v in a.view.split(",") if v.strip()]
if a.view_all_left:
    views = list(range(min(a.view_all_left, len(G))))
for n in views:
    if n < len(G):
        sc = 8 if max(G[n]["rep"]["mask"].shape) <= 70 else 6 if max(G[n]["rep"]["mask"].shape) <= 100 else 5
        closeup(n, G[n], os.path.join(a.out, "view_%03d.png" % n), sc)
