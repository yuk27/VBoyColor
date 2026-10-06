#include "menu/pages/SaveStatesPage.h"
#include "emu/Emulator.h"
#include "io/Settings.h"
#include "menu/AppMenu.h"
#include "menu/pages/AppMenuLayout.h"

namespace
{
    // The slot's picture on the right, the list on the left.
    constexpr float kListWidthSaves = 104.0f;
    constexpr float kPreviewGap = 10.0f;
    constexpr float kPreviewX = kContentX + kListWidthSaves + kPreviewGap;
    constexpr float kPreviewWidth = kContentRight - kPreviewX;
    constexpr float kPreviewHeight = kPreviewWidth * Emulator::kPreviewHeight / Emulator::kPreviewWidth;
    constexpr float kPreviewY = kContentTop + kGroupHeaderHeight + 2.0f;
}

void SaveStatesPage::Init(UiRenderer &ui, const UiMenuResources &resources)
{
    auto list = MakeList(ui, resources, kContentX, kListWidthSaves);
    AppMenu *appMenu = resources.appMenu;
    m_emulator = resources.emulator;

    list->AddHeader("Slot");
    m_saveSlotEntry = list->AddEntry("Slot", [this](MenuItem *) { ChangeSaveSlot(1); }, // Select acts like Right - advance the slot
                                     [this](MenuItem *) { ChangeSaveSlot(-1); }, [this](MenuItem *) { ChangeSaveSlot(1); },
                                     UiIconId::SaveSlot);
    m_saveEntry = list->AddEntry("Save", [this](MenuItem *)
                                 {
                                     if (m_emulator)
                                     {
                                         m_emulator->SaveState(m_saveSlot);
                                         RefreshSavePreview();
                                     }
                                 },
                                 nullptr, nullptr, UiIconId::Save);
    m_loadEntry = list->AddEntry("Load", [this, appMenu](MenuItem *)
                                 {
                                     if (!m_emulator || !m_emulator->SaveStateExists(m_saveSlot))
                                         return; // nothing to load in this slot - do nothing rather than load garbage
                                     m_emulator->LoadState(m_saveSlot);
                                     if (appMenu)
                                         appMenu->Hide(); // nothing left to do in the menu once a state is loaded - back to gameplay
                                 },
                                 nullptr, nullptr, UiIconId::Load);

    list->AddHeader("Game");
    list->AddEntry("Reset", [this, appMenu](MenuItem *)
                   {
                       if (m_emulator && m_emulator->ResetGame() && appMenu)
                           appMenu->Hide();
                   },
                   nullptr, nullptr, UiIconId::Reset);
    m_menu.MenuItems.push_back(list);

    AppSettings *settings = resources.settings;
    m_preview = std::make_shared<MenuImage>(ui, resources.cardFont, Emulator::kPreviewWidth, Emulator::kPreviewHeight,
                                            kPreviewX, kPreviewY, kPreviewWidth, kPreviewHeight,
                                            // Save previews are stored as plain luminance, so with a
                                            // per-shade palette active (ScreenTint() white) they show
                                            // in grayscale rather than in the palette's colors.
                                            [settings]() -> XrColor4f
                                            {
                                                return settings ? settings->ScreenTint() : XrColor4f{1.0f, 1.0f, 1.0f, 1.0f};
                                            },
                                            [settings]() -> int
                                            { return settings ? settings->ScreenPattern() : -1; });
    m_menu.MenuItems.push_back(m_preview);
    m_menu.Init();

    ChangeSaveSlot(0);
}

std::string SaveStatesPage::Subtitle() const
{
    if (!m_emulator)
        return "";
    const std::string &name = m_emulator->RomName();
    return name.substr(0, name.find(" (")); // (without its region)
}

void SaveStatesPage::ResetSelection()
{
    MenuPage::ResetSelection();
    RefreshSavePreview();
}

void SaveStatesPage::OnShow() { RefreshSavePreview(); }

void SaveStatesPage::ChangeSaveSlot(int delta)
{
    m_saveSlot += delta;
    if (m_saveSlot < kMinSaveSlot)
        m_saveSlot = kMaxSaveSlot;
    else if (m_saveSlot > kMaxSaveSlot)
        m_saveSlot = kMinSaveSlot;

    m_saveSlotEntry->SetValue(std::to_string(m_saveSlot));
    RefreshSavePreview();
}

void SaveStatesPage::RefreshSavePreview()
{
    if (!m_emulator || !m_preview)
        return;

    std::vector<uint8_t> rgba;
    if (m_emulator->LoadStatePreview(m_saveSlot, rgba))
        m_preview->SetImage(rgba);
    else
        m_preview->Clear();
}
