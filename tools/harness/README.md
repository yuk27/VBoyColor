# Test harness

The app's emulator core, tile tracker and coloring code (`core/emu/`) built as
a shared library that Python drives through ctypes: run a game headless, read
what drew every pixel, render both eyes exactly as the app paints them, import
paintings into packs, measure eye symmetry and speed. Not part of the app.

```
cmake -S tools/harness -B build/harness -DCMAKE_BUILD_TYPE=Release
cmake --build build/harness -j
cd tools/harness
export VBP_LIB=../../build/harness/libvbp.so   # (the default)
export VBP_STATES=/path/to/states               # save states (not in the repo)
python3 worlds.py ROM.vb [STATE] ["_*60"]       # how the game draws its 3D right now
python3 symmetry.py ROM.vb PACK.vbcp|auto [STATE] [FRAMES] [EVERY]
python3 timing.py ROM.vb PACK.vbcp|auto [STATE] [FRAMES]
python3 tas.py ROM.vb MOVIE.bk2 OUT_DIR [--pack PACK.vbcp] [--all]   # a TAS's run, F7-style paint sheets
python3 library.py ROM.vb MOVIE.bk2 PACK.vbcp LIB.npz       # every tile along the run, with the pack's colors
python3 bgscan.py ROM.vb MOVIE.bk2 BG.npy                   # tiles a background layer draws too
python3 extrapolate.py ROM.vb PACK.vbcp LIB.npz SHEETS OUT --bg BG.npy --design DESIGN.json   # color the sheets
python3 sheetview.py OUT SHEETS [scale] [x0 y0 x1 y1 sheet]  # the result, uncolored in grays, with a grid
python3 compare.py ROM.vb MOVIE.bk2 OLD.vbcp NEW.vbcp OUT EVERY [N]   # the N most changed frames, old | new
```

ROMs, save states, packs and paintings stay out of the repo (they're the
games' content or Juan's work); they live on Juan's PC under `out\`.

- `vbp.cpp` / `vbp.py` - the library and its Python wrapper (`VB`: run with
  buttons, `raw()` frame + shade/brightness tags, `records()` / `tags()` per
  pixel (a sprite pixel's tag carries its OBJ number), `render()` both eyes as
  the app shows them, `paint_ms()` how long coloring them takes, `worlds()` the
  VIP's 32 world attribute blocks, `world_info()` how the renderer classified
  them (per-eye pairs, their disparities), save/load states; `import_folder()`
  like F11, `capture(eye)` / `write_capture()` like F10 (Shift+F10 with
  `right_own=VB.right_own()`: a right-eye capture), `lookup()` a pack's
  color for a tile pixel).
- `worlds.py` - the worlds (layers) a frame is drawn with: which eyes draw
  each one, its type and parallax registers, and for every pair of
  left-only/right-only worlds whether the right one is the same picture
  shifted (exact pixel correspondence) or a different picture (a stereo pair).
- `symmetry.py` - for every right-eye pixel of shared content (layers both
  eyes draw, sprites), is the same point - layer, map cell and tile pixel, or
  OBJ and tile pixel - the same color in the left eye? (0.00% expected.) Per-eye
  pairs apart: their shared tiles, and how much is the right picture's own.
  And per-eye content judged from the pictures alone: each right pixel against
  the left pixel where the left eye's per-eye content best matches the shades
  around it (not 0 - the two pictures are often different drawings - but
  lower is closer).
- `tas.py` - plays a BizHawk movie (`.bk2`, as TASVideos publishes them) with
  the app's core and collects everything the pack doesn't color yet along the
  whole run into paint sheets, like the app's F7 (sprites only unless `--all`).
  Syncs with the core options BizHawk's Mednafen core uses (accurate CPU, both
  directions of a D-pad at once) - `VBP_OPTS` passes core options to the
  library (`key=value,key=value`).
- `library.py`, `bgscan.py`, `extrapolate.py`, `sheetview.py`, `compare.py`,
  `findtiles.py` - coloring a TAS's sheets without painting them by hand:
  colors carried over from the pack's painted tiles (similar tiles - the next
  animation frame - and small gaps), then a design file (rects of the sheets
  with a ramp per object, `"over"` for shared tiles the pack colors otherwise,
  `"skip"` to protect another object's tiles, ramps for whole groups). Tiles a
  background also draws are left alone (a pack colors a tile everywhere).
  The painted sheets import with the pack's paintings like any painting.
  `compare.py` renders the run with both packs and keeps the most changed
  frames; `findtiles.py` finds the frame where a group's tiles first show.
  `designs/` holds the design files the Wario Land, Jack Bros. and
  Teleroboxer packs were made with (TASVideos runs: Wario Land "Best
  Ending", Jack Bros. 5906M, Teleroboxer 3619M; `tas.py` sheets, sprites).
- `timing.py` - milliseconds per emulated frame for coloring both eyes
  (colorize + paint, as the app's UploadFrame - with the app's frame layout,
  1024 pixels a row; the app logs the same, with the emulation's time, on the
  headset every 250 frames - logcat tag VirtualBoyGo). `VBP_SKEW=bytes` shifts the colored buffer
  against the core's, to see whether their addresses' alignment matters.
