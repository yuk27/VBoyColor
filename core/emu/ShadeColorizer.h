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

// Red Viper-style per-shade colorization ("multicolour" mode in Red Viper,
// github.com/skyfloogle/red-viper): instead of tinting the finished image by
// brightness, every pixel is colored by WHICH of the VB's 4 shades the game
// drew it with, so a game's dark/light/lightest layers each keep their own
// hue regardless of the brightness the game happens to use.
//
// Brightness then only drives fades, as one uniform dim of the whole palette
// toward the background color, taken from the brightest shade's current
// level (games fade by scaling all 3 shade brightnesses together):
//
//     color = lerp(palette[0], palette[shade], fade)
//
// so the 4 palette colors are exactly what's on screen at full brightness,
// in every game, and stay in proportion through fades. (Red Viper instead
// dims each shade by its own brightness register, which needs per-shade
// scale factors tuned to a game's brightness settings.) One exception: a
// shade a game has switched fully off (brightness 0 - sometimes used to hide
// a layer) shows as background.
//
// Reads the shade index and fade level the patched core stores in each
// pixel's unused top byte - see cmake/PatchBeetleVip.cmake.
class ShadeColorizer
{
public:
    // palette[0] = background (the VB's black), [1..3] = darkest to lightest
    // drawn shade. Rebuilds the lookup table - cheap (256 entries), but no
    // need to call it every frame.
    void SetPalette(const std::array<ShadeRgb, 4> &palette);

    // A gradient instead (Settings.h's kScreenPatterns, 5 stops, darkest
    // first): each drawn shade takes the gradient's color at the brightness
    // the game gives that shade - what Gradient mode shows at the game's full
    // brightness, so a game with a dim dark shade gets the gradient's deep
    // second stop too - and fades as one uniform dim toward the first stop,
    // like the per-shade palettes, so hues hold through fades instead of
    // sliding down the gradient. The shades' brightnesses are learnt from the
    // frames (Observe); until then they're taken as the usual 1/2, 3/4, 1.
    // referenceLevel: the game's full brightness (0-63, as the fade level in
    // the pixel tags), e.g. a color pack's; -1 = the brightest seen so far.
    void SetGradient(const std::array<ShadeRgb, 5> &stops, int referenceLevel = -1);
    bool IsGradient() const { return m_gradient; }
    // Gradient only: learns the shade brightnesses from a frame of the core's
    // output (same layout as Colorize's src); call once per frame before
    // Colorize. Cheap: a sparse scan, and the table is only rebuilt when
    // something changed.
    void Observe(const uint8_t *src, size_t width, size_t height, size_t strideBytes);
    // How far a shade is toward its full color at a fade level (0-63) - the
    // factor Colorize uses.
    float Fade(int level) const;
    // The 4 colors as shown at full brightness (background, 3 shades).
    std::array<ShadeRgb, 4> Palette() const { return m_palette; }

    // src: the core's XRGB8888 frame (bytes B,G,R,tag per pixel - tag bits
    // 0-1 shade index, 2-7 fade level). dst: B,G,R,A bytes ready for the
    // B8G8R8A8 screen texture, A forced opaque. src and dst may not overlap.
    void Colorize(const uint8_t *src, uint8_t *dst, size_t pixelCount) const;

private:
    void BuildGradient();

    // Indexed by the whole tag byte ([fade level][shade]) -> B,G,R,A bytes.
    std::array<std::array<uint8_t, 4>, 256> m_lut{};
    std::array<uint8_t, 4> m_background{};
    std::array<ShadeRgb, 4> m_palette{};
    // Gradient mode
    bool m_gradient = false;
    std::array<ShadeRgb, 5> m_stops{};
    int m_referenceLevel = -1, m_fixedReference = -1; // 1-63
    std::array<float, 4> m_shadeLuma{0.0f, 0.5f, 0.75f, 1.0f}; // each shade's brightness at the reference level, 0-1
};
