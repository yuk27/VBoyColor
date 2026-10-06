#pragma once
#include "menu/MenuPage.h"

#include <memory>

class MenuList;
struct AppSettings;
class Platform;
class Emulator;

// Settings: Button Mapping, Adjust Screen, and the VB screen colors - a
// Color Mode (tint / gradient / multicolor), that mode's Color Palette
// presets, and the custom R/G/B tint - plus a version label (and Change ROMs Folder, on
// platforms where Platform::SupportsChangeRomsFolder() is true). Screen
// placement/view settings live in the Adjust Screen page. Every change
// autosaves immediately (see RefreshLabels); Back is the bottom-bar B hint
// (see MenuPage::HasBackAction).
class SettingsPage : public MenuPage
{
public:
    MenuPage *mainPage = nullptr;
    MenuPage *emulatorButtonMapPage = nullptr;
    MenuPage *moveScreenPage = nullptr;
    MenuPage *aboutPage = nullptr;

    void Init(UiRenderer &ui, const UiMenuResources &resources) override;
    void OnShow() override;

private:
    static constexpr float kColorStep = 0.05f; // matches FrontendGo's COLOR_STEP_SIZE

    // The Color Mode row's choices - which kind of palette the Color
    // Palette row cycles: Tint = flat single color (kPredefColors + custom
    // R/G/B), Gradient = multi-hue brightness gradient (kScreenPatterns),
    // Multicolor = per-shade colors (kShadePalettes, Red Viper-style).
    enum class ColorMode
    {
        Tint,
        Gradient,
        Multicolor,
        Auto // colors by layer and sprite (kAutoColors) - the default
    };
    static constexpr int kColorModeCount = 4;

    ColorMode CurrentColorMode() const;
    void ChangeColorMode(int delta);
    void ChangePalette(int delta);
    void ChangeColorChannel(float AppSettings::*channel, float delta);
    // save: also autosave settings.dat and the game's colors (a change).
    void RefreshLabels(bool save = true);
    void RequestChangeRomsFolder();

    std::shared_ptr<MenuList::Entry> m_colorModeEntry;
    std::shared_ptr<MenuList::Entry> m_colorREntry;
    std::shared_ptr<MenuList::Entry> m_colorGEntry;
    std::shared_ptr<MenuList::Entry> m_colorBEntry;
    std::shared_ptr<MenuList::Entry> m_changeRomsFolderEntry;
    // Last palette used in each non-Tint mode this session (see
    // ChangeColorMode) - first visit starts at each list's first preset.
    int m_lastPattern = 0;
    int m_lastShadePalette = 0;
    AppSettings *m_settings = nullptr;
    Platform *m_platform = nullptr;
    Emulator *m_emulator = nullptr;
};
