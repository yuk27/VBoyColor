#pragma once

#include "input/ButtonMapping.h"

#include <openxr/openxr.h> // XrColor4f

#include <array>
#include <cstdint>
#include <string>

class Platform;

// kShadePalettes' entries made from the gradient patterns (kScreenPatterns):
// palette kFirstGradientShadePalette + n is pattern n (see the table below).
inline constexpr int kFirstGradientShadePalette = 6;

// AppSettings::selectedShadePalette's value for the Auto color mode: colors
// by what drew each pixel - its layer's depth, a sprite's palette (see
// core/emu/AutoColors.h) - under a game's color pack, if it has one. The
// default, so a game shows in color (its pack's, if any) out of the box.
inline constexpr int kAutoColors = 100;

// How the screen/menu quads react to head rotation - see
// OpenXrApp::ComputeScreenOrientation.
enum class FollowHeadMode : int32_t
{
    Off = 0,     // world-fixed, no head influence
    Smooth = 1,  // gradually chases the head orientation
    Instant = 2, // rigidly locked to the current head orientation every frame
};

// Persisted app-wide settings, one instance for the app's lifetime, threaded
// to every menu page via UiMenuResources::settings.
// How the game screen is drawn (Settings > Screen > Look - see
// shaders/screen_filter.frag): crisp pixels, smoothed edges, or the
// Virtual Boy's own LED rows with a glow.
enum class ScreenLook
{
    Sharp = 0,
    Smooth = 1,
    Led = 2,
};
inline constexpr int kScreenLookCount = 3;

struct AppSettings
{
    // Bumped whenever the on-disk layout changes - Load() refuses (leaves
    // defaults in place) on a mismatch rather than attempting migration,
    // except from version 11, whose layout is an exact prefix of this one
    // (12 only appended selectedShadePalette) - see Settings.cpp.
    static constexpr int kVersion = 18;

    // Move Screen / Follow Head. Only OpenXrApp's quad-layer pose consumes
    // these - meaningless on the flat pc2d debug build.
    FollowHeadMode followHeadMode = FollowHeadMode::Off;
    float screenYaw = 0.0f;
    float screenPitch = 0.0f;
    float screenRoll = 0.0f;
    // meters - also doubles as the curved-screen cylinder radius (see
    // OpenXrApp::RenderScreenLayer). Menu distance is separate, see
    // kMenuDistanceMeters.
    float screenDistance = 2.2f;
    float screenScale = 1.0f;
    // Unused: the curved screen option was dropped (kept so the file's
    // layout stays the same).
    bool curvedScreen = false;

    bool useThreeDeeMode = true;
    float ipdOffset = 0.0f;  // meters
    int selectedPalette = 0; // authentic Virtual Boy red
    float colorR = 1.0f, colorG = 0.0f, colorB = 0.0f;
    // -1 = off (flat colorR/G/B tint above); 0-5 = index into kScreenPatterns,
    // a multi-hue gradient recolor instead. Mutually exclusive with the tint:
    // SettingsPage hides the R/G/B rows while this is >= 0.
    int selectedPattern = -1;

    // VB gameplay button remapping, indexed by VBButtonBit constants (see
    // Emulator.h); slots 1 and 9 are unused gaps and stay unbound. Each
    // button carries two independent physical bindings
    // (MappedButtons::Buttons[0]/[1]) - either triggers it.
    ButtonMapper::MappedButtons vbButtons[16];

    // -1 = off; 0+ = index into kShadePalettes - per-shade colors baked into
    // the frame itself by Emulator::SetShadePalette (Red Viper-style
    // colorization); kAutoColors = the Auto mode (the default). Mutually
    // exclusive with both the tint and selectedPattern above: while one's
    // active, ScreenTint()/ScreenPattern() go neutral and SettingsPage hides
    // the R/G/B rows. Kept as the LAST field so a version-11 settings file is
    // an exact prefix of this layout (see Settings.cpp's migration).
    int selectedShadePalette = kAutoColors;

