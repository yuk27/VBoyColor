#pragma once
#include "io/DataFolders.h"
#include "menu/MenuPage.h"

#include <functional>
#include <memory>
#include <string>

class Platform;

// Settings > Folders > one kind of file (see io/DataFolders.h): where it
// goes now, and choosing another folder - on the desktop apps by browsing
// the computer's folders right here (mouse, gamepad or headset alike); on
// Android with the system's folder picker. Moving a kind copies its files
// the new folder doesn't have yet; the originals stay.
class FolderPage : public MenuPage
{
public:
    MenuPage *settingsPage = nullptr;
    // Something changed (SettingsPage refreshes its rows, the library
    // rescans the games folder...).
    std::function<void(DataKind)> onChanged;

    void Init(UiRenderer &ui, const UiMenuResources &resources) override;
    std::string Title() const override;
    std::string Subtitle() const override;
    void Update(uint32_t *buttonState, uint32_t *lastButtonState, float deltaSeconds) override;
    void OnShow() override { Rebuild(); }

    // Shows a kind - call before navigating here.
    void Open(DataKind kind);

private:
    void Rebuild();
    void Browse(const std::string &path); // (from a row: rebuilt after the list's update)
    void Choose(const std::string &path); // "" = the default place

    Platform *m_platform = nullptr;
    std::shared_ptr<MenuList> m_list;
    DataKind m_kind = DataKind::Saves;
    std::string m_browsePath;
    std::string m_status;
    std::string m_builtLabel; // the folder shown as "Now"
    float m_sinceCheck = 0.0f;
    bool m_rebuild = false;
};
