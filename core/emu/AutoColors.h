#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// "Auto" colors: what a game shows in color without anyone painting it.
//
// Coloring by shade alone (Red Viper's per-shade palettes, or a gradient by
// brightness) can only ever show 4 colors at once - the Virtual Boy only
// outputs 4 shades. The tile tracker knows more about every pixel: which
// layer (world) drew it - and the game's draw order says how far back that
// layer is - or that it's a sprite, and with which palette. So:
//  - background layers take a ramp by their depth in the drawing order, far
//    to near: indigo, teal, green, sand - the farthest layer the game draws
//    is the first ramp, its nearest the last (over the layers seen so far);
//  - sprites take a warm, saturated ramp by their palette (most games draw
//    their characters with palette 0: red / orange / yellow), so they stand
//    out against the scenery - and so do small layers that a game draws a
//    character on (by their palette too - see ColorPackRenderer's figures);
//  - each of the VB's 3 drawn shades is that ramp's dark, light or lightest
//    color, like a Multicolor palette - and fades with the game's
//    brightness the same way.
// A color pack's colors win wherever it has some (ColorPackRenderer).
namespace AutoColors
{
    using Rgb = std::array<uint8_t, 3>;
    using Ramp = std::array<Rgb, 3>; // the dark, light and lightest shade

    // Far to near.
    inline constexpr std::array<Ramp, 4> kLayerRamps = {{
        {{{28, 30, 88}, {70, 88, 178}, {168, 186, 238}}},  // indigo
        {{{14, 66, 78}, {36, 146, 150}, {158, 226, 214}}}, // teal
        {{{38, 78, 26}, {104, 168, 58}, {208, 236, 150}}}, // green
        {{{96, 54, 22}, {206, 128, 52}, {252, 224, 164}}}, // sand / orange
    }};
    // By sprite palette (JPLT0-3).
    inline constexpr std::array<Ramp, 4> kSpriteRamps = {{
        {{{132, 24, 24}, {236, 82, 44}, {255, 226, 128}}},  // red / yellow
        {{{24, 46, 132}, {70, 128, 236}, {206, 232, 255}}}, // blue
        {{{22, 96, 36}, {70, 196, 78}, {210, 252, 176}}},   // green
        {{{108, 22, 96}, {214, 76, 176}, {255, 206, 242}}}, // magenta
    }};
    // The background (shade 0) and what untracked pixels show (drawn without
    // tiles - rare): a neutral dark blue-gray ramp.
    inline constexpr std::array<Rgb, 4> kBase = {{{6, 6, 14}, {70, 76, 98}, {140, 148, 170}, {226, 230, 240}}};

    // The ramp at a depth (0 = farthest layer, 1 = nearest), blended between
    // the 4 above.
    inline Ramp LayerRamp(float depth)
    {
        depth = depth < 0.0f ? 0.0f : depth > 1.0f ? 1.0f : depth;
        const float at = depth * (kLayerRamps.size() - 1);
        const std::size_t i = at >= kLayerRamps.size() - 1 ? kLayerRamps.size() - 2 : static_cast<std::size_t>(at);
        const float f = at - static_cast<float>(i);
        Ramp out{};
        for (std::size_t s = 0; s < 3; ++s)
            for (std::size_t c = 0; c < 3; ++c)
                out[s][c] = static_cast<uint8_t>(kLayerRamps[i][s][c] + (kLayerRamps[i + 1][s][c] - kLayerRamps[i][s][c]) * f + 0.5f);
        return out;
    }
} // namespace AutoColors
