# VBoy Color on the Steam Frame

The Steam Frame runs SteamOS on ARM64, so VBoy Color runs there natively as
the Linux VR app, `VBoyColorVR`, from the `linux-arm64` download. Nothing is
emulated or translated, and it's the same OpenXR + Vulkan app as on the
Quest and on PC.

## Installing

1. Download `VBoyColor-<version>-linux-arm64.tar.gz` from
   [Releases](https://github.com/yuk27/VBoyColor/releases) and unpack it
   somewhere in your home folder on the Frame (say `~/Games/VBoyColor`).
2. Put your `.vb` ROMs in its `roms` folder. Settings, saves and save states
   go next to them, same as the desktop apps (Settings > Folders moves them).
3. Run `./install-to-steam.py` in that folder. It adds VBoy Color to Steam
   as a non-Steam game, as a VR app, with the Quest's icon and library
   artwork made from the logo (`steam/`, made by `tools/make_icon.py`), so
   it looks like any other game in the library. Steam only reads its list of
   non-Steam games when it starts, so the script closes it for a moment
   (on SteamOS by stopping and starting Steam's service). Running it again
   updates the entry; `--uninstall` takes it out again.
4. Start it from the library, not from a terminal: SteamVR only hands the
   controllers to apps it launched.

## Controls

The Frame's controllers are a gamepad split in two: the D-pad and View on
the left, A, B, X, Y and Menu on the right. The buttons do what the same
buttons do on a Quest, and the extra ones work like a gamepad's.

| Virtual Boy | Steam Frame |
|---|---|
| Left D-pad | left stick or the D-pad |
| Right D-pad | right stick |
| A / B | A / B |
| L / R | left / right trigger, or the bumpers |
| Start | Y |
| Select | X, or View |
| VBoy Color's menu | Menu (or hold the left stick click for a second) |

Everything can be remapped in Settings > Button mapping, as anywhere else.
The D-pad, the bumpers and View show up there as the gamepad's buttons.

## The refresh rate

SteamVR picks the refresh rate, not the app: set it for VBoy Color in
SteamVR's per-app settings in the headset. The Virtual Boy draws 50.27
frames a second, so a rate that's a multiple of it (100 Hz) scrolls most
evenly, and 120 Hz is the next best.

## How it works

Two changes make the Frame work. Both probably help PC VR on SteamVR too, but
I've only tried them on the Frame.

The first is the 3D. The screen used to be two quad layers, one per eye
(`eyeVisibility` LEFT and RIGHT), but SteamVR shows such a layer to both
eyes, so on the Frame both eyes got the same picture and the 3D was gone.
On SteamVR the screen now goes out as a projection layer instead, which is
per eye by definition: each eye's view looks from that eye straight at its
quad, with the field of view the quad's corners make from there
(`OpenXrApp::RenderScreenProjection`). The compositor draws each picture
exactly where the quad was, so placement, size, Distance, the IPD offset and
Follow head all work as before. Other runtimes keep the quads.

The second is the controllers. With `XR_VALVE_frame_controller_interaction`
the Frame's controllers get their own bindings (`XrInput::Initialize`).
Touch's bindings are still suggested for every other runtime and controller.

The Linux builds now include `VBoyColorVR` (it already built there, it just
wasn't packaged), and CI builds Linux on ARM64 too.

## Building it yourself

The Linux steps in [BUILDING.md](BUILDING.md) work on any ARM64 Linux
machine; build the `VBoyColorVR` target. `tools/steam_frame/` has a Docker
build that does the same on an ARM64 Mac or Linux box.
