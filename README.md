# VirtualBoyGo

VirtualBoyGo is a Virtual Boy emulator for VR headsets, built on **OpenXR +
Vulkan** as a single app sharing one Vulkan rendering codebase. This is a
from-scratch rework of the original VirtualBoyGo.

Currently supported: **Quest** (Android), **Desktop VR** (PC, streamed to a
headset via Virtual Desktop/SteamVR/Link), and **Desktop 2D** (PC, no headset
needed, for fast local iteration). Other Android VR headsets (Frame, etc.)
are planned for the future.

|   |   |
|---|---|
| ![](images/0.png) | ![](images/1.png) |
| ![](images/2.png) | ![](images/3.png) |

## Features

- In-VR menu for ROM selection, settings, and button mapping
- Save states (multiple slots, with preview thumbnails)
- Adjustable screen placement/size in the VR view
- Configurable VB screen color palette, including a custom R/G/B tint
- Per-shade color palettes (Red Viper-style colorization): each of the
  Virtual Boy's 4 shades gets its own color, so a game's layers keep their
  own hues through fades - Settings → Color Mode → Multicolor, then pick a
  palette with Color Palette. The gradient palettes (Jade, Ocean, Sunset,
  Ember, Frost, Toxic) use all 5 of their colors: each shade takes the
  gradient's color at the brightness the game gives it - what Gradient mode
  shows at the game's full brightness - and keeps it through fades
