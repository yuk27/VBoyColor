#include "emu/ShadeColorizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    uint8_t ToByte(float v)
    {
        return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    }

    std::array<uint8_t, 4> ToBgra(float r, float g, float b)
    {
        return {ToByte(b), ToByte(g), ToByte(r), 0xFF};
    }
} // namespace

void ShadeColorizer::SetPalette(const std::array<ShadeRgb, 4> &palette)
{
    const ShadeRgb &background = palette[0];
    m_background = ToBgra(background.r, background.g, background.b);

    for (int tag = 0; tag < 256; ++tag)
    {
        const int shade = tag & 3;
        const int fadeLevel = tag >> 2; // BrightnessCache[3] >> 2: linear light, 0-63
        // Same gamma curve the core applies to its own output (see vip.c's
        // MakeColorLUT), so the palette fades exactly as fast as the game's
        // grayscale would have.
        const float fade = std::pow(fadeLevel / 63.0f, 1.0f / 2.2f);
        const float t = shade == 0 ? 0.0f : fade; // shade 0 is the VB's black - always the background
        const ShadeRgb &target = palette[shade];
        m_lut[tag] = ToBgra(background.r + (target.r - background.r) * t,
                            background.g + (target.g - background.g) * t,
                            background.b + (target.b - background.b) * t);
    }
}

void ShadeColorizer::Colorize(const uint8_t *src, uint8_t *dst, size_t pixelCount) const
{
    for (size_t i = 0; i < pixelCount; ++i, src += 4, dst += 4)
    {
        // A shade switched fully off by the game (output 0) shows as
        // background, whatever the fade level says.
        const bool off = (src[0] | src[1] | src[2]) == 0;
        std::memcpy(dst, off ? m_background.data() : m_lut[src[3]].data(), 4);
    }
}
