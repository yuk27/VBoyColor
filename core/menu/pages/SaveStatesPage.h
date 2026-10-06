#pragma once
#include "menu/MenuPage.h"

#include <memory>

class MenuList;
class MenuImage;
class Emulator;

// Save states for the game being played: pick a slot (its picture shows
// beside the list), save to it, load it - and reset the game.
class SaveStatesPage : public MenuPage
{
public:
    void Init(UiRenderer &ui, const UiMenuResources &resources) override;
    void ResetSelection() override;
    void OnShow() override;
    std::string Title() const override { return "Save states"; }
    std::string Subtitle() const override;

private:
    static constexpr int kMinSaveSlot = 0;
    static constexpr int kMaxSaveSlot = 9; // 10 slots total (0-9), matches FrontendGo's saveStates[10]

    void ChangeSaveSlot(int delta);
    void RefreshSavePreview();

    std::shared_ptr<MenuList::Entry> m_saveSlotEntry;
    std::shared_ptr<MenuList::Entry> m_saveEntry;
    std::shared_ptr<MenuList::Entry> m_loadEntry;
    std::shared_ptr<MenuImage> m_preview;
    Emulator *m_emulator = nullptr;
    int m_saveSlot = kMinSaveSlot;
};
