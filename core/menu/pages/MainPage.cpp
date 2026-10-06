#include "menu/pages/MainPage.h"
#include "emu/Emulator.h"
#include "io/Settings.h"
#include "menu/AppMenu.h"
#include "menu/MenuPage.h"
#include "menu/pages/AppMenuLayout.h"

namespace
{
    constexpr float kPreviewWidth = Emulator::kPreviewWidth * 0.5f;   // 192
    constexpr float kPreviewHeight = Emulator::kPreviewHeight * 0.5f; // 112
    constexpr float kPreviewGap = 8.0f;
    constexpr float kListWidthMain = kListWidth - kPreviewGap - kPreviewWidth;
    constexpr float kPreviewX = kMenuContentX + kListWidthMain + kPreviewGap;
    constexpr float kPreviewY = kMenuContentY + (kListHeight - kPreviewHeight) / 2.0f;
}

void MainPage::Init(UiRenderer &ui, const UiMenuResources &resources)
{
    auto list = std::make_shared<MenuList>(ui, resources.menuFont, kMenuContentX, kMenuContentY, kListWidthMain,
                                           kListHeight, kMenuItemSize, resources.icons);
    list->Color = kMenuTextColor;
    list->SelectionColor = kMenuSelectionColor;
    list->HighlightColor = kMenuHighlightColor;

    AppMenu *appMenu = resources.appMenu;
    m_emulator = resources.emulator;

    list->AddEntry("Resume", [appMenu](MenuItem *)
                   { if (appMenu) appMenu->Hide(); }, nullptr, nullptr, UiIconId::Resume);
    list->AddEntry("Reset", [this, appMenu](MenuItem *)
                   {
        if (m_emulator && m_emulator->ResetGame() && appMenu)
            appMenu->Hide(); }, nullptr, nullptr, UiIconId::Reset);

    list->AddSpacer(kMenuSpacerSize);

    m_saveSlotEntry = list->AddEntry("Save Slot: " + std::to_string(m_saveSlot), [this](MenuItem *)
                                     { ChangeSaveSlot(1); }, // Select acts like Right - advance the slot
                                     [this](MenuItem *)
                                     { ChangeSaveSlot(-1); }, [this](MenuItem *)
                                     { ChangeSaveSlot(1); }, UiIconId::SaveSlot);
    list->AddEntry("Save", [this](MenuItem *)
                   {
        if (m_emulator)
        {
            m_emulator->SaveState(m_saveSlot);
            RefreshSavePreview();
        } }, nullptr, nullptr, UiIconId::Save);
    list->AddEntry("Load", [this, appMenu](MenuItem *)
                   {
                       if (!m_emulator || !m_emulator->SaveStateExists(m_saveSlot))
                           return; // nothing to load in this slot - do nothing rather than load garbage
                       m_emulator->LoadState(m_saveSlot);
                       if (appMenu)
                           appMenu->Hide(); // nothing left to do in the menu once a state is loaded - back to gameplay
                   },
                   nullptr, nullptr, UiIconId::Load);

    list->AddSpacer(kMenuSpacerSize);

    m_loadRomEntry = list->AddEntry("Load ROM", [this](MenuItem *)
                                    { if (romSelectPage) Navigate(romSelectPage, 1); }, nullptr, nullptr, UiIconId::RomList);

    list->AddSpacer(kMenuSpacerSize);

    list->AddEntry("Settings", [this](MenuItem *)
                   { if (settingsPage)  Navigate(settingsPage,  1); }, nullptr, nullptr, UiIconId::Settings);

    m_menu.MenuItems.push_back(list);

    AppSettings *settings = resources.settings;
    m_preview = std::make_shared<MenuImage>(ui, resources.smallFont, Emulator::kPreviewWidth, Emulator::kPreviewHeight,
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

    m_menu.BackPress = [appMenu]() { if (appMenu) appMenu->Hide(); };
    m_menu.Init();

    RefreshSavePreview();
}

void MainPage::ResetSelection()
{
    MenuPage::ResetSelection();
    RefreshSavePreview();
}

void MainPage::SelectLoadRomEntry()
{
    if (m_loadRomEntry)
        m_loadRomEntry->Select();
}

void MainPage::ChangeSaveSlot(int delta)
{
    m_saveSlot += delta;
    if (m_saveSlot < kMinSaveSlot)
        m_saveSlot = kMaxSaveSlot;
    else if (m_saveSlot > kMaxSaveSlot)
        m_saveSlot = kMinSaveSlot;

    m_saveSlotEntry->SetText("Save Slot: " + std::to_string(m_saveSlot));
    RefreshSavePreview();
}

void MainPage::RefreshSavePreview()
{
    if (!m_emulator || !m_preview)
        return;

    std::vector<uint8_t> rgba;
    if (m_emulator->LoadStatePreview(m_saveSlot, rgba))
        m_preview->SetImage(rgba);
    else
        m_preview->Clear();
}
