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
  pixel, `render()` both eyes as the app shows them, `worlds()` the VIP's 32
  world attribute blocks, save/load states; `import_folder()` like F11,
  `capture()` / `write_capture()` like F10, `lookup()` a pack's color for a
  tile pixel).
- `worlds.py` - the worlds (layers) a frame is drawn with: which eyes draw
  each one, its type and parallax registers, and for every pair of
  left-only/right-only worlds whether the right one is the same picture
  shifted (exact pixel correspondence) or a different picture (a stereo pair).
- `symmetry.py` - for every right-eye pixel the left eye shows too (the same
  tile pixel on the same row), are the colors identical? And how much of the
  right eye has no such counterpart (per-eye pictures).
- `timing.py` - milliseconds per emulated frame for coloring both eyes.