    // Version 13: the room around the screen (passthrough, on headsets that
    // have it) instead of black. Appended - older files load as a prefix.
    bool passthrough = false;

    // Version 14 (appended - older files load as a prefix): the library as
    // a list instead of cards, and its thumbnails from libretro's box art
    // (downloaded) instead of title screens - on by default since version 16
    // (a game without box art gets its title screen; see Settings.cpp).
    bool libraryListView = false;
    bool downloadBoxArt = true;

    // Version 15 (appended): the library's last played games first.
    bool librarySortRecent = false;

    // Version 16 (appended): how the game screen is drawn - ScreenLook - and,
    // in the PC window, how big: as big as fits (false) or the biggest whole
    // multiple of its pixels (true). LED by default since version 18 (the
    // layout didn't change; an older file still on Sharp, the old default,
    // gets LED once - see Settings.cpp).
    int screenLook = static_cast<int>(ScreenLook::Led);
    bool screenWholePixels = false;

    // Version 17 (appended): the library shows only the games with a color
    // pack.
    bool libraryOnlyPacks = false;

    // What the screen should be drawn with (Emulator::DrawScreen's tint/
    // patternIndex) - neutral while a shade palette is active, since its
    // colors are already in the frame and tinting on top would double them.
    //
    // The per-shade palette the emulator colors with (Emulator::
    // SetShadePalette): the Multicolor one or kAutoColors, the modes that
    // show a game's color pack. Tint and Gradient never do - they only tint
    // the shades (a Multicolor palette made from a gradient colors with the
    // pack, and takes the gradient wherever the pack doesn't).
    int EffectiveShadePalette() const { return selectedShadePalette >= 0 ? selectedShadePalette : -1; }
    XrColor4f ScreenTint() const
    {
        return EffectiveShadePalette() >= 0 ? XrColor4f{1.0f, 1.0f, 1.0f, 1.0f} : XrColor4f{colorR, colorG, colorB, 1.0f};
    }
    int ScreenPattern() const { return EffectiveShadePalette() >= 0 ? -1 : selectedPattern; }

    // Best-effort; failures are silently ignored. Takes Platform by
    // reference rather than storing one - this struct is memcpy'd whole
    // to/from disk (see Settings.cpp), so it must stay trivially copyable.
    void Save(Platform &platform) const;

    // Replaces every field; returns false (self left untouched) if the file
    // doesn't exist or its version doesn't match kVersion.
    bool Load(Platform &platform);

    // Colors per game: the color settings (Color Mode, its palette, the
    // tint) are remembered for each game - SaveGameColors whenever they
    // change while it's loaded ("<game>.colors" in the States folder) - and
    // ApplyGameColors puts them back when it loads. A game with none saved
    // yet gets its suggested ones (a few games look best in a particular
    // palette - see Settings.cpp), else Auto. The tint's R/G/B carry over
    // from game to game unless a game has its own.
    void SaveGameColors(Platform &platform, const std::string &game) const;
    // romCrc (the ROM's CRC-32, 0 if unknown) finds a game's suggestion
    // whatever its file is called.
    void ApplyGameColors(Platform &platform, const std::string &game, uint32_t romCrc = 0);
};

// Preset VB screen colors. Index 0 is the authentic red Virtual Boy display
// and the factory default.
inline constexpr std::array<XrColor4f, 11> kPredefColors = {{
    {1.0f, 0.0f, 0.0f, 1.0f},
    {0.9f, 0.3f, 0.1f, 1.0f},
    {1.0f, 0.85f, 0.1f, 1.0f},
    {0.25f, 1.0f, 0.1f, 1.0f},
    {0.0f, 1.0f, 0.45f, 1.0f},
    {0.0f, 1.0f, 0.85f, 1.0f},
    {0.0f, 0.85f, 1.0f, 1.0f},
    {0.15f, 1.0f, 1.0f, 1.0f},
    {0.75f, 0.65f, 1.0f, 1.0f},
    {1.0f, 1.0f, 1.0f, 1.0f},
    {1.0f, 0.3f, 0.2f, 1.0f},
}};
inline constexpr int kPredefColorCount = static_cast<int>(kPredefColors.size());

