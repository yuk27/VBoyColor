# Color packs

A color pack gives every 8x8 tile of a game its own colors, painted by hand
on screenshots. The core records which tile, and which pixel of it, every
screen pixel comes from (`core/emu/vbgo_tiletrack.h`, hooked in through
`cmake/PatchBeetleVip.cmake`), so a painted screenshot can be turned into
tile colors and those colors show wherever the game draws those tiles again.

Packs contain only tile hashes and colors - no game graphics - so they can be
shared freely. ROMs, save states and paintings never go into this repository.

## Desktop tools

In the desktop app:

- **F9** toggles a debug view: one random color per distinct tile.
- **F10** saves the current frame to `roms/captures/` as a paint-ready PNG
  (3x, in the active Multicolor palette - Ember works well - or grayscale)
  plus a `.tiles` file mapping every pixel to its tile, so a painted copy can
  be turned into tile colors. **Shift+F10** does the same for the right eye
  (see "Both eyes" below).
- **F11** re-imports the pack after editing a painting; **F8** toggles the
  pack for comparison.
- **F7** collects what the pack doesn't color yet, **F6** saves tile sheets
  (both below).
- **F12** records gameplay for side-by-side videos (see the main README).

## Making a pack

Put painted copies of captures (same size, pixels not
moved, saved as PNG) next to their `.tiles` files in
`roms/colorpacks/<rom name>/`. Loading the ROM imports them into
`roms/<rom name>.vbcp`: each painted pixel votes for its tile pixel's color,
the majority wins where a tile was painted differently in different places,
and stray near-duplicate shades are merged. The pack then colors every
occurrence of those tiles anywhere in the game in Auto mode or while a
Multicolor palette is active (one made from a gradient too); unpainted
tiles keep the mode's colors. Tint and Gradient modes never show a pack -
they only tint the game's shades.

## Built-in packs, and using a pack elsewhere

The packs in the repository's `colorpacks/` folder are built into every
build: the Quest app bundles them, and the desktop builds copy them next to
the executable (`colorpacks/`, with `index.txt`). To ship a new or updated
pack, copy its `.vbcp` from the desktop app's `roms` folder into
`colorpacks/` and commit it - only `.vbcp` files, never ROMs, paintings or
save states.

The `.vbcp` file alone is enough on other platforms: copy it next to the ROM
(on the Quest too) and it wins over the built-in one. For your own Quest
builds, `colorpacks.dir` in `android/local.properties` can name a folder of
`.vbcp` files (the desktop app's `roms` folder, say, with forward slashes:
`colorpacks.dir=C:/path/to/roms`); they're bundled too, replacing the
repository's packs of the same name. A pack records the ROM it was made for
(CRC-32 and size), so the game finds it whatever its ROM file is called: next
to the ROM by name first, then built in by name, then the built-in pack made
for this ROM (`colorpacks/index.txt`), else - on desktop - one in the `roms`
folder. Packs made before this get their
ROM recorded the first time the game loads them by name, or with
`tools/vbcp_rom.py PACK.vbcp ROM.vb`.

## Both eyes

Packs are painted from the left eye, and both eyes always show
the same thing in the same colors: a pixel's color depends only on what both
eyes share, never on a search of the other eye. Layers both eyes draw and
every sprite both eyes show are the same tile pixels in both eyes, so they're
colored the same by construction. Some games draw a picture per eye instead -
a left-only layer next to a right-only one (Mario Clash's stage, Wario Land's
title, Galactic Pinball's tables - some draw a few left layers, then their
right partners in the same order), or sprites only one eye shows (much of a
Galactic Pinball table). The right picture then takes its left partner's
colors: tiles both pictures use by tile (and a map cell's own colors by the
left picture's cell with that tile pixel); the right picture's own tiles
from the left picture where they line up - a disparity per 8x8 block, worked
out from the two pictures when they change (things at very different depths
side by side each get their own), and the color of the nearest left-picture
pixel of the same shade there - so the game's dither stays its own and
nothing flickers. To paint the right eye's own pictures yourself,
**Shift+F10** captures the right eye: only its own pictures count there, and
what you paint on them overrides what they'd take from the left (the left
eye's colors stay the left paintings'). Unpainted, they keep following the
left paintings.

## Painting rules

