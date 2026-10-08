#include "menu/pages/SettingsPage.h"
#include "emu/AutoColors.h"
#include "emu/Emulator.h"
#include "io/Platform.h"
#include "io/Settings.h"
#include "menu/MenuPage.h"
#include "menu/pages/AppMenuLayout.h"
#include "menu/ThumbnailLibrary.h"

#include <algorithm>
#include <cstdio>

namespace
{
std::string FormatFloat(const char *prefix, float value, int precision = 3, const char *suffix = "")
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s%.*f%s", prefix, precision, value, suffix);
    return buf;
}

// Preview swatches drawn on the "Palette" row's right, showing the VB's 4
// brightness levels (black up to the full tint) or the palette's colors -
// stands in for the selected-palette index (nobody reads "7/11"; the colors
// themselves are what matters). Reads AppSettings live each frame, so it
// tracks palette/R/G/B edits immediately.
constexpr float kSwatchSize = 9.0f;
constexpr float kSwatchGap = 2.5f;

// The real hardware doesn't space its 4 brightness levels evenly (0, 1/3,
// 2/3, 1) - it's levels 0x00/0x63/0x87 out of a 0xff full intensity (level 3
// is whatever colorR/G/B is currently set to, since that already stands in
// for "full brightness" everywhere else in this app - the tint applied to
// the actual game screen).
constexpr float kBrightnessLevels[4] = {0.0f, 0x63 / 255.0f, 0x87 / 255.0f, 1.0f};

// The palette's colors, right-aligned in the row's value area.
void DrawSwatches(UiRenderer &ui, const XrColor4f *colors, int count, float rowX, float rowY, float rowW, float rowH, float alpha)
{
    float x = rowX + rowW - MenuList::kValueRightPad - count * (kSwatchSize + kSwatchGap) + kSwatchGap;
    const float y = rowY + (rowH - kSwatchSize) / 2.0f;
    for (int i = 0; i < count; ++i)
    {
        ui.DrawQuadRounded(x, y, kSwatchSize, kSwatchSize, XrColor4f{colors[i].r, colors[i].g, colors[i].b, alpha}, 2.5f);
        x += kSwatchSize + kSwatchGap;
    }
}

void DrawColorPreview(AppSettings *settings, UiRenderer &ui, float rowX, float rowY, float rowW, float rowH, float alpha)
{
    if (!settings)
        return;
    XrColor4f colors[5];
    int count = 0;
    // Auto: its layers' colors, far to near, and its sprites' red - what the
    // screen mostly shows (see AutoColors.h).
    if (settings->selectedShadePalette == kAutoColors)
    {
        const AutoColors::Rgb swatches[5] = {AutoColors::kLayerRamps[0][1], AutoColors::kLayerRamps[1][1],
                                             AutoColors::kLayerRamps[2][1], AutoColors::kLayerRamps[3][1],
                                             AutoColors::kSpriteRamps[0][1]};
        for (const AutoColors::Rgb &c : swatches)
            colors[count++] = XrColor4f{c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, 1.0f};
    }
    // A per-shade palette made from a gradient picks its shades' colors from
    // all 5 stops, so those show; otherwise its own 4 colors.
    else if (settings->selectedShadePalette >= 0 && settings->selectedShadePalette < kShadePaletteCount &&
             GradientOfShadePalette(settings->selectedShadePalette) >= 0)
    {
        for (const XrColor4f &stop : kScreenPatterns[GradientOfShadePalette(settings->selectedShadePalette)])
            colors[count++] = stop;
    }
    else if (settings->selectedShadePalette >= 0 && settings->selectedShadePalette < kShadePaletteCount)
    {
        for (const XrColor4f &shade : kShadePalettes[settings->selectedShadePalette])
            colors[count++] = shade;
    }
    // A pattern's 5 gradient stops - the same kScreenPatterns the screen
    // shader reads (see Settings.h).
    else if (settings->selectedPattern >= 0 && settings->selectedPattern < kScreenPatternCount)
    {
        for (const XrColor4f &stop : kScreenPatterns[settings->selectedPattern])
            colors[count++] = stop;
    }
    else
    {
        for (int i = 0; i < 4; ++i)
        {
            const float level = kBrightnessLevels[i];
            colors[count++] = XrColor4f{settings->colorR * level, settings->colorG * level, settings->colorB * level, 1.0f};
        }
    }
    DrawSwatches(ui, colors, count, rowX, rowY, rowW, rowH, alpha);
}
} // namespace

