# Third-party notices

VBoy Color is licensed under the GNU General Public License v3.0 (see
`LICENSE`). It is built on, and ships with, the following work by others.
Full license texts are in `licenses/` unless noted.

## Code this app grew from

**VirtualBoyGo** by CidVonHighwind (Patrick) and contributors -
[github.com/CidVonHighwind/VirtualBoyGo](https://github.com/CidVonHighwind/VirtualBoyGo).
VBoy Color started as a fork of its 2026 OpenXR + Vulkan rework: the VR app,
renderer, menu, save states, button mapping and the menu icons
(`assets/icons/`) come from there. GPL-3.0 (`LICENSE`). The VBoy Color
logo (`assets/logo/`) is Juan's own artwork.

## Compiled into the app

| Component | Use | License |
|---|---|---|
| [Beetle VB](https://github.com/libretro/beetle-vb-libretro) (libretro's fork of Mednafen's Virtual Boy emulation) | the emulator core | GPL-2.0-or-later (`third_party/beetle-vb-libretro/COPYING`) |
| libretro-common (inside Beetle VB) | core interface | MIT-style (headers in `third_party/beetle-vb-libretro/libretro-common/`) |
| [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) loader | VR runtime access | Apache-2.0 (`licenses/Apache-2.0.txt`) |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | Vulkan API definitions | Apache-2.0 or MIT (`licenses/Vulkan-Headers.txt`) |
| [volk](https://github.com/zeux/volk) | Vulkan loader | MIT (`licenses/MIT-volk.txt`) |
| [FreeType](https://freetype.org) | font rendering | FreeType License (`licenses/FTL-FreeType.txt`) |
| [GLFW](https://www.glfw.org) (desktop only) | windows and input | zlib (`licenses/Zlib-GLFW.txt`) |
| [stb_image, stb_image_write](https://github.com/nothings/stb) | PNG/JPEG reading and writing | public domain or MIT (in the headers, `third_party/`) |
| [miniaudio](https://miniaud.io) | audio output | public domain or MIT-0 (in the header, `third_party/`) |

Portions of this software are copyright © 2023 The FreeType Project
(www.freetype.org). All rights reserved.

## Fonts

| Font | License |
|---|---|
| Roboto (Regular, Bold) - Copyright 2011 Google Inc. | Apache-2.0 (`licenses/Apache-2.0.txt`) |

## Build tools (not shipped)

[glslang](https://github.com/KhronosGroup/glslang) compiles the shaders on
the PC build (BSD-3-Clause and others, see its repository). Gradle and the
Android SDK/NDK build the Quest app.

## Ideas

**Red Viper** ([skyfloogle/red-viper](https://github.com/skyfloogle/red-viper)),
the Virtual Boy emulator for the 3DS: the per-shade colorization idea (its
"multicolour" mode); the "fire & leaf" preset is adapted from its default
multicolour palette. No Red Viper code is used.