Only what you change counts: pixels left in the capture's
own colors are ignored, so a capture can be painted a bit at a time. Paint
**magenta (#FF00FF)** over anything that should keep the Multicolor palette's
colors - it overrides whatever else (a tile sheet, say) would color those
tiles. Paint what you want to see: captures record the game's brightness, so
a game that normally runs dimmed (Mario's Tennis runs at about 90%) shows
exactly the painted colors, and fades follow the game from there.

## Same tile, different colors

Games reuse tiles, so a tile's colors are
looked up from the most specific to the most general, each only where the
paintings consistently ask for it:

1. *Map cell* - a background tile at a fixed spot painted differently there
   than elsewhere (the same letter yellow in one heading and white in the
   next, a selected menu entry) keeps that spot's colors.
2. *Palette* - the game shows a tile in another palette to mark something (an
   option that isn't selected), and that palette is painted differently.
3. *Layer* - a tile reused on another layer (a cloud tile inside a mountain).
4. The tile's own colors.

## Same tile, different characters

Games also reuse the very same sprite
tiles in different characters (Jack Bros.' Lantern and Skelton share their hat
brims). When sprites painted differently share a tile, the importer finds
tiles only one of them has (Skelton's skull - "markers"), and the shared tile
takes that character's colors wherever one of its markers was drawn nearby in
the last frame - so both can even stand side by side. Nothing to do: it comes
from the paintings (a sprite is a connected group of sprite pixels). A
sprite's colors only ever go to sprites, never to a background drawn nearby
(a dialog box behind them). Players a game draws as backgrounds (Mario's
Tennis) count too: the one figure on a background layer that holds nothing
else, away from the screen's sides - with markers that turn up nowhere but in
its own frames, and that only count on the layer they're drawn on, so the
portraits next to each other on a select screen keep their own colors.

## Painting the background

Transparent parts of background tiles can be
painted too ("fills" - a court's surface between its speckles, a sky behind a
logo): they get that color at that spot of the map, even where the game
scales or scrolls the layer. A fill only needs to be painted once per spot;
pixels another capture shows that the paintings don't are filled in from
their nearest painted neighbor in the tile.

## Finding what's still uncolored

In the desktop app, **F7** starts
collecting: play normally, and every object the pack doesn't color yet (a new
enemy, another pose of a character, new scenery) is gathered once, cropped from
the frame it appeared in. Press **F7** again (or switch ROMs / quit) to save
them as compact paint sheets, `roms/captures/<rom name> todo NNN.png` plus
`.tiles` - colored parts in their pack colors, the rest in the Multicolor
palette. Paint the rest, drop the sheet into the pack folder like any other
painting, and repeat until nothing new turns up. A whole new area is kept as a
full screen instead of pieces, so it's easier to recognize. Whatever is left
the way it was captured - Multicolor shades or the pack's colors - doesn't
count as painted (in any capture, F10 and F6 too), so a wrong color the pack
showed (another sprite's color on a shared tile) only goes away by painting
over it, but leaving it doesn't paint it in for good. Captures from builds
before this didn't record what they showed: there, every pixel that isn't a
Multicolor shade counts as painted.

## Tile sheets

**F6** saves everything in the game's tile memory at that
moment - the graphics loaded for the current area, on screen or not - as
`roms/captures/<rom name> tiles NNN.png` + `.tiles`, laid out 32 tiles wide
like a tile viewer (tiles the pack already colors in its colors). Paint it and
drop it into the pack folder: it fills in every tile pixel the screen
paintings don't color (screen paintings always win, since they show tiles in
context). Paintings and sheets may be resized by an editor as long as the
shape stays the same. Games load some graphics only while they're shown (Wario
Land streams Wario's poses, for example), so sheets and F7 complement each
other: a sheet per area for most things, F7 for what only appears briefly.

## Cost

Tile tracking only runs while something uses it: Auto mode, a
Multicolor palette with a pack, or (desktop) the capture tools and debug
view. The core tags each pixel with one 64-bit store while it draws (tile,
pixel, palette, layer and map cell together - a whole tile row at once where
it can), and the pack is painted from those tags with a few table reads per
pixel - a tile's colors are looked up when its graphics change, not per pixel.
Measured on one desktop core (2.1 GHz Xeon, optimized build), against the
core's own ~2-2.7 ms per emulated frame: tracking adds ~0.4-0.8 ms (most in
games that draw many layers over each other, like Jack Bros.), and coloring
both eyes takes ~0.8-1.9 ms (most where each eye's picture is drawn
separately - Galactic Pinball, Mario Clash - which is matched by looks once
per picture and remembered). The app logs both every 250 frames (logcat tag
`VBoyColor` on the headset: emulation, coloring, and how often it fell
behind). Output is otherwise identical.

## Formats

`.tiles` version 2 (`VBGOTIL2`) adds the map cell of every
pixel after the per-pixel records (version 1 files from older captures still
import, just without cell colors or fills), and the palette block
(`VBGOPAL2`) adds the game's brightness level; newer captures follow it
with what every pixel showed (`VBGOSHW1`, RGB at 1x); character sheets add a
figure id per pixel (`VBGOFIG1`, 16 bit). `.vbcp` version 3 adds the
palette and map cell colors and the reference brightness, optionally followed
by the per-character colors (`VBGOCTX1`, then `VBGOCTXL` marking the
background figures' ones - older builds skip both) and the tiles a
character sheet paints two ways in a frame (`VBGOAMB1`), and ends with the
ROM it was made for (`VBGOROM1`, CRC-32, size - always the last 16 bytes);
older packs load as before.

## Tools

A loose mockup that doesn't line up with a capture (different size, shifted,
partly redrawn) can be turned into a painting with
`tools/mockup_to_painting.py` (numpy + Pillow): it fits the mockup onto the
capture's pixels layer by layer, gives every tile pixel the color it shows most
often, and writes a 3x painting to touch up and drop into the pack folder. It's
pixel-exact when the capture shows the same moment as the mockup.

`tools/export_sprite_map.py` (numpy + Pillow) does the same as F6 for
Mario's Tennis straight from the ROM, which keeps nearly all of its graphics
compressed: it decodes every character's animation frames, the screens and
every tile set into an overview plus paint sheets (tile sheets - they fill in
what the screen paintings don't), drawing whatever the game's `.vbcp`
already colors in its colors so what's left stands out. It also writes one
**character sheet** per player ("character sheets/"): every animation frame
the pack doesn't fully color yet, near and far court, in the shades the court
shows them. Each frame on it counts as a figure of its own - as if each had
been captured on a screen by itself - and the frames of one sheet are one
character, so the colors painted there stay that character's even on tiles
the players share (`--characters-only` writes just these; `--all-frames`
puts every frame on them, colored or not). For their tiles a character's
sheet decides: no other character's tiles switch its colors on, and what
screen paintings show of those tiles at a map cell or on a layer makes no
cell or layer colors (players move through the map and swap layers with the
side of the court). A tile a sheet paints two ways within one frame (a plain
filled tile: a shirt's white, a cap's green) takes, pixel by pixel, the color
the same shade has right next to it on screen. Everything on a character
sheet counts as painted, colors left as the pack showed them too (paint over
wrong ones).
