#pragma once
#include "menu/MenuPage.h"

#include <memory>

class MenuList;
struct AppSettings;
class Platform;
class Emulator;
class ThumbnailLibrary;

// Settings, in groups: Controls (Button mapping, and Adjust screen in the
// headset), Colors (the VB screen colors - a Color mode (tint / gradient /
// multicolor / auto), that mode's Palette presets, and the custom R/G/B
// tint) and Library (make the thumbnails again, and Change ROMs folder on
// platforms where Platform::SupportsChangeRomsFolder() is true). Every
// change autosaves immediately (see RefreshLabels).
class SettingsPage : public MenuPage
{
public:
    MenuPage *emulatorButtonMapPage = nullptr;
    MenuPage *moveScreenPage = nullptr;

    void Init(UiRenderer &ui, const UiMenuResources &resources) override;
    void OnShow() override;
    std::string Title() const override { return "Settings"; }
    std::string Subtitle() const override;
    void Update(uint32_t *buttonState, uint32_t *lastButtonState, float deltaSeconds) override;

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
    std::shared_ptr<MenuList::Entry> m_paletteEntry;
    std::shared_ptr<MenuList::Entry> m_rebuildEntry;
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
    ThumbnailLibrary *m_library = nullptr;
};
