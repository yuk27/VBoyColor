#include "menu/MenuPage.h"
#include "menu/pages/AppMenuLayout.h"

void MenuPage::Draw(UiRenderer &ui, int transitionDirX, int transitionDirY, float moveProgress, float moveDist,
                    float fadeProgress)
{
    const float offsetX = transitionDirX * moveProgress * moveDist;
    const float offsetY = transitionDirY * moveProgress * moveDist;
    const std::string title = m_resources ? Title() : std::string();
    if (!title.empty())
    {
        const UiFontHandle titleFont = m_resources->titleFont, subFont = m_resources->cardFont;
        const float titleY = kPageTitleY + kPageTitleHeight / 2.0f - ui.GetFontPHeight(titleFont) / 2.0f -
                             ui.GetFontPStart(titleFont);
        XrColor4f color = kMenuTextColor;
        color.a *= fadeProgress;
        ui.DrawText(titleFont, title, kContentX + offsetX, titleY + offsetY, 1.0f, color);
        const std::string subtitle = Subtitle();
        if (!subtitle.empty())
        {
            // On the title's baseline.
            const float baseline = titleY + ui.GetFontPStart(titleFont) + ui.GetFontPHeight(titleFont);
            const float subY = baseline - ui.GetFontPStart(subFont) - ui.GetFontPHeight(subFont);
            XrColor4f dim = kMenuDimTextColor;
            dim.a *= fadeProgress;
            ui.DrawText(subFont, subtitle, kContentX + ui.GetTextWidth(titleFont, title) + 8.0f + offsetX, subY + offsetY,
                        1.0f, dim);
        }
    }
    m_menu.Draw(ui, transitionDirX, transitionDirY, moveProgress, moveDist, fadeProgress);
}

std::shared_ptr<MenuList> MenuPage::MakeList(UiRenderer &ui, const UiMenuResources &resources, float x, float width)
{
    auto list = std::make_shared<MenuList>(ui, resources.bodyFont, x > 0 ? x : kContentX, kContentTop,
                                           width > 0 ? width : kContentWidth, kContentHeight, kRowHeight, resources.icons);
    list->SetFonts(resources.bodyBoldFont, resources.captionBoldFont);
    list->Color = kMenuTextColor;
    list->SelectionColor = kMenuSelectionColor;
    return list;
}
