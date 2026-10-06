#include "menu/pages/AboutPage.h"
#include "menu/pages/AppMenuLayout.h"

#include <memory>
#include <string>

void AboutPage::Init(UiRenderer &ui, const UiMenuResources &resources)
{
    // The one selectable row (Back), first: the menu starts on item 0.
    constexpr float kBackHeight = kMenuItemSize;
    auto list = std::make_shared<MenuList>(ui, resources.menuFont, kMenuContentX, kMenuContentY + kListHeight - kBackHeight,
                                           kListWidth, kBackHeight, kMenuItemSize, resources.icons);
    list->Color = kMenuTextColor;
    list->SelectionColor = kMenuSelectionColor;
    list->HighlightColor = kMenuHighlightColor;
    auto back = list->AddEntry("Back", [this](MenuItem *)
                               { if (settingsPage) Navigate(settingsPage, -1); });
    back->centered = true;
    m_menu.MenuItems.push_back(list);

    struct Line
    {
        const char *text;
        bool small;
        XrColor4f color;
    };
    const std::string title = std::string("VBoy Color ") + kVersionString;
    const XrColor4f dim = kMenuVersionColor;
    const Line lines[] = {
        {title.c_str(), false, kMenuSelectionColor},
        {"A free, open-source Virtual Boy emulator", false, kMenuTextColor},
        {"for Meta Quest and PC - GPL-3.0", false, kMenuTextColor},
        {"", true, dim},
        {"Started from VirtualBoyGo by CidVonHighwind", false, kMenuTextColor},
        {"Emulation: Beetle VB (Mednafen, libretro)", false, kMenuTextColor},
        {"Shade colors: an idea from Red Viper", false, kMenuTextColor},
        {"", true, dim},
        {"Not affiliated with Nintendo or Meta.", true, dim},
        {"github.com/yuk27/VBoyColor", true, dim},
        {"Support it: github.com/sponsors/yuk27", true, dim},
    };
    float y = kMenuContentY + 4.0f;
    for (const Line &line : lines)
    {
        const UiFontHandle font = line.small ? resources.smallFont : resources.menuFont;
        const float height = line.small ? 11.0f : line.color.r == kMenuSelectionColor.r && line.color.g == kMenuSelectionColor.g ? 17.0f : 13.5f;
        if (line.text[0])
            m_menu.MenuItems.push_back(
                std::make_shared<MenuLabel>(ui, font, line.text, kMenuContentX, y, kListWidth, height, line.color));
        y += line.text[0] ? height : 5.0f;
    }

    m_menu.BackPress = [this]()
    { if (settingsPage) Navigate(settingsPage, -1); };
    m_menu.Init();
}
