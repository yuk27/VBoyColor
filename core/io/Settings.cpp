#include "io/Settings.h"
#include "io/Platform.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr const char *kSettingsFileName = "settings.dat";

    // Versions 12 and 13 only appended fields (selectedShadePalette, then
    // passthrough), so an older file's AppSettings bytes are exactly a
    // prefix of today's layout - copying just that much keeps everything the
    // user had set up (button mapping, screen placement, palette) and leaves
    // the newer fields at their defaults.
    size_t PrefixCompatibleSize(int version)
    {
        switch (version)
        {
        case 11:
            return offsetof(AppSettings, selectedShadePalette);
        case 12:
            return offsetof(AppSettings, passthrough);
        default:
            return 0;
        }
    }

    // Shared by both platform Load paths.
    bool ApplyLoadedSettings(AppSettings &self, int version, const AppSettings &loaded)
    {
        if (version != AppSettings::kVersion)
            return false; // stale layout - leave self untouched, defaults stand

        self = loaded;
        return true;
    }
} // namespace

void AppSettings::Save(Platform &platform) const
{
    std::vector<uint8_t> bytes(sizeof(int) + sizeof(AppSettings));
    const int version = kVersion;
    std::memcpy(bytes.data(), &version, sizeof(version));
    std::memcpy(bytes.data() + sizeof(version), this, sizeof(AppSettings));
    platform.WriteRomsFile(kSettingsFileName, true, bytes.data(), bytes.size());
}

bool AppSettings::Load(Platform &platform)
{
    const std::vector<uint8_t> bytes = platform.ReadRomsFile(kSettingsFileName, true);
    if (bytes.size() < sizeof(int))
        return false;

    int version = 0;
    std::memcpy(&version, bytes.data(), sizeof(version));

    if (const size_t prefix = PrefixCompatibleSize(version); prefix && bytes.size() >= sizeof(int) + prefix)
    {
        AppSettings migrated; // fields past the prefix keep their defaults
        std::memcpy(&migrated, bytes.data() + sizeof(version), prefix);
        *this = migrated;
        return true;
    }

    if (bytes.size() < sizeof(int) + sizeof(AppSettings))
        return false;
    AppSettings loaded;
    std::memcpy(&loaded, bytes.data() + sizeof(version), sizeof(AppSettings));
    return ApplyLoadedSettings(*this, version, loaded);
}

namespace
{
    std::string GameColorsFile(const std::string &game) { return game + ".colors"; }

    // Games that look best in a particular palette when nobody has picked
    // one: matched by a part of the ROM's name (lower case). Everything else
    // starts in Auto.
    struct Suggestion
    {
        const char *nameContains;
        int shadePalette; // kAutoColors, a Multicolor palette, or -1
        int pattern;      // a gradient (with shadePalette -1), else -1
    };
    constexpr Suggestion kSuggestions[] = {
        // Sunset: its blocks, coins and treasure in warm, natural hues.
        {"wario land", -1, 2},
    };
} // namespace

void AppSettings::SaveGameColors(Platform &platform, const std::string &game) const
{
    if (game.empty())
        return;
    char text[160];
    const int n = std::snprintf(text, sizeof(text), "VBoyColor colors 1\nshade %d\npattern %d\npalette %d\ntint %.3f %.3f %.3f\n",
                                selectedShadePalette, selectedPattern, selectedPalette, colorR, colorG, colorB);
    if (n > 0)
        platform.WriteRomsFile(GameColorsFile(game), true, text, static_cast<size_t>(n));
}

void AppSettings::ApplyGameColors(Platform &platform, const std::string &game)
{
    if (game.empty())
        return;
    const std::vector<uint8_t> bytes = platform.ReadRomsFile(GameColorsFile(game), true);
    const std::string text(bytes.begin(), bytes.end());
    int shade = 0, pattern = 0, palette = 0;
    float r = 0, g = 0, b = 0;
    if (std::sscanf(text.c_str(), "VBoyColor colors 1 shade %d pattern %d palette %d tint %f %f %f", &shade, &pattern,
                    &palette, &r, &g, &b) == 6)
    {
        const bool shadeOk = shade == kAutoColors || (shade >= -1 && shade < kShadePaletteCount);
        if (shadeOk && pattern >= -1 && pattern < kScreenPatternCount && palette >= -1 && palette < kPredefColorCount)
        {
            selectedShadePalette = shade;
            selectedPattern = pattern;
            selectedPalette = palette;
            colorR = std::clamp(r, 0.0f, 1.0f);
            colorG = std::clamp(g, 0.0f, 1.0f);
            colorB = std::clamp(b, 0.0f, 1.0f);
            return;
        }
    }
    // Nothing saved for this game: its suggestion, else Auto.
    std::string lower = game;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    selectedShadePalette = kAutoColors;
    selectedPattern = -1;
    for (const Suggestion &suggestion : kSuggestions)
        if (lower.find(suggestion.nameContains) != std::string::npos)
        {
            selectedShadePalette = suggestion.shadePalette;
            selectedPattern = suggestion.pattern;
            break;
        }
}