void SettingsPage::Init(UiRenderer &ui, const UiMenuResources &resources)
{
    m_settings = resources.settings;
    m_platform = resources.platform;
    m_emulator = resources.emulator;
    m_library = resources.thumbnails;

    auto list = MakeList(ui, resources);

    list->AddHeader("Controls");
    list->AddEntry("Button mapping", [this](MenuItem *)
                   { if (emulatorButtonMapPage) Navigate(emulatorButtonMapPage, 1); }, nullptr, nullptr, UiIconId::Mapping)
        ->opensPage = true;
    // (the flat desktop window has no screen to place)
    if (resources.buttonMappingProfile != ButtonMappingProfile::Desktop)
        list->AddEntry("Adjust screen", [this](MenuItem *)
                       { if (moveScreenPage) Navigate(moveScreenPage, 1); }, nullptr, nullptr, UiIconId::Move)
            ->opensPage = true;

    // Color mode picks the kind of coloring (see ColorMode in
    // SettingsPage.h); Palette then only cycles that mode's presets, so each
    // list stays short and the modes are discoverable by name.
    list->AddHeader("Colors");
    m_colorModeEntry = list->AddEntry("Color mode", [this](MenuItem *) { ChangeColorMode(1); }, // Select acts like Right
        [this](MenuItem *) { ChangeColorMode(-1); }, [this](MenuItem *) { ChangeColorMode(1); }, UiIconId::Palette);
    m_paletteEntry = list->AddEntry("Palette", [this](MenuItem *) { ChangePalette(1); }, // Select acts like Right - advance the palette
        [this](MenuItem *) { ChangePalette(-1); }, [this](MenuItem *) { ChangePalette(1); }, UiIconId::None,
        [this](UiRenderer &ui, float x, float y, float w, float h, float a) { DrawColorPreview(m_settings, ui, x, y, w, h, a); });
    m_paletteEntry->reserveIconSpace = true; // indented under Color mode, like the R/G/B rows
    m_colorREntry = list->AddEntry("Red", [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorR, kColorStep); },
        [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorR, -kColorStep); },
        [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorR, kColorStep); });
    m_colorGEntry = list->AddEntry("Green", [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorG, kColorStep); },
        [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorG, -kColorStep); },
        [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorG, kColorStep); });
    m_colorBEntry = list->AddEntry("Blue", [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorB, kColorStep); },
        [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorB, -kColorStep); },
        [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorB, kColorStep); });
    m_colorREntry->reserveIconSpace = true;
    m_colorGEntry->reserveIconSpace = true;
    m_colorBEntry->reserveIconSpace = true;

    // How the game screen is drawn (see shaders/screen_filter.frag).
    list->AddHeader("Screen");
    m_lookEntry = list->AddEntry("Look", [this](MenuItem *) { ChangeLook(1); }, [this](MenuItem *) { ChangeLook(-1); },
                                 [this](MenuItem *) { ChangeLook(1); }, UiIconId::FlatScreen);
    if (resources.buttonMappingProfile == ButtonMappingProfile::Desktop)
    {
        // The PC window: as big as fits, or whole multiples of the pixels.
        auto toggleSize = [this](MenuItem *)
        {
            m_settings->screenWholePixels = !m_settings->screenWholePixels;
            RefreshLabels();
        };
        m_sizeEntry = list->AddEntry("Size", toggleSize, toggleSize, toggleSize, UiIconId::Scale);
    }

    list->AddHeader("Library");
    // Box art from libretro's thumbnail collection instead of title screens
    // (on by default; a game without one gets its title screen).
    auto boxArt = list->AddEntry("Download box art", [this](MenuItem *)
                                 {
                                     m_settings->downloadBoxArt = !m_settings->downloadBoxArt;
                                     m_settings->Save(*m_platform);
                                     if (m_library)
                                         m_library->SetBoxArt(m_settings->downloadBoxArt);
                                 },
                                 nullptr, nullptr, UiIconId::Save);
    boxArt->toggle = [this]() { return m_settings && m_settings->downloadBoxArt; };
    // (also the library's own filter chip)
    auto onlyPacks = list->AddEntry("Only games with color packs", [this](MenuItem *)
                                    {
                                        m_settings->libraryOnlyPacks = !m_settings->libraryOnlyPacks;
                                        m_settings->Save(*m_platform);
                                        if (m_library)
                                            m_library->SetOnlyPacks(m_settings->libraryOnlyPacks);
                                    },
                                    nullptr, nullptr, UiIconId::Palette);
    onlyPacks->toggle = [this]() { return m_settings && m_settings->libraryOnlyPacks; };
    m_rebuildEntry = list->AddEntry("Make thumbnails again", [this](MenuItem *)
                                    { if (m_library) m_library->RebuildAll(); }, nullptr, nullptr, UiIconId::Reset);
    if (m_platform->SupportsChangeRomsFolder())
    {
        // Way back into the ROMs-folder picker (SAF, on Android). Clears the
        // folder and asks for a restart rather than re-popping the picker
        // directly (see Platform::RequestChangeRomsFolder).
        m_changeRomsFolderEntry = list->AddEntry("Change ROMs folder", [this](MenuItem *) { RequestChangeRomsFolder(); },
            nullptr, nullptr, UiIconId::RomList);
    }

    m_menu.MenuItems.push_back(list);
    m_menu.Init();

    RefreshLabels(false);
}