// Named multi-hue gradients (5 stops each, darkest to brightest) - shared
// by screen_pattern.frag's rendering and the Color Palette row's preview
// swatches (SettingsPage::DrawColorPreview), so no color data is duplicated
// into the shader itself.
inline constexpr std::array<std::array<XrColor4f, 5>, 6> kScreenPatterns = {{
    // Jade - near black -> deep green -> teal -> yellow-green -> cream white
    {{{0.02f, 0.03f, 0.02f, 1.0f}, {0.05f, 0.20f, 0.10f, 1.0f}, {0.10f, 0.45f, 0.40f, 1.0f}, {0.55f, 0.75f, 0.35f, 1.0f}, {0.97f, 0.96f, 0.85f, 1.0f}}},
    // Ocean - near black -> navy -> azure -> sky blue -> ice white
    {{{0.01f, 0.02f, 0.04f, 1.0f}, {0.05f, 0.15f, 0.35f, 1.0f}, {0.10f, 0.35f, 0.65f, 1.0f}, {0.55f, 0.80f, 0.90f, 1.0f}, {0.95f, 0.98f, 1.00f, 1.0f}}},
    // Sunset - near black -> deep purple -> magenta -> orange -> pale gold
    {{{0.04f, 0.01f, 0.05f, 1.0f}, {0.30f, 0.05f, 0.35f, 1.0f}, {0.75f, 0.20f, 0.25f, 1.0f}, {0.95f, 0.55f, 0.20f, 1.0f}, {1.00f, 0.92f, 0.75f, 1.0f}}},
    // Ember - near black -> maroon -> deep red -> amber -> pale yellow
    {{{0.03f, 0.01f, 0.00f, 1.0f}, {0.35f, 0.04f, 0.02f, 1.0f}, {0.65f, 0.15f, 0.02f, 1.0f}, {0.95f, 0.55f, 0.10f, 1.0f}, {1.00f, 0.95f, 0.75f, 1.0f}}},
    // Frost - near black -> indigo -> violet -> lavender -> ice white
    {{{0.02f, 0.02f, 0.05f, 1.0f}, {0.15f, 0.15f, 0.40f, 1.0f}, {0.40f, 0.35f, 0.70f, 1.0f}, {0.75f, 0.75f, 0.95f, 1.0f}, {0.98f, 0.98f, 1.00f, 1.0f}}},
    // Toxic - near black -> dark olive -> teal-green -> chartreuse -> pale lime
    {{{0.02f, 0.03f, 0.00f, 1.0f}, {0.15f, 0.30f, 0.02f, 1.0f}, {0.35f, 0.55f, 0.05f, 1.0f}, {0.65f, 0.85f, 0.15f, 1.0f}, {0.95f, 1.00f, 0.70f, 1.0f}}},
}};
inline constexpr int kScreenPatternCount = static_cast<int>(kScreenPatterns.size());

namespace SettingsDetail
{
    // 0xRRGGBB -> XrColor4f, so the palette table below can be written (and
    // compared against reference palettes) as familiar hex codes.
    constexpr XrColor4f Hex(uint32_t rgb)
    {
        return {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, 1.0f};
    }

    constexpr std::array<XrColor4f, 4> Hex4(uint32_t bg, uint32_t dark, uint32_t light, uint32_t lightest)
    {
        return {{Hex(bg), Hex(dark), Hex(light), Hex(lightest)}};
    }

    // A gradient pattern's per-shade equivalent: its darkest stop as the
    // background and its 3 brightest stops for the 3 drawn shades - about
    // what the gradient itself shows at a typical game's full brightness
    // (e.g. Virtual Boy Wario Land's), but held steady through fades. Read
    // straight from kScreenPatterns, so the two can't drift apart. Only the
    // starting point: the emulator colors with all 5 stops, each shade where
    // the game's brightness for it falls on the gradient (see
    // ShadeColorizer::SetGradient).
    constexpr std::array<XrColor4f, 4> FromPattern(int pattern)
    {
        const auto &stops = kScreenPatterns[pattern];
        return {{stops[0], stops[2], stops[3], stops[4]}};
    }
} // namespace SettingsDetail

