#include "menu/pages/SettingsPage.h"
#include "emu/AutoColors.h"
#include "emu/Emulator.h"
#include "io/Platform.h"
#include "io/Settings.h"
#include "menu/MenuPage.h"
#include "menu/pages/AppMenuLayout.h"

#include <cstdio>

namespace
{
std::string FormatFloat(const char *prefix, float value, int precision = 3, const char *suffix = "")
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s%.*f%s", prefix, precision, value, suffix);
    return buf;
}

// Preview swatches drawn directly on the "Color Palette" row, immediately
// after its icon+label (not right-aligned against the row edge), showing
// the VB's 4 brightness levels (black up to the full tint) - stands in for
// the selected-palette index (nobody reads "7/11"; the colors themselves
// are what matters). Reads AppSettings live each frame, so it tracks
// palette/R/G/B edits immediately.
constexpr float kSwatchSize = 11.0f;
constexpr float kSwatchGap = 2.0f;
constexpr float kSwatchLeftGap = 6.0f;
constexpr const char *kColorPaletteLabel = "Color Palette";

// The real hardware doesn't space its 4 brightness levels evenly (0, 1/3,
// 2/3, 1) - it's levels 0x00/0x63/0x87 out of a 0xff full intensity (level 3
// is whatever colorR/G/B is currently set to, since that already stands in
// for "full brightness" everywhere else in this app - the tint applied to
// the actual game screen).
constexpr float kBrightnessLevels[4] = {0.0f, 0x63 / 255.0f, 0x87 / 255.0f, 1.0f};

void DrawColorPreview(UiRenderer &ui, UiFontHandle labelFont, AppSettings *settings, float rowX, float rowY, float rowW, float rowH, float alpha)
{
    if (!settings)
        return;

    const float labelWidth = ui.GetTextWidth(labelFont, kColorPaletteLabel);
    float x = rowX + MenuList::kIconSize + MenuList::kIconTextGap + labelWidth + kSwatchLeftGap;
    const float y = rowY + (rowH - kSwatchSize) / 2.0f;

    // Auto: its layers' colors, far to near, and its sprites' red - what the
    // screen mostly shows (see AutoColors.h).
    if (settings->selectedShadePalette == kAutoColors)
    {
        const AutoColors::Rgb swatches[5] = {AutoColors::kLayerRamps[0][1], AutoColors::kLayerRamps[1][1],
                                             AutoColors::kLayerRamps[2][1], AutoColors::kLayerRamps[3][1],
                                             AutoColors::kSpriteRamps[0][1]};
        for (const AutoColors::Rgb &c : swatches)
        {
            ui.DrawQuadRounded(x, y, kSwatchSize, kSwatchSize, XrColor4f{c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, alpha}, 1.0f);
            x += kSwatchSize + kSwatchGap;
        }
        return;
    }
    // A per-shade palette's own 4 colors - exactly what each shade shows on
    // screen at full brightness (see ShadeColorizer). One made from a
    // gradient picks its shades' colors from all 5 stops, so those show.
    if (settings->selectedShadePalette >= 0 && settings->selectedShadePalette < kShadePaletteCount &&
        GradientOfShadePalette(settings->selectedShadePalette) >= 0)
    {
        for (const XrColor4f &stop : kScreenPatterns[GradientOfShadePalette(settings->selectedShadePalette)])
        {
            ui.DrawQuadRounded(x, y, kSwatchSize, kSwatchSize, XrColor4f{stop.r, stop.g, stop.b, alpha}, 1.0f);
            x += kSwatchSize + kSwatchGap;
        }
        return;
    }
    if (settings->selectedShadePalette >= 0 && settings->selectedShadePalette < kShadePaletteCount)
    {
        for (const XrColor4f &shade : kShadePalettes[settings->selectedShadePalette])
        {
            ui.DrawQuadRounded(x, y, kSwatchSize, kSwatchSize, XrColor4f{shade.r, shade.g, shade.b, alpha}, 1.0f);
            x += kSwatchSize + kSwatchGap;
        }
        return;
    }

    // A pattern's 5 gradient stops stand in for the 4 tint brightness
    // swatches below - same kScreenPatterns array the screen shader itself
    // reads (see Settings.h), so this always matches what's actually drawn.
    if (settings->selectedPattern >= 0 && settings->selectedPattern < kScreenPatternCount)
    {
        for (const XrColor4f &stop : kScreenPatterns[settings->selectedPattern])
        {
            ui.DrawQuadRounded(x, y, kSwatchSize, kSwatchSize, XrColor4f{stop.r, stop.g, stop.b, alpha}, 1.0f);
            x += kSwatchSize + kSwatchGap;
        }
        return;
    }

    for (int i = 0; i < 4; ++i)
    {
        const float level = kBrightnessLevels[i];
        const XrColor4f c{settings->colorR * level, settings->colorG * level, settings->colorB * level, alpha};
        ui.DrawQuadRounded(x, y, kSwatchSize, kSwatchSize, c, 1.0f);
        x += kSwatchSize + kSwatchGap;
    }
}
} // namespace