std::string SettingsPage::Subtitle() const
{
    if (!m_emulator || m_emulator->RomName().empty())
        return "";
    const std::string &name = m_emulator->RomName();
    return "Colors for " + name.substr(0, name.find(" ("));
}

void SettingsPage::Update(uint32_t *buttonState, uint32_t *lastButtonState, float deltaSeconds)
{
    MenuPage::Update(buttonState, lastButtonState, deltaSeconds);
    if (m_rebuildEntry && m_library)
    {
        const char *value = m_library->Remaining() > 0 ? "Making..." : "";
        if (m_rebuildEntry->value != value)
            m_rebuildEntry->SetValue(value);
    }
}

SettingsPage::ColorMode SettingsPage::CurrentColorMode() const
{
    // Derived from which palette field is set rather than stored, so it can
    // never disagree with what's actually being drawn.
    if (m_settings->selectedShadePalette == kAutoColors)
        return ColorMode::Auto;
    if (m_settings->selectedShadePalette >= 0)
        return ColorMode::Multicolor;
    if (m_settings->selectedPattern >= 0)
        return ColorMode::Gradient;
    return ColorMode::Tint;
}

void SettingsPage::ChangeColorMode(int delta)
{
    if (!m_settings)
        return;

    // Remember the palette being left, so cycling back to its mode returns
    // to it (this session only - the active one is what's saved to disk).
    const ColorMode current = CurrentColorMode();
    if (current == ColorMode::Gradient)
        m_lastPattern = m_settings->selectedPattern;
    else if (current == ColorMode::Multicolor)
        m_lastShadePalette = m_settings->selectedShadePalette;

    const int next = (static_cast<int>(current) + delta + kColorModeCount) % kColorModeCount;
    // Tint needs nothing set: colorR/G/B (and selectedPalette) are kept
    // untouched while another mode is active, so the previous tint returns.
    m_settings->selectedPattern = next == static_cast<int>(ColorMode::Gradient) ? m_lastPattern : -1;
    m_settings->selectedShadePalette = next == static_cast<int>(ColorMode::Multicolor) ? m_lastShadePalette
                                       : next == static_cast<int>(ColorMode::Auto)     ? kAutoColors
                                                                                        : -1;
    RefreshLabels();
}

void SettingsPage::ChangePalette(int delta)
{
    if (!m_settings)
        return;
    // Cycles only the presets of the current Color Mode - see ChangeColorMode.
    switch (CurrentColorMode())
    {
    case ColorMode::Tint:
    {
        // -1 = a custom R/G/B color - start cycling from the first preset.
        int index = m_settings->selectedPalette >= 0 ? m_settings->selectedPalette : 0;
        index = (index + delta + kPredefColorCount) % kPredefColorCount;
        m_settings->selectedPalette = index;
        m_settings->colorR = kPredefColors[index].r;
        m_settings->colorG = kPredefColors[index].g;
        m_settings->colorB = kPredefColors[index].b;
        break;
    }
    case ColorMode::Gradient:
        m_settings->selectedPattern = (m_settings->selectedPattern + delta + kScreenPatternCount) % kScreenPatternCount;
        break;
    case ColorMode::Multicolor:
        m_settings->selectedShadePalette =
            (m_settings->selectedShadePalette + delta + kShadePaletteCount) % kShadePaletteCount;
        break;
    case ColorMode::Auto:
        break; // no presets - the row just shows what Auto uses
    }
    RefreshLabels();
}

