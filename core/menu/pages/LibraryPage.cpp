#include "menu/pages/LibraryPage.h"
#include "emu/Emulator.h"
#include "io/Platform.h"
#include "io/Settings.h"
#include "menu/AppMenu.h"
#include "menu/ThumbnailLibrary.h"
#include "menu/pages/AppMenuLayout.h"
#include "menu/pages/LibraryGrid.h"

#include <algorithm>

namespace
{
    // The list/cards switch, top right, and the sort chip left of it.
    constexpr float kChipW = 36.0f, kChipH = 13.0f;
    constexpr float kChipX = kContentRight - kChipW;
    constexpr float kChipY = kPageTitleY + (kPageTitleHeight - kChipH) / 2.0f;
    constexpr float kSortW = 40.0f;
    constexpr float kSortX = kChipX - 4.0f - kSortW;
    constexpr const char *kSortAz = "A\xE2\x80\x93Z";
    constexpr const char *kSortRecent = "Recent";
    constexpr const char *kMakingText = " \xC2\xB7 making thumbnails\xE2\x80\xA6";
    constexpr const char *kDownloadingText = " \xC2\xB7 getting box art\xE2\x80\xA6";

    XrColor4f WithAlpha(XrColor4f c, float alpha)
    {
        c.a *= alpha;
        return c;
    }
} // namespace

void LibraryPage::Init(UiRenderer &ui, const UiMenuResources &resources)
{
    m_library = resources.thumbnails;
    m_emulator = resources.emulator;
    m_appMenu = resources.appMenu;
    m_settings = resources.settings;
    m_platform = resources.platform;
    ui.EnsureGlyphsForText(resources.cardFont, kMakingText);
    ui.EnsureGlyphsForText(resources.cardFont, kDownloadingText);

    if (HasGames())
    {
        m_grid = std::make_shared<LibraryGrid>(ui, resources, *m_library, kContentX, kContentTop, kContentWidth,
                                               kContentHeight);
        m_grid->SetListView(m_settings && m_settings->libraryListView);
        m_grid->onPlay = [this](int game) { Play(game); };
        m_menu.MenuItems.push_back(m_grid);
        m_menu.YPress = [this]() { ToggleView(); };
        m_menu.XPress = [this]() { ToggleSort(); };
        ui.EnsureGlyphsForText(resources.cardFont, std::string(kSortAz) + kSortRecent);
    }
    else
    {
        m_emptyList = MakeList(ui, resources);
        Platform *platform = m_platform;
        if (!platform->HasRomsFolder())
        {
            // Only reachable if "Change ROMs folder" (Settings) cleared the
            // folder this session - picking again needs an app restart.
            m_emptyList->AddHeader("No ROMs folder");
            m_pickEntry = m_emptyList->AddEntry("Pick ROMs folder", [this, platform](MenuItem *)
                                                {
                                                    platform->RequestChangeRomsFolder();
                                                    m_pickEntry->SetText("Folder cleared - restart the app!");
                                                },
                                                nullptr, nullptr, UiIconId::RomList);
        }
        else
        {
            m_emptyList->AddHeader("No games yet");
            const bool desktop = resources.buttonMappingProfile == ButtonMappingProfile::Desktop;
            m_emptyList->AddEntry(desktop ? "Put .vb ROMs in the roms folder next to VBoy Color" : "Put .vb ROMs in your ROMs folder",
                                  nullptr, nullptr, nullptr, UiIconId::VbCartridge);
            m_emptyList->AddEntry(desktop ? "...or drop one on this window" : "...then restart the app", nullptr, nullptr,
                                  nullptr, UiIconId::None)
                ->reserveIconSpace = true;
        }
        m_menu.MenuItems.push_back(m_emptyList);
    }
    m_menu.Init();
}

bool LibraryPage::HasGames() const { return m_library && !m_library->Games().empty(); }

std::string LibraryPage::Subtitle() const
{
    if (!HasGames())
        return "";
    const size_t count = m_library->Games().size();
    std::string text = std::to_string(count) + (count == 1 ? " game" : " games");
    if (m_library->Making())
        text += kMakingText;
    else if (m_library->Downloading())
        text += kDownloadingText;
    return text;
}

std::vector<MenuHint> LibraryPage::Hints() const
{
    const char *back = m_emulator && m_emulator->HasGame() ? "Resume" : "Back";
    if (!m_grid)
        return {{UiIconId::ButtonA, "Select"}, {UiIconId::ButtonB, back}};
    return {{UiIconId::ButtonA, "Play"},
            {UiIconId::ButtonB, back},
            {UiIconId::ButtonY, m_grid->IsListView() ? "Card view" : "List view"},
            {UiIconId::ButtonX, m_library->IsSortRecent() ? "A\xE2\x80\x93Z" : "Recent first"}};
}

void LibraryPage::OnShow()
{
    if (m_grid)
        m_grid->RefreshLabels(); // (the order may have changed: a game was played)
    // On the game being played, if any.
    if (m_grid && m_emulator && m_library)
    {
        const int index = m_library->IndexOf(m_emulator->RomName());
        if (index >= 0)
            m_grid->SetSelectedGame(index, true);
    }
}