// Per-shade ("Multicolor") palettes (AppSettings::selectedShadePalette) -
// one color for each of the VB's 4 shades: background (the VB's black), then
// the dark, light and lightest drawn shades (BRTA, BRTB, BRTA+BRTB+BRTC).
// Unlike the tint/patterns above, which recolor the finished frame by
// brightness, these color each pixel by which shade it is
// (core/emu/ShadeColorizer.h) - the colorization approach Red Viper
// (github.com/skyfloogle/red-viper) introduced, which keeps each layer's hue
// stable through fades. At full brightness each shade appears as exactly the
// color listed, in every game. Order matters only for the Color Palette
// cycle (index 0 is what Multicolor mode starts on); the index is what's
// saved to settings.dat.
inline constexpr std::array<std::array<XrColor4f, 4>, 16> kShadePalettes = {{
    // --- Vivid multi-hue ---
    SettingsDetail::Hex4(0x050A24, 0xE0301E, 0x2E7DF0, 0xFFE14D), // Arcade - red / blue / yellow on navy
    SettingsDetail::Hex4(0x0B0614, 0x7A1FA2, 0x00B8D4, 0xF4FF81), // Neon - purple / cyan / lime
    SettingsDetail::Hex4(0x160A1C, 0x6A3FB5, 0xFF5FA2, 0xB8F4FF), // Candy - violet / pink / ice
    SettingsDetail::Hex4(0x080200, 0x8C2A0A, 0x1DBB00, 0xFFD800), // Fire & leaf - rust / green / gold (after Red Viper's multicolour default)
    SettingsDetail::Hex4(0x03141A, 0x8E2C6B, 0x1FB3A6, 0xFFD9A0), // Tropical - plum / teal / peach
    SettingsDetail::Hex4(0x000000, 0xAA00AA, 0x00AAAA, 0xFFFFFF), // Retro PC - magenta / cyan / white
    // --- The gradient patterns, per shade ---
    SettingsDetail::FromPattern(0), // Jade
    SettingsDetail::FromPattern(1), // Ocean
    SettingsDetail::FromPattern(2), // Sunset
    SettingsDetail::FromPattern(3), // Ember
    SettingsDetail::FromPattern(4), // Frost
    SettingsDetail::FromPattern(5), // Toxic
    // --- Classic handheld looks (light ones fade toward their light background) ---
    SettingsDetail::Hex4(0x9BBC0F, 0x6A8F1C, 0x306230, 0x0F380F), // LCD green, light background (dark shade spaced off the background so it stays readable)
    SettingsDetail::Hex4(0x0F380F, 0x306230, 0x8BAC0F, 0x9BBC0F), // LCD green, dark background
    SettingsDetail::Hex4(0xF8E8C8, 0xD89048, 0xA82820, 0x301850), // Super warm - cream / orange / red / plum
    SettingsDetail::Hex4(0xF4EFE1, 0x8C877B, 0x4A463F, 0x141414), // Paper - dark ink on off-white
}};
inline constexpr int kShadePaletteCount = static_cast<int>(kShadePalettes.size());

// The gradient pattern a per-shade palette was made from, or -1.
constexpr int GradientOfShadePalette(int palette)
{
    return palette >= kFirstGradientShadePalette && palette < kFirstGradientShadePalette + kScreenPatternCount
               ? palette - kFirstGradientShadePalette
               : -1;
}
static_assert(kShadePalettes[kFirstGradientShadePalette][1].g == kScreenPatterns[0][2].g &&
                  kShadePalettes[kFirstGradientShadePalette + kScreenPatternCount - 1][3].r == kScreenPatterns[kScreenPatternCount - 1][4].r,
              "kFirstGradientShadePalette must point at the FromPattern entries");
