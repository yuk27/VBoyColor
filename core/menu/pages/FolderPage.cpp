#include "menu/pages/FolderPage.h"
#include "io/Platform.h"
#include "menu/pages/AppMenuLayout.h"

namespace
{
    // Path strings as the platform hands them over (UTF-8, '/' or '\').
    bool IsSeparator(char c) { return c == '/' || c == '\\'; }

    std::string TrimSeparators(std::string path)
    {
        // (but keep a root: "/", "C:\")
        while (path.size() > 1 && IsSeparator(path.back()) && !(path.size() == 3 && path[1] == ':'))
            path.pop_back();
        return path;
    }

    // The folder above, or "" at a root.
    std::string Parent(const std::string &path)
    {
        const std::string p = TrimSeparators(path);
        if (p == "/" || (p.size() <= 3 && p.size() >= 2 && p[1] == ':'))
            return "";
        const size_t at = p.find_last_of("/\\");
        if (at == std::string::npos)
            return "";
        if (at == 0)
            return "/";
        if (at == 2 && p[1] == ':')
            return p.substr(0, 3);
        return p.substr(0, at);
    }

    std::string Join(const std::string &path, const std::string &name)
    {
        const char sep = path.find('\\') != std::string::npos ? '\\' : '/';
        return path.empty() || IsSeparator(path.back()) ? path + name : path + sep + name;
    }

    // The last part of a path ("Games" of "C:\Users\me\Games").
    std::string LastPart(const std::string &path)
    {
        const std::string p = TrimSeparators(path);
        const size_t at = p.find_last_of("/\\");
        return at == std::string::npos || at + 1 >= p.size() ? p : p.substr(at + 1);
    }

    // A long path's end, to fit beside the title.
    std::string Shorten(const std::string &path, size_t max = 44)
    {
        if (path.size() <= max)
            return path;
        size_t at = path.size() - max + 3;
        while (at < path.size() && !IsSeparator(path[at]))
            ++at;
        return "..." + path.substr(at < path.size() ? at : path.size() - max + 3);
    }
} // namespace

void FolderPage::Init(UiRenderer &ui, const UiMenuResources &resources)
{
    m_platform = resources.platform;
    m_list = MakeList(ui, resources);
    m_menu.MenuItems.push_back(m_list);
    m_menu.Init();
    m_menu.BackPress = [this]() { if (settingsPage) Navigate(settingsPage, -1); };
}

std::string FolderPage::Title() const
{
    return std::string(DataKindName(m_kind)) + " folder";
}

std::string FolderPage::Subtitle() const
{
    return m_platform && m_platform->BrowsesDataFolders() ? Shorten(m_browsePath) : "Settings";
}

void FolderPage::Open(DataKind kind)
{
    m_kind = kind;
    m_status.clear();
    m_browsePath = m_platform ? m_platform->DataFolderPath(kind) : "";
}

void FolderPage::Update(uint32_t *buttonState, uint32_t *lastButtonState, float deltaSeconds)
{
    MenuPage::Update(buttonState, lastButtonState, deltaSeconds);
    // (a folder the system's picker set meanwhile - checked now and then)
    m_sinceCheck += deltaSeconds;
    if (m_sinceCheck > 0.5f && m_platform)
    {
        m_sinceCheck = 0.0f;
        if (m_platform->DataFolderLabel(m_kind) != m_builtLabel)
            m_rebuild = true;
        if (std::string status = m_platform->TakeDataFolderStatus(); !status.empty())
        {
            m_status = status;
            m_rebuild = true;
            if (onChanged)
                onChanged(m_kind);
        }
    }
    if (m_rebuild)
        Rebuild(); // (not from inside the row that asked - Rebuild replaces the rows)
}

void FolderPage::Browse(const std::string &path)
{
    m_browsePath = path;
    m_rebuild = true;
}

void FolderPage::Choose(const std::string &path)
{
    if (!m_platform)
        return;
    const std::string result = m_platform->SetDataFolder(m_kind, path);
    m_status = result.empty() ? "Already there" : result;
    if (!result.empty() && onChanged)
        onChanged(m_kind);
    m_rebuild = true;
}

void FolderPage::Rebuild()
{
    m_rebuild = false;
    if (!m_list || !m_platform)
        return;
    m_list->Clear();

    const std::string label = m_platform->DataFolderLabel(m_kind);
    const bool custom = m_kind == DataKind::Games ? false : !label.empty();
    const char *defaultPlace = m_kind == DataKind::Games ? "" : (m_kind == DataKind::Saves ? "In the games folder" : "In the games folder's States");

    m_list->AddHeader("Now");
    auto now = m_list->AddEntry(custom || m_kind == DataKind::Games ? Shorten(label, 52) : defaultPlace, nullptr, nullptr, nullptr,
                                UiIconId::RomList);
    if (!m_status.empty())
        m_list->AddEntry(m_status)->reserveIconSpace = true;

    if (!m_platform->BrowsesDataFolders())
    {
        // The system's folder picker.
        m_list->AddHeader("Change");
        m_list->AddEntry("Choose a folder", [this](MenuItem *)
                         {
                             m_status = m_platform->PickDataFolder(m_kind);
                             m_rebuild = true;
                         },
                         nullptr, nullptr, UiIconId::Load);
        if (custom)
            m_list->AddEntry(m_kind == DataKind::Saves ? "Back in the games folder" : "Back in the games folder's States",
                             [this](MenuItem *) { Choose(""); }, nullptr, nullptr, UiIconId::Reset);
    }
    else
    {
        m_list->AddHeader("Change");
        m_list->AddEntry("Use this folder", [this](MenuItem *) { Choose(m_browsePath); }, nullptr, nullptr, UiIconId::Save)
            ->SetValue(Shorten(LastPart(m_browsePath), 24));
        if (custom)
            m_list->AddEntry(m_kind == DataKind::Saves ? "Back in the games folder" : "Back in the games folder's States",
                             [this](MenuItem *) { Choose(""); }, nullptr, nullptr, UiIconId::Reset);
        else if (m_kind == DataKind::Games && !label.empty() && label != "roms")
            m_list->AddEntry("Back to roms, next to VBoy Color", [this](MenuItem *) { Choose(""); }, nullptr, nullptr,
                             UiIconId::Reset);

        // The folders here, and up.
        m_list->AddHeader("Folders in " + Shorten(LastPart(m_browsePath), 28));
        const std::string parent = Parent(m_browsePath);
        if (!parent.empty())
            m_list->AddEntry("Up one level", [this, parent](MenuItem *) { Browse(parent); }, nullptr, nullptr, UiIconId::Back);
        const std::vector<std::string> folders = m_platform->ListFolders(m_browsePath);
        for (const std::string &name : folders)
        {
            const std::string path = Join(m_browsePath, name);
            m_list->AddEntry(name, [this, path](MenuItem *) { Browse(path); }, nullptr, nullptr, UiIconId::RomList)->opensPage =
                true;
        }
        if (folders.empty())
            m_list->AddEntry("(no folders inside)")->reserveIconSpace = true;

        m_list->AddHeader("Places");
        for (const auto &[name, path] : m_platform->FolderPlaces())
        {
            const std::string to = path;
            m_list->AddEntry(name, [this, to](MenuItem *) { Browse(to); }, nullptr, nullptr, UiIconId::Move)->opensPage = true;
        }
    }

    m_list->ResetSelection();
    m_builtLabel = label;
    (void)now;
}
