# VBoy Color

**Virtual Boy games in color - in 3D on Meta Quest, and on PC.**

VBoy Color is a free, open-source Virtual Boy emulator. On a Quest headset
it shows games the way the Virtual Boy was meant to be seen, in stereoscopic
3D, but in color instead of red on black. On Windows and Linux it plays them
in a window, no headset needed.

> VBoy Color is an independent fan project. It is not affiliated with,
> endorsed by, or sponsored by Nintendo or Meta. "Virtual Boy" is a trademark
> of Nintendo. No games are included - use only ROMs you made from cartridges
> you own.

|   |   |
|---|---|
| ![](images/0.png) | ![](images/1.png) |
| ![](images/2.png) | ![](images/3.png) |

## Features

- **Color, four ways** (Settings → Color Mode):
  - **Auto** (default) - every pixel is colored by what drew it: background
    layers by their depth (far to near: indigo, teal, green, sand),
    characters by their palette, each with dark/light/lightest steps. Every
    game shows many colors with no setup.
  - **Multicolor** - each of the Virtual Boy's four shades gets its own color
    (in the spirit of Red Viper's multicolour mode), from a set of palettes.
  - **Gradient** - the game's brightness mapped onto a color gradient
    (Jade, Ocean, Sunset, Ember, Frost, Toxic).
  - **Tint** - the classic look: red, or any color you mix.
- **Color packs** - hand-painted colors for a game, tile by tile, shown in
  Auto and Multicolor modes. A pack is one small `.vbcp` file next to the
  ROM; it holds only colors, no game graphics. You can paint your own on PC:
  see [docs/COLOR_PACKS.md](docs/COLOR_PACKS.md).
- **Real 3D on Quest** - each eye gets its own picture, as on the hardware,
  and both eyes always show the same colors.
- **Save states** - several slots per game, with thumbnails.
- **Button mapping** - Quest controllers, gamepads and keyboard, all
  remappable.
- **Screen placement** - size and distance of the screen in VR.
- **Video recording** (PC) - F12 records the original red and the colored
  version side by side, frame for frame, for comparison videos.

## Getting started

### Quest (Quest 2, 3, 3S, Pro)

1. Download `VBoyColor-<version>.apk` from
   [Releases](https://github.com/yuk27/VirtualBoyGo/releases).
2. Install it with [SideQuest](https://sidequestvr.com) or
   `adb install VBoyColor-<version>.apk` (the headset must be in developer
   mode).
3. Copy your `.vb` ROMs to a folder on the headset (for example
   `Download/VBoyColor`), along with any `.vbcp` color packs.
4. Open VBoy Color from **Unknown Sources** in the app library and pick that
   folder when it asks.

### Windows

Download `VBoyColor-windows-<version>.zip` from
[Releases](https://github.com/yuk27/VirtualBoyGo/releases), unzip it, put your
ROMs in the `roms` folder and run `VBoyColor.exe`. `VBoyColorVR.exe` is the
VR version for a PC headset (Quest Link, Virtual Desktop or SteamVR).

### Linux

Download `VBoyColor-linux-<version>.tar.gz`, unpack it, put your ROMs in
`roms` and run `./VBoyColor` from that folder. Needs a Vulkan driver.

## Controls

**Menu:** left stick click (Quest controllers and gamepads), the left
controller's Menu button, or **Tab** on PC (gamepad: Guide or left stick
click). Some PC VR runtimes keep the Menu button for themselves - the stick
click always works.

| Virtual Boy | Quest controllers | Keyboard | Gamepad (PC) |
|---|---|---|---|
| Left D-pad | left stick | arrow keys | D-pad |
| Right D-pad | right stick | W A S D | right stick |
| A / B | A / B | X / Z | A / B |
| L / R | left / right trigger | Q / E | LB / RB |
| Start / Select | Y / X | Enter / Backspace | Start / Back |

Everything can be remapped in Settings → Button Mapping. On PC,
**Alt+Enter** toggles fullscreen.

## Building

See [docs/BUILDING.md](docs/BUILDING.md) - Windows, Linux and Quest, all from
one CMake project. Every push is built by GitHub Actions.

## License

VBoy Color is free software under the **GNU General Public License v3.0**
(see [LICENSE](LICENSE)). The emulator core, Beetle VB, is GPL-2.0-or-later.
Other components and fonts are listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## Thanks

- **[VirtualBoyGo](https://github.com/CidVonHighwind/VirtualBoyGo)** by
  CidVonHighwind - VBoy Color started as a fork of its OpenXR + Vulkan
  rework. The VR app, renderer, menu, save states and button mapping come
  from there. Thank you for building it and sharing it.
- **[Beetle VB](https://github.com/libretro/beetle-vb-libretro)** - libretro's
  fork of Mednafen's Virtual Boy emulation, the emulator core. It is compiled
  untouched, apart from a generated hook in its video code that reports which
  tile drew each pixel (`cmake/PatchBeetleVip.cmake`).
- **[Red Viper](https://github.com/skyfloogle/red-viper)** - the Virtual Boy
  emulator for the 3DS, for the idea of coloring the four shades (its
  multicolour mode). No Red Viper code is used.
- The libraries and fonts listed in
  [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