- **Auto** colors (Settings → Color Mode → Auto, the default): coloring by
  shade can only show 4 colors at once, because the Virtual Boy only outputs
  4 shades. Auto colors each pixel by what drew it instead - a background
  layer by its depth in the game's drawing order (far to near: indigo, teal,
  green, sand), a sprite by its palette (red/yellow for most characters) - and
  a small layer some games draw a character on (Mario's Tennis's players,
  Teleroboxer's opponent) like a sprite - with a dark/light/lightest step per
  shade, so even a game nobody painted
  shows many colors, the same in both eyes. A game's color pack (see below)
  colors over it, so a game with a pack shows in color out of the box

## Opening the menu

| Device | Button |
|---|---|
| VR controllers | **Left stick click**, or the left controller's **Menu** button |
| Gamepad (Quest) | **Left stick click**, or the **Xbox/Guide** button |
| Desktop 2D build | **Tab** |

Left stick click always works. The physical menu button depends on the
runtime - SteamVR, for instance, keeps it for its own dashboard and never
passes it to the app.

## Project layout

```
CMakeLists.txt              root build - FetchContent for volk, Vulkan-Headers,
                             OpenXR-SDK (loader), glslang (shader compiler)
core/                        shared, platform-agnostic app code
  OpenXrApp.cpp/.h           instance/system/session/swapchain/event loop
  VulkanRenderer.cpp/.h      Vulkan device/pipelines/rendering
  XrMath.h                   small self-contained matrix math
  io/Platform.h              platform interface (asset/ROM/settings I/O, battery),
                             implemented per-platform under platform/
  third_party/stb_image.h    vendored image decoder (JPEG/PNG)
  generated_shaders/         SPIR-V headers, produced by the PC build's
                             shader compiler and committed (Android's
                             cross-compile can't build/run glslang itself)
platform/                    per-platform entry points only, call into core/
  pc/Main.cpp                desktop entry point (OpenXR, streamed to headset)
  pc2d/Main.cpp               flat GLFW window entry point (no headset needed)
  android/AndroidMain.cpp    NativeActivity entry point
shaders/                     GLSL sources (compiled to SPIR-V at PC build time)
assets/                      shared between PC and Android (Gradle assets dir)
tools/ShaderCompiler.cpp     glslang-based GLSL -> SPIR-V compiler, PC-only build tool
android/                     Gradle wrapper project (externalNativeBuild -> root CMakeLists.txt)
```

## Building - PC

No Vulkan SDK required (Vulkan is loaded dynamically via `volk`; shaders are
compiled via glslang's C++ API, fetched and built as part of this project).

```
cmake -B build-pc -G "Visual Studio 18 2026" -A x64 -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-pc --config Debug
build-pc\Debug\VirtualBoyGoPC.exe
```

Requires an OpenXR runtime already registered and a headset actively
connected (Virtual Desktop, SteamVR, or Oculus Link) - it streams straight to
the headset with no APK/adb involved. `XR_ERROR_FORM_FACTOR_UNAVAILABLE`
means the headset isn't currently connected, not a code bug.

### Version string

The Settings page's version label is auto-generated at build time (see
`cmake/GenerateVersion.cmake`) - by default `v<VBGO_VERSION>-dev.<commit
count>` (e.g. `v2.0.0-dev.81`, `-dirty` appended if the working tree has
uncommitted changes). This is deliberately independent of `--config
Debug`/`Release` - an optimized Release build is still just a local dev/perf-
test build unless you explicitly say otherwise. Only pass this for the build
you're actually cutting as a numbered release:

```
cmake -B build-pc -G "Visual Studio 18 2026" -A x64 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DVBGO_RELEASE_BUILD=ON
cmake --build build-pc --config Release
```

which produces the clean `v<VBGO_VERSION>` string instead. Bump `VBGO_VERSION`
in the root `CMakeLists.txt` by hand at each release.

## Building - Android (Quest)

Requires Android SDK (compileSdk 34, build-tools 34.0.0) + NDK 23.2.8568313 +
CMake 3.22.1 (e.g. via `sdkmanager`).

### Setup

Set your SDK path in `android/local.properties` (forward slashes, even on Windows):

```
sdk.dir=C\:/Users/<you>/AppData/Local/Android/Sdk
```

Or set the `ANDROID_HOME` environment variable instead.

For **release** builds only, point at the signing keystore folder (kept
outside the repo) in the same `android/local.properties`:

```
keystore.dir=D\:/Development/VR/VirtualBoyGo Key
```

The folder must contain `android.keystore` and a `keystore.txt` (line 1 =
store password, line 2 = key password). Debug builds don't need this.

Gradle (`./gradlew`) also needs a JDK, separate from the Android SDK/NDK
above - if you get an error like "JAVA_HOME is not set" or "java: command not
found", set `JAVA_HOME` before invoking gradlew. Android Studio already
bundles a JDK, so if it's installed, point at that instead of installing one
separately:

```
# PowerShell, one-time for the session:
$env:JAVA_HOME = "C:\Program Files\Android\Android Studio\jbr"

# or inline per command:
JAVA_HOME="C:\Program Files\Android\Android Studio\jbr" ./gradlew assembleRelease
```

(Adjust the path if Android Studio is installed elsewhere, or use any other
JDK 17+ install's home directory.)

### Build & install (USB)

```
cd android
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Or `assembleRelease` / `app/build/outputs/apk/release/app-release.apk` - an
optimized build for testing on-device (fast enough to actually play), but
still just a dev build (`v2.0.0-dev.<commit count>` in Settings) unless you
add `-Pofficial=true`:

```
./gradlew assembleRelease -Pofficial=true
```

which is what actually cutting a numbered release should use - see
"Version string" above for why this is a separate flag from the Debug/Release
build type.

### Run

```
adb shell am start -n com.nintendont.virtualboygo/android.app.NativeActivity
```

### Wi-Fi ADB (wireless debugging)

One-time: on the headset, **Settings → Developer → Wireless debugging → Pair
device with pairing code**, then `adb pair <ip>:<pairing-port>`.

Each session: `adb connect <headset-ip>:5555`, then use the same
`adb install`/`adb shell am start` commands above. Reconnect (`adb connect`
again) if it drops when the headset sleeps; `adb disconnect` to go back to
USB.

If you change a shader (`shaders/*.vert|frag`), rebuild the **PC** target
first to refresh the committed headers in `core/generated_shaders/`
before building Android - the Android cross-compile doesn't build the shader
compiler itself.

## Building - PC, 2D debug (no headset)

Same CMake project - configuring the PC build (above) also produces this
target. No OpenXR runtime or headset required at all:

```
build-pc\Debug\VirtualBoyGoPC2D.exe
```

## Experimental: tile colorization (work in progress)

Groundwork for per-game color packs, where each 8x8 tile of a game gets its
own colors. The core can record which tile, and which pixel of it, every
screen pixel comes from (`core/emu/vbgo_tiletrack.h`, hooked in through
`cmake/PatchBeetleVip.cmake`; off by default). In the desktop 2D build:

- **F9** toggles a debug view: one random color per distinct tile.
- **F10** saves the current frame to `roms/captures/` as a paint-ready PNG
  (3x, in the active Multicolor palette - Ember works well - or grayscale)
  plus a `.tiles` file mapping every pixel to its tile, so a painted copy can
  be turned into tile colors. **Shift+F10** does the same for the right eye
  (see "Both eyes" below).

**Color packs.** Put painted copies of captures (same size, pixels not
moved, saved as PNG) next to their `.tiles` files in
`roms/colorpacks/<rom name>/`. Loading the ROM imports them into
`roms/<rom name>.vbcp`: each painted pixel votes for its tile pixel's color,
the majority wins where a tile was painted differently in different places,
and stray near-duplicate shades are merged. The pack then colors every
occurrence of those tiles anywhere in the game in Auto mode or while a
Multicolor palette is active - or in Gradient mode, which then colors per
shade too, from the gradient's 5 colors (see above); unpainted tiles keep the
mode's colors. The `.vbcp` file alone is enough on other platforms: copy it
next to the ROM on the Quest, or build it into the app - set `colorpacks.dir`
in `android/local.properties` to a folder of `.vbcp` files (the desktop
build's `roms` folder, say, with forward slashes:
`colorpacks.dir=C:/path/to/roms`) and every Android build bundles them (only
the `.vbcp` files; a `.vbcp` in the headset's ROMs folder still wins). A pack
records the ROM it was made for (CRC-32 and size), so the game finds it
whatever its ROM file is called: by name first, else the bundled pack made
for this ROM (the Android build lists them in `colorpacks/index.txt`), else
- on desktop - one in the `roms` folder. Packs made before this get their
ROM recorded the first time the game loads them by name, or with
`tools/vbcp_rom.py PACK.vbcp ROM.vb`. In the
desktop build, **F11** re-imports after editing a painting and **F8** toggles
the pack for comparison.

**Both eyes.** Packs are painted from the left eye, and both eyes always show
the same thing in the same colors: a pixel's color depends only on what both
eyes share, never on a search of the other eye. Layers both eyes draw and
every sprite both eyes show are the same tile pixels in both eyes, so they're
colored the same by construction. Some games draw a picture per eye instead -
a left-only layer next to a right-only one (Mario Clash's stage, Wario Land's
title, Galactic Pinball's tables), or sprites only one eye shows (much of a
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

**Painting rules.** Only what you change counts: pixels left in the capture's
own colors are ignored, so a capture can be painted a bit at a time. Paint
**magenta (#FF00FF)** over anything that should keep the Multicolor palette's
colors - it overrides whatever else (a tile sheet, say) would color those
tiles. Paint what you want to see: captures record the game's brightness, so
a game that normally runs dimmed (Mario's Tennis runs at about 90%) shows
exactly the painted colors, and fades follow the game from there.

**Same tile, different colors.** Games reuse tiles, so a tile's colors are
looked up from the most specific to the most general, each only where the
paintings consistently ask for it:

1. *Map cell* - a background tile at a fixed spot painted differently there
   than elsewhere (the same letter yellow in one heading and white in the
   next, a selected menu entry) keeps that spot's colors.
2. *Palette* - the game shows a tile in another palette to mark something (an
   option that isn't selected), and that palette is painted differently.
3. *Layer* - a tile reused on another layer (a cloud tile inside a mountain).
4. The tile's own colors.

**Same tile, different characters.** Games also reuse the very same sprite
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

**Painting the background.** Transparent parts of background tiles can be
painted too ("fills" - a court's surface between its speckles, a sky behind a
logo): they get that color at that spot of the map, even where the game
scales or scrolls the layer. A fill only needs to be painted once per spot;
pixels another capture shows that the paintings don't are filled in from
their nearest painted neighbor in the tile.

**Finding what's still uncolored.** In the desktop build, **F7** starts
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

**Tile sheets.** **F6** saves everything in the game's tile memory at that
moment - the graphics loaded for the current area, on screen or not - as
`roms/captures/<rom name> tiles NNN.png` + `.tiles`, laid out 32 tiles wide
like a tile viewer (tiles the pack already colors in its colors). Paint it and
drop it into the pack folder: it fills in every tile pixel the screen
paintings don't color (screen paintings always win, since they show tiles in
context). Paintings and sheets may be resized by an editor as long as the
shape stays the same. Games load some graphics only while they're shown (Wario
Land streams Wario's poses, for example), so sheets and F7 complement each
other: a sheet per area for most things, F7 for what only appears briefly.

**Cost.** Tile tracking is only switched on for games that have a pack (or in
the desktop debug build). The core tags each pixel with one 64-bit store
while it draws (tile, pixel, palette, layer and map cell together), and the
pack is painted from those tags with a few table reads per pixel - a tile's
colors are looked up when its graphics change, not per pixel. Measured on one
desktop core, against the core's ~1.9 ms per emulated frame: tracking adds
~0.25 ms (fills only where the pack has some), and painting a full pack with
map cells and fills ~0.2-0.35 ms per eye. Matching the right eye to the left
adds ~1.5-4 ms (most where each eye's picture is drawn separately, which is
matched by looks; a pixel's match is remembered from frame to frame).
Output is otherwise identical.

**Formats.** `.tiles` version 2 (`VBGOTIL2`) adds the map cell of every
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

## Credits

- **Beetle VB** ([libretro/beetle-vb-libretro](https://github.com/libretro/beetle-vb-libretro)),
  libretro's fork of Mednafen's Virtual Boy emulation - the emulator core,
  GPL-2.0. Compiled from the untouched submodule, except for a one-line
  generated change to its video code that exposes each pixel's shade index
  (see `cmake/PatchBeetleVip.cmake`).
- **Red Viper** ([skyfloogle/red-viper](https://github.com/skyfloogle/red-viper)),
  the Virtual Boy emulator for the 3DS - the per-shade colorization idea
  (its "multicolour" mode), and the "fire & leaf" preset is adapted from
  Red Viper's default multicolour palette. VirtualBoyGo's implementation
  (`core/emu/ShadeColorizer`) is its own code; no Red Viper code is used.
