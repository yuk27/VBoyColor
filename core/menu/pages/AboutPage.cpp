#include "menu/pages/AboutPage.h"
#include "menu/pages/AppMenuLayout.h"

#include <memory>
#include <string>
#include <vector>

namespace
{
    // Lines of text on rounded cards, top to bottom.
    class TextCards : public MenuItem
    {
    public:
        struct Line
        {
            std::string text;
            UiFontHandle font;
            XrColor4f color;
        };
        // An empty line ends a card (the next starts after a gap).
        std::vector<Line> lines;

        void Draw(UiRenderer &ui, float offsetX, float offsetY, float alpha) override
        {
            constexpr float kLineHeight = 12.5f, kPad = 6.0f;
            float y = kContentTop + offsetY;
            size_t i = 0;
            while (i < lines.size())
            {
                size_t end = i;
                while (end < lines.size() && !lines[end].text.empty())
                    ++end;
                const float height = (end - i) * kLineHeight + kPad * 2.0f;
                XrColor4f card = kMenuCardColor;
                card.a *= alpha;
                ui.DrawQuadRounded(kContentX + offsetX, y, kContentWidth, height, card, kCardRadius);
                float lineY = y + kPad;
                for (; i < end; ++i)
                {
                    const Line &line = lines[i];
                    XrColor4f c = line.color;
                    c.a *= alpha;
                    const float textY = lineY + kLineHeight / 2.0f - ui.GetFontPHeight(line.font) / 2.0f -
                                        ui.GetFontPStart(line.font);
                    ui.DrawText(line.font, line.text, kContentX + 10.0f + offsetX, textY, 1.0f, c);
                    lineY += kLineHeight;
                }
                y += height + kGroupGap;
                ++i; // (the empty line)
            }
        }
    };
} // namespace

void AboutPage::Init(UiRenderer &ui, const UiMenuResources &resources)
{
    kVersion = std::string("VBoy Color ") + kVersionString;
    const UiFontHandle body = resources.bodyFont, bold = resources.bodyBoldFont, small = resources.captionFont;
    auto cards = std::make_shared<TextCards>();
    cards->lines = {
        {"A free, open-source Virtual Boy emulator", bold, kMenuTextColor},
        {"for Meta Quest and PC - GPL-3.0", body, kMenuTextColor},
        {"Emulation: Beetle VB (Mednafen, libretro)", body, kMenuTextColor},
        {"", body, kMenuTextColor},
        {"Not affiliated with Nintendo or Meta in any way.", small, kMenuDimTextColor},
        {"github.com/yuk27/VBoyColor", small, kMenuDimTextColor},
        {"", body, kMenuTextColor},
        {"Thanks", bold, kMenuSelectionColor},
        {"VirtualBoyGo by CidVonHighwind - where the inspiration started", body, kMenuTextColor},
        {"Red Viper - whose colors inspired our gradients", body, kMenuTextColor},
    };
    for (const auto &line : cards->lines)
        ui.EnsureGlyphsForText(line.font, line.text);
    ui.EnsureGlyphsForText(resources.cardFont, kVersion);
    m_menu.MenuItems.push_back(cards);
    m_menu.Init();
}