void SettingsPage::Init(UiRenderer &ui, const UiMenuResources &resources)
{
    m_settings = resources.settings;
    m_platform = resources.platform;
    m_emulator = resources.emulator;

    auto list = std::make_shared<MenuList>(ui, resources.menuFont, kMenuContentX, kMenuContentY, kListWidth, kListHeight,
                                           kMenuItemSize, resources.icons);
    list->Color = kMenuTextColor;
    list->SelectionColor = kMenuSelectionColor;
    list->HighlightColor = kMenuHighlightColor;

    list->AddEntry("Button Mapping", [this](MenuItem *)
                   { if (emulatorButtonMapPage) Navigate(emulatorButtonMapPage, 1); }, nullptr, nullptr, UiIconId::Mapping);
    list->AddEntry("Adjust Screen", [this](MenuItem *)
                   { if (moveScreenPage) Navigate(moveScreenPage, 1); }, nullptr, nullptr, UiIconId::Move);

    list->AddSpacer(kMenuSpacerSize);

    UiFontHandle menuFont = resources.menuFont;
    // Color Mode picks the kind of coloring (see ColorMode in
    // SettingsPage.h); Color Palette then only cycles that mode's presets,
    // so each list stays short and the modes are discoverable by name.
    m_colorModeEntry = list->AddEntry("Color Mode: Tint", [this](MenuItem *) { ChangeColorMode(1); }, // Select acts like Right
        [this](MenuItem *) { ChangeColorMode(-1); }, [this](MenuItem *) { ChangeColorMode(1); }, UiIconId::Palette);
    auto paletteEntry = list->AddEntry(kColorPaletteLabel, [this](MenuItem *) { ChangePalette(1); }, // Select acts like Right - advance the palette
        [this](MenuItem *) { ChangePalette(-1); }, [this](MenuItem *) { ChangePalette(1); }, UiIconId::None,
        [this, menuFont](UiRenderer &ui, float x, float y, float w, float h, float a) { DrawColorPreview(ui, menuFont, m_settings, x, y, w, h, a); });
    paletteEntry->reserveIconSpace = true; // indented under Color Mode, like the R/G/B rows
    m_colorREntry = list->AddEntry("Red: 1.00", [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorR, kColorStep); },
        [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorR, -kColorStep); },
        [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorR, kColorStep); });
    m_colorGEntry = list->AddEntry("Green: 1.00", [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorG, kColorStep); },
        [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorG, -kColorStep); },
        [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorG, kColorStep); });
    m_colorBEntry = list->AddEntry("Blue: 1.00", [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorB, kColorStep); },
        [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorB, -kColorStep); },
        [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorB, kColorStep); });
    m_colorREntry->reserveIconSpace = true;
    m_colorGEntry->reserveIconSpace = true;
    m_colorBEntry->reserveIconSpace = true;

    if (m_platform->SupportsChangeRomsFolder())
    {
        // Way back into the ROMs-folder picker (SAF, on Android). Clears the
        // folder and asks for a restart rather than re-popping the picker
        // directly (see Platform::RequestChangeRomsFolder).
        list->AddSpacer(kMenuSpacerSize);
        m_changeRomsFolderEntry = list->AddEntry("Change ROMs Folder...", [this](MenuItem *) { RequestChangeRomsFolder(); },
            nullptr, nullptr, UiIconId::RomList);
    }

    list->AddSpacer(kMenuSpacerSize);
    auto aboutEntry = list->AddEntry("About VBoy Color", [this](MenuItem *)
                                     { if (aboutPage) Navigate(aboutPage, 1); });
    aboutEntry->reserveIconSpace = true;

    m_menu.MenuItems.push_back(list);

    // Version string, bottom-right, small/secondary font - not part of the
    // scrollable list (non-selectable, doesn't participate in navigation).
    // MenuLabel centers text within its box, so measure the actual text
    // width and size the box to match - that's what makes it right-aligned
    // against kMenuContentX instead of just centered somewhere near it.
    const float versionWidth = ui.GetTextWidth(resources.smallFont, kVersionString) + 0.5f;
    constexpr float kVersionHeight = 8.0f;
    auto versionLabel = std::make_shared<MenuLabel>(
        ui, resources.smallFont, kVersionString,
        kMenuWidth - 5.0f - versionWidth, kMenuHeight - kBottomHeight - kVersionHeight - 4.0f,
        versionWidth, kVersionHeight, kMenuVersionColor);
    m_menu.MenuItems.push_back(versionLabel);

    m_menu.BackPress = [this]()
    { if (mainPage) Navigate(mainPage, -1); };
    m_menu.Init();

    RefreshLabels();
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

    // Color Palette's label stays static ("Color Palette") - its row draws
    // the actual colors via DrawColorPreview instead of a selected-index
    // number (see AddEntry's accessoryDraw above).
    static constexpr const char *kModeNames[kColorModeCount] = {"Tint", "Gradient", "Multicolor", "Auto"};
    m_colorModeEntry->SetText(std::string("Color Mode: ") + kModeNames[static_cast<int>(CurrentColorMode())]);
    // 2 decimals, not 3 - kColorStep is 0.05, so the third decimal is always
    // 0 and never actually reachable by adjusting the value.
    m_colorREntry->SetText(FormatFloat("Red: ", m_settings->colorR, 2));
    m_colorGEntry->SetText(FormatFloat("Green: ", m_settings->colorG, 2));
    m_colorBEntry->SetText(FormatFloat("Blue: ", m_settings->colorB, 2));
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
    // ...and remembered for the game being played (see AppSettings::SaveGameColors).
    if (m_emulator)
        m_settings->SaveGameColors(*m_platform, m_emulator->RomName());
}
