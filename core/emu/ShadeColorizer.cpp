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
} // namespace

void ShadeColorizer::SetPalette(const std::array<ShadeRgb, 4> &palette)
{
    const ShadeRgb &background = palette[0];
    for (int shade = 0; shade < 4; ++shade)
    {
        const ShadeRgb &target = palette[shade];
        const float nominal = kVbNominalShadeLevels[shade];
        for (int level = 0; level < 256; ++level)
        {
            // Shade 0 is the VB's black - always exactly the background,
            // whatever its (always zero) brightness.
            const float t = nominal > 0.0f ? std::min(1.0f, (level / 255.0f) / nominal) : 0.0f;
            std::array<uint8_t, 4> &out = m_lut[shade][level];
            out[0] = ToByte(background.b + (target.b - background.b) * t);
            out[1] = ToByte(background.g + (target.g - background.g) * t);
            out[2] = ToByte(background.r + (target.r - background.r) * t);
            out[3] = 0xFF;
        }
    }
}

void ShadeColorizer::Colorize(const uint8_t *src, uint8_t *dst, size_t pixelCount) const
{
    for (size_t i = 0; i < pixelCount; ++i, src += 4, dst += 4)
    {
        // The core outputs gray (R==G==B) at its default "black & white"
        // color mode; max() just keeps this right if that's ever changed.
        const uint8_t level = std::max({src[0], src[1], src[2]});
        std::memcpy(dst, m_lut[src[3] & 3][level].data(), 4);
    }
}
