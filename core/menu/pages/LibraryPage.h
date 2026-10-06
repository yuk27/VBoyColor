#pragma once
#include "menu/MenuPage.h"

#include <memory>

class LibraryGrid;
class ThumbnailLibrary;
class Emulator;
class AppMenu;
struct AppSettings;
class Platform;

// The library: every ROM in the ROMs folder as a card with its thumbnail
// (see ThumbnailLibrary, LibraryGrid) - or as a list (Y). Picking one loads
// it with its colors and starts it. Without a ROMs folder (Android, after
// "Change ROMs folder") or without ROMs, it says so instead.
class LibraryPage : public MenuPage
{
public:
    void Init(UiRenderer &ui, const UiMenuResources &resources) override;
    void OnShow() override;
    void ResetSelection() override {}
    std::string Title() const override { return "Library"; }
    std::string Subtitle() const override;
    std::vector<MenuHint> Hints() const override;
    void Update(uint32_t *buttonState, uint32_t *lastButtonState, float deltaSeconds) override;
    void Draw(UiRenderer &ui, int transitionDirX, int transitionDirY, float moveProgress, float moveDist,
              float fadeProgress) override;
    void HandlePointer(const MenuPointer &pointer) override;

private:
    void Play(int game);
    void ToggleView();
    bool HasGames() const;

    std::shared_ptr<LibraryGrid> m_grid;
    std::shared_ptr<MenuList> m_emptyList; // (no ROMs folder / no ROMs)
    std::shared_ptr<MenuList::Entry> m_pickEntry;
    ThumbnailLibrary *m_library = nullptr;
    Emulator *m_emulator = nullptr;
    AppMenu *m_appMenu = nullptr;
    AppSettings *m_settings = nullptr;
    Platform *m_platform = nullptr;
};
