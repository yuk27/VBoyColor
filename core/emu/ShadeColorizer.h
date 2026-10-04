#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// One color per Virtual Boy shade, in the core's own gamma-encoded 0-1
// space (same as XrColor4f values in Settings.h's palette tables). Plain
// floats rather than XrColor4f so this file has no OpenXR/Vulkan dependency
// and can be unit-tested on its own.
struct ShadeRgb
{
    float r, g, b;
};

// Gamma-encoded output brightness of each VB shade (0 = black, 1-3 = the
// BRTA / BRTB / BRTA+BRTB+BRTC shades) at a typical game's brightness
// settings. A shade drawn at exactly this brightness shows its palette
// color exactly - dimmer (fades, flashes, darker games) blends toward the
// background color, brighter clamps at the palette color. Also what the
// Settings page's Color Palette preview swatches use, so the preview
// matches what's on screen.
inline constexpr float kVbNominalShadeLevels[4] = {0.0f, 0x63 / 255.0f, 0x87 / 255.0f, 1.0f};

// Red Viper-style per-shade colorization ("multicolour" mode in Red Viper,
// github.com/skyfloogle/red-viper): instead of tinting the finished image by
// brightness, every pixel is colored by WHICH of the VB's 4 shades the game
// drew it with, then dimmed by that shade's current brightness:
//
//     color = lerp(palette[0], palette[shade], brightness / nominal[shade])
//
// so a game's dark/light/lightest layers each keep their own hue through
// fades and brightness changes, rather than sliding through a gradient.
// (Red Viper's own math is the same lerp; the brightness normalization is
// VirtualBoyGo's, so the 4 palette colors are what a typical game actually
// shows instead of needing per-shade scale factors.)
//
// Reads the shade index the patched core stores in each pixel's unused top
// byte - see cmake/PatchBeetleVip.cmake.
class ShadeColorizer
{
public:
    // palette[0] = background (the VB's black), [1..3] = darkest to lightest
    // drawn shade. Rebuilds the lookup table - cheap (1024 entries), but no
    // need to call it every frame.
    void SetPalette(const std::array<ShadeRgb, 4> &palette);

    // src: the core's XRGB8888 frame (bytes B,G,R,tag per pixel, tag = shade
    // index 0-3). dst: B,G,R,A bytes ready for the B8G8R8A8 screen texture,
    // A forced opaque. src and dst may not overlap.
    void Colorize(const uint8_t *src, uint8_t *dst, size_t pixelCount) const;

private:
    // [shade][brightness byte] -> B,G,R,A bytes.
    std::array<std::array<std::array<uint8_t, 4>, 256>, 4> m_lut{};
};
