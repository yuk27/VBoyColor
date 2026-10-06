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
- `timing.py` - milliseconds per emulated frame for coloring both eyes
  (colorize + paint, as the app's UploadFrame - with the app's frame layout,
  1024 pixels a row; the app logs the same, with the emulation's time, on the
  headset every 250 frames - logcat tag VirtualBoyGo). `VBP_SKEW=bytes` shifts the colored buffer
  against the core's, to see whether their addresses' alignment matters.