void LibraryPage::Update(uint32_t *buttonState, uint32_t *lastButtonState, float deltaSeconds)
{
    MenuPage::Update(buttonState, lastButtonState, deltaSeconds);
}

void LibraryPage::Play(int game)
{
    if (!m_library || game < 0 || game >= static_cast<int>(m_library->Games().size()) || !m_emulator)
        return;
    const RomEntry rom = m_library->Games()[game].rom;
    if (m_emulator->LoadRom(rom.fullPath, rom.name))
    {
        if (m_settings)
            m_settings->ApplyGameColors(*m_platform, m_emulator->RomName(), m_emulator->RomCrc()); // this game's colors
        if (m_appMenu)
            m_appMenu->Hide(); // straight to the game
    }
}

void LibraryPage::ToggleView()
{
    if (!m_grid)
        return;
    m_grid->SetListView(!m_grid->IsListView());
    if (m_settings)
    {
        m_settings->libraryListView = m_grid->IsListView();
        m_settings->Save(*m_platform);
    }
}

void LibraryPage::ToggleSort()
{
    if (!m_grid || !m_library)
        return;
    // (the selection stays on the same game)
    const auto &games = m_library->Games();
    const std::string selected = m_grid->SelectedGame() < static_cast<int>(games.size())
                                     ? games[m_grid->SelectedGame()].rom.name
                                     : std::string();
    m_library->SetSortRecent(!m_library->IsSortRecent());
    m_grid->RefreshLabels();
    m_grid->SetSelectedGame(std::max(0, m_library->IndexOf(selected)), true);
    if (m_settings)
    {
        m_settings->librarySortRecent = m_library->IsSortRecent();
        m_settings->Save(*m_platform);
    }
}

void LibraryPage::Draw(UiRenderer &ui, int transitionDirX, int transitionDirY, float moveProgress, float moveDist,
                       float fadeProgress)
{
    MenuPage::Draw(ui, transitionDirX, transitionDirY, moveProgress, moveDist, fadeProgress);
    if (!m_grid || !m_resources)
        return;
    const float ox = transitionDirX * moveProgress * moveDist, oy = transitionDirY * moveProgress * moveDist;
    const float a = fadeProgress;
    // Sort: A-Z or Recent.
    {
        const char *label = m_library && m_library->IsSortRecent() ? kSortRecent : kSortAz;
        const UiFontHandle font = m_resources->cardFont;
        ui.DrawQuadRounded(kSortX + ox, kChipY + oy, kSortW, kChipH, WithAlpha(kMenuCardColor, a), kChipH / 2.0f);
        const float tw = ui.GetTextWidth(font, label);
        ui.DrawText(font, label, kSortX + (kSortW - tw) / 2.0f + ox,
                    kChipY + kChipH / 2.0f - ui.GetFontPHeight(font) / 2.0f - ui.GetFontPStart(font) + oy, 1.0f,
                    WithAlpha(kMenuTextColor, a));
    }
    ui.DrawQuadRounded(kChipX + ox, kChipY + oy, kChipW, kChipH, WithAlpha(kMenuCardColor, a), kChipH / 2.0f);
    const bool list = m_grid->IsListView();
    constexpr float kHalf = kChipW / 2.0f, kIcon = 8.0f;
    const float activeX = kChipX + (list ? 0.0f : kHalf) + 1.0f;
    ui.DrawQuadRounded(activeX + ox, kChipY + 1.0f + oy, kHalf - 2.0f, kChipH - 2.0f,
                       WithAlpha({1.0f, 0.79f, 0.34f, 0.2f}, a), (kChipH - 2.0f) / 2.0f);
    if (m_resources->icons)
    {
        m_resources->icons->Draw(ui, UiIconId::RomList, kChipX + (kHalf - kIcon) / 2.0f + ox, kChipY + (kChipH - kIcon) / 2.0f + oy,
                                 kIcon, a, list ? kMenuSelectionColor : kMenuDimTextColor);
        m_resources->icons->Draw(ui, UiIconId::Palette, kChipX + kHalf + (kHalf - kIcon) / 2.0f + ox,
                                 kChipY + (kChipH - kIcon) / 2.0f + oy, kIcon, a, list ? kMenuDimTextColor : kMenuSelectionColor);
    }
}

void LibraryPage::HandlePointer(const MenuPointer &pointer)
{
    if (m_grid && pointer.clicked && pointer.x >= kSortX && pointer.x <= kSortX + kSortW && pointer.y >= kChipY &&
        pointer.y <= kChipY + kChipH)
    {
        ToggleSort();
        return;
    }
    if (m_grid && pointer.clicked && pointer.x >= kChipX && pointer.x <= kChipX + kChipW && pointer.y >= kChipY &&
        pointer.y <= kChipY + kChipH)
    {
        const bool wantList = pointer.x < kChipX + kChipW / 2.0f;
        if (wantList != m_grid->IsListView())
            ToggleView();
        return;
    }
    MenuPage::HandlePointer(pointer);
}
