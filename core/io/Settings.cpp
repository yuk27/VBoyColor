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

    // Versions 12 to 17 only appended fields (selectedShadePalette,
    // passthrough, then the library's), so an older file's AppSettings bytes are exactly a
    // prefix of today's layout - copying just that much keeps everything the
    // user had set up (button mapping, screen placement, palette) and leaves
    // the newer fields at their defaults. 18 has 17's layout (only the
    // screen look's default changed).
    size_t PrefixCompatibleSize(int version)
    {
        switch (version)
        {
        case 11:
            return offsetof(AppSettings, selectedShadePalette);
        case 12:
            return offsetof(AppSettings, passthrough);
        case 13:
            return offsetof(AppSettings, libraryListView);
        case 14:
            return offsetof(AppSettings, librarySortRecent);
        case 15:
            return offsetof(AppSettings, screenLook);
        case 16:
            return offsetof(AppSettings, libraryOnlyPacks);
        case 17:
            return sizeof(AppSettings);
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
        // Box art became the default in 16 - off was just the old default.
        if (version < 16)
            migrated.downloadBoxArt = true;
        // The LED look became the default in 18 - Sharp was just the old one.
        if (version < 18 && migrated.screenLook == static_cast<int>(ScreenLook::Sharp))
            migrated.screenLook = static_cast<int>(ScreenLook::Led);
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

    // Games that look best in a particular scheme when nobody has picked one:
    // by the ROM's CRC-32, else a part of its name (lower case). Picked by
    // rendering each game's title and play screens in every scheme (Oct 2026,
    // the games without a color pack). Everything else starts in Auto - the
    // games with a pack, and those Auto suits (Space Pinball; Red Alarm, its
    // wireframes by depth; the games with a pack listed here say so).
    // Multicolor palettes, by index in kShadePalettes:
    constexpr int kArcade = 0, kNeon = 1, kCandy = 2, kFireLeaf = 3, kOcean = 7, kSunsetM = 8;
    struct Suggestion
    {
        uint32_t crc;
        const char *nameContains;
        int shadePalette; // kAutoColors, a Multicolor palette, or -1
        int pattern;      // a gradient (with shadePalette -1), else -1
    };
    constexpr Suggestion kSuggestions[] = {
        {0x133e9372, "wario land", kAutoColors, -1}, // its color pack (it started in Gradient, Sunset before it had one)
        {0xbb71b522, "3-d tetris", kAutoColors, -1}, // the well by depth (DepthColors.h), menus, HUD and title by its pack
        {0xe81a3703, "bound high", kArcade, -1},
        {0x2199af41, "golf", kFireLeaf, -1}, // green fairways
        {0x6ba07915, "virtual golf", kFireLeaf, -1},
        {0x83cb6a00, "innsmouth", kAutoColors, -1}, // its color pack (it started in Ocean before it had one)
        {0xdf4d56b4, "funky bowling", kCandy, -1},
        {0xf3cd40dd, "niko-chan", kArcade, -1},
        {0x19bb2dfb, "panic bomber", kAutoColors, -1}, // its color pack (scenery) - Panibon uses it too
        {0x40498f5e, "panibon", kAutoColors, -1},
        {0xaa10a7b4, "red alarm", kAutoColors, -1}, // wireframes colored by depth (DepthColors.h), HUD and menus by its pack
        {0x7e85c45d, "red alarm", kAutoColors, -1},
        {0x44788197, "gundam", kOcean, -1},
        {0xfa44402d, "space invaders", kAutoColors, -1}, // its color pack (it started in Neon before it had one)
        {0x60895693, "space squash", kNeon, -1},
        {0x3ccb67ae, "v-tetris", kAutoColors, -1}, // its color pack
        {0x4c32ba5e, "vertical force", kAutoColors, -1}, // its color pack - the Japanese release uses it too
        {0x9e9b8b92, "vertical force", kAutoColors, -1},
        {0x20688279, "virtual bowling", kSunsetM, -1}, // wooden lanes - its color pack paints pins, HUD and menus over them (the close-ups are drawn without tiles)
        {0x526cc969, "virtual fishing", kOcean, -1},
        {0x8989fe0a, "virtual lab", kCandy, -1},
        {0x736b40d6, "league baseball", kArcade, -1},
        {0x9ba8bb5e, "yakyuu", kArcade, -1},
        {0x82a95e51, "waterworld", kOcean, -1},
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

void AppSettings::ApplyGameColors(Platform &platform, const std::string &game, uint32_t romCrc)
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
    const Suggestion *found = nullptr;
    for (const Suggestion &suggestion : kSuggestions)
        if (romCrc && suggestion.crc == romCrc)
        {
            found = &suggestion;
            break;
        }
    for (const Suggestion &suggestion : kSuggestions)
        if (!found && lower.find(suggestion.nameContains) != std::string::npos)
            found = &suggestion;
    if (found)
    {
        selectedShadePalette = found->shadePalette;
        selectedPattern = found->pattern;
    }
}