void SettingsPage::ChangeColorChannel(float AppSettings::*channel, float delta)
{
    if (!m_settings)
        return;
    float &value = m_settings->*channel;
    value += delta;
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    m_settings->selectedPalette = -1; // diverges from whatever preset was selected, matches FrontendGo
    m_settings->selectedPattern = -1; // R/G/B rows are only reachable in tint mode anyway, but stay defensive
    m_settings->selectedShadePalette = -1;
    RefreshLabels();
}

void SettingsPage::ChangeLook(int delta)
{
    if (!m_settings)
        return;
    m_settings->screenLook = (m_settings->screenLook + delta + kScreenLookCount) % kScreenLookCount;
    RefreshLabels();
}

void SettingsPage::RequestChangeRomsFolder()
{
    m_platform->RequestChangeRomsFolder();
    if (m_changeRomsFolderEntry)
        m_changeRomsFolderEntry->SetText("Folder cleared - restart the app!");
}

void SettingsPage::OnShow()
{
    RefreshLabels(false); // (a game's own colors may have loaded since)
}

void SettingsPage::RefreshLabels(bool save)
{
    if (!m_settings)
        return;

    // The Palette row draws the actual colors via DrawColorPreview instead
    // of a selected-index number (see AddEntry's accessoryDraw above).
    static constexpr const char *kModeNames[kColorModeCount] = {"Tint", "Gradient", "Multicolor", "Auto"};
    m_colorModeEntry->SetValue(kModeNames[static_cast<int>(CurrentColorMode())]);
    static constexpr const char *kLookNames[kScreenLookCount] = {"Sharp", "Smooth", "LED"};
    m_lookEntry->SetValue(kLookNames[std::clamp(m_settings->screenLook, 0, kScreenLookCount - 1)]);
    if (m_sizeEntry)
        m_sizeEntry->SetValue(m_settings->screenWholePixels ? "Whole pixels" : "Fit");
    // 2 decimals, not 3 - kColorStep is 0.05, so the third decimal is always
    // 0 and never actually reachable by adjusting the value.
    m_colorREntry->SetValue(FormatFloat("", m_settings->colorR, 2));
    m_colorGEntry->SetValue(FormatFloat("", m_settings->colorG, 2));
    m_colorBEntry->SetValue(FormatFloat("", m_settings->colorB, 2));
    // Auto has no presets to step through.
    const bool autoMode = CurrentColorMode() == ColorMode::Auto;
    if (autoMode != !m_paletteEntry->leftFunction)
    {
        if (autoMode)
            m_paletteEntry->leftFunction = m_paletteEntry->rightFunction = nullptr;
        else
        {
            m_paletteEntry->leftFunction = [this](MenuItem *) { ChangePalette(-1); };
            m_paletteEntry->rightFunction = [this](MenuItem *) { ChangePalette(1); };
        }
    }
    // A gradient pattern or per-shade palette has no R/G/B of its own to
    // adjust - hide those rows entirely while one's selected (see
    // MenuList::Entry::Visible).
    const bool tintMode = CurrentColorMode() == ColorMode::Tint;
    m_colorREntry->Visible = tintMode;
    m_colorGEntry->Visible = tintMode;
    m_colorBEntry->Visible = tintMode;

    if (!save)
        return;
    m_settings->Save(*m_platform); // always-on autosave - no explicit save action anywhere in the menu anymore
    // ...and remembered for the game being played (see AppSettings::SaveGameColors),
    // whose thumbnail then shows them too.
    if (m_emulator)
    {
        m_settings->SaveGameColors(*m_platform, m_emulator->RomName());
        if (m_library)
            m_library->Recheck(m_emulator->RomName());
    }
}
