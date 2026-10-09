#include "menu/MenuWidgets.h"
#include "io/Settings.h"
#include "menu/pages/AppMenuLayout.h"

#include <algorithm>
#include <cmath>

// ---- MenuItem ----

void MenuItem::Update(uint32_t *buttonState, uint32_t *lastButtonState, float deltaSeconds)
{
    if (UpdateFunction != nullptr)
        UpdateFunction(this, buttonState, lastButtonState);
}

int MenuItem::PressedUp() { return 0; }
int MenuItem::PressedDown() { return 0; }
int MenuItem::PressedLeft() { return 0; }
int MenuItem::PressedRight() { return 0; }
int MenuItem::PressedEnter() { return 0; }

void MenuItem::OnSelect(int direction)
{
    if (OnSelectFunction != nullptr)
        OnSelectFunction(this, direction);
}

void MenuItem::Select() { Selected = true; }
void MenuItem::Unselect() { Selected = false; }

void MenuItem::Draw(UiRenderer &ui, float offsetX, float offsetY, float alpha) {}

// ---- MenuLabel ----

MenuLabel::MenuLabel(UiRenderer &ui, UiFontHandle font, const std::string &text, float posX, float posY, float width, float height,
                     XrColor4f color)
    : m_ui(&ui), m_font(font), m_containerX(posX), m_containerY(posY), m_containerWidth(width), m_containerHeight(height)
{
    Color = color;
    SetText(text);
}

void MenuLabel::SetText(const std::string &newText)
{
    m_text = newText;
    // Bake any glyphs this text needs that aren't already in the atlas
    // (e.g. non-ASCII characters) before measuring/drawing - safe to do here
    // (not mid-frame), see UiFontManager::EnsureGlyphsForText.
    m_ui->EnsureGlyphsForText(m_font, newText);
    // Center text within the label's container, same as the original.
    const float textWidth = m_ui->GetTextWidth(m_font, newText);
    PosX = m_containerX + m_containerWidth / 2.0f - textWidth / 2.0f;
    PosY = m_containerY + m_containerHeight / 2.0f - m_ui->GetFontPHeight(m_font) / 2.0f - m_ui->GetFontPStart(m_font);
}

void MenuLabel::Draw(UiRenderer &ui, float offsetX, float offsetY, float alpha)
{
    if (!Visible)
        return;
    XrColor4f c = Color;
    c.a *= alpha;
    ui.DrawText(m_font, m_text, PosX + offsetX + (Selected ? 2.5f : 0.0f), PosY + offsetY, 1.0f, c);
}

// ---- MenuButton ----

MenuButton::MenuButton(UiRenderer &ui, UiFontHandle font, const std::string &text, float posX, float posY, float width, float height,
                       std::function<void(MenuItem *)> pressFunction, std::function<void(MenuItem *)> leftFunction,
                       std::function<void(MenuItem *)> rightFunction)
    : m_ui(&ui), m_font(font), m_pressFunction(pressFunction), m_leftFunction(leftFunction), m_rightFunction(rightFunction)
{
    PosX = posX;
    PosY = posY + (height / 2.0f - ui.GetFontPHeight(font) / 2.0f) - ui.GetFontPStart(font);
    m_containerWidth = width;
    Selectable = true;
    SetText(text);
}

MenuButton::MenuButton(UiRenderer &ui, UiFontHandle font, const std::string &text, float posX, float posY,
                       std::function<void(MenuItem *)> pressFunction, std::function<void(MenuItem *)> leftFunction,
                       std::function<void(MenuItem *)> rightFunction)
    : m_ui(&ui), m_font(font), m_pressFunction(pressFunction), m_leftFunction(leftFunction), m_rightFunction(rightFunction)
{
    PosX = posX;
    PosY = posY;
    Selectable = true;
    SetText(text);
}

void MenuButton::SetText(const std::string &newText)
{
    Text = newText;
    m_ui->EnsureGlyphsForText(m_font, newText); // see MenuLabel::SetText
    if (m_containerWidth > 0)
    {
        const float textWidth = m_ui->GetTextWidth(m_font, newText);
        m_offsetX = m_containerWidth / 2.0f - textWidth / 2.0f;
    }
}

int MenuButton::PressedLeft()
{
    if (m_leftFunction != nullptr)
    {
        m_leftFunction(this);
        return 1;
    }
    return 0;
}

int MenuButton::PressedRight()
{
    if (m_rightFunction != nullptr)
    {
        m_rightFunction(this);
        return 1;
    }
    return 0;
}

int MenuButton::PressedEnter()
{
    if (m_pressFunction != nullptr)
    {
        m_pressFunction(this);
        return 1;
    }
    return 0;
}

void MenuButton::Draw(UiRenderer &ui, float offsetX, float offsetY, float alpha)
{
    if (!Visible)
        return;
    XrColor4f c = Selected ? SelectionColor : Color;
    c.a *= alpha;
    ui.DrawText(m_font, Text, PosX + (Selected ? 2.5f : 0.0f) + offsetX + m_offsetX, PosY + offsetY, 1.0f, c);
}

// ---- MenuImage ----

namespace
{
    const std::string kMenuImageEmptyText = "Empty slot";
}

MenuImage::MenuImage(UiRenderer &ui, UiFontHandle font, uint32_t textureWidth, uint32_t textureHeight, float posX,
                     float posY, float width, float height, std::function<XrColor4f()> tintProvider,
                     std::function<int()> patternIndexProvider)
    : m_ui(&ui), m_font(font), m_width(width), m_height(height), m_tintProvider(std::move(tintProvider)),
      m_patternIndexProvider(std::move(patternIndexProvider))
{
    PosX = posX;
    PosY = posY;
    // _SRGB, not _UNORM - SetImage's bytes (Emulator::LoadStatePreview) are
    // gamma-encoded, and ui_image.frag/screen_pattern.frag expect their
    // source to auto-linearize on sample - see Emulator.cpp's
    // RETRO_ENVIRONMENT_SET_PIXEL_FORMAT comment for the full reasoning.
    m_texture = ui.CreateStreamingImage(textureWidth, textureHeight, VK_FORMAT_B8G8R8A8_SRGB);
    m_ui->EnsureGlyphsForText(m_font, kMenuImageEmptyText);
}

void MenuImage::SetImage(const std::vector<uint8_t> &rgba)
{
    m_ui->UpdateStreamingImage(m_texture, rgba.data(), rgba.size());
    m_hasImage = true;
}

void MenuImage::Clear() { m_hasImage = false; }

void MenuImage::Draw(UiRenderer &ui, float offsetX, float offsetY, float alpha)
{
    if (!Visible)
        return;

    const float x = PosX + offsetX;
    const float y = PosY + offsetY;

    // Rounded frame, matching the header/bottom bars - hollow, not filled:
    // the body-colored quad punches the middle back out so the frame only
    // ever shows as a border, whatever's drawn on top of it.
    constexpr float kFrameThickness = 2.5f;
    constexpr float kFrameRadius = 6.0f;
    XrColor4f frameColor = kMenuCardColor;
    frameColor.a *= alpha;
    ui.DrawQuadRounded(x - kFrameThickness, y - kFrameThickness, m_width + kFrameThickness * 2,
                       m_height + kFrameThickness * 2, frameColor, kFrameRadius);

    if (m_hasImage)
    {
        const int patternIndex = m_patternIndexProvider ? m_patternIndexProvider() : -1;
        if (patternIndex >= 0 && patternIndex < kScreenPatternCount)
        {
            ui.DrawImageRegionPattern(m_texture, x, y, m_width, m_height, 0.0f, 0.0f, 1.0f, 1.0f,
                                      kScreenPatterns[patternIndex], alpha);
        }
        else
        {
            XrColor4f tint = m_tintProvider ? m_tintProvider() : XrColor4f{1.0f, 1.0f, 1.0f, 1.0f};
            tint.a *= alpha;
            ui.DrawImage(m_texture, x, y, m_width, m_height, tint);
        }
        return;
    }

    const float textWidth = ui.GetTextWidth(m_font, kMenuImageEmptyText);
    const float textX = x + (m_width - textWidth) / 2.0f;
    const float textY = y + m_height / 2.0f - ui.GetFontPHeight(m_font) / 2.0f - ui.GetFontPStart(m_font);
    const XrColor4f textColor{kMenuDimTextColor.r, kMenuDimTextColor.g, kMenuDimTextColor.b, alpha};
    ui.DrawText(m_font, kMenuImageEmptyText, textX, textY, 1.0f, textColor);
}

// ---- Menu ----

void Menu::Init() { MenuItems[CurrentSelection]->Select(); }

bool Menu::HasSelectable() const
{
    for (const auto &item : MenuItems)
        if (item->Selectable)
            return true;
    return false;
}

void Menu::ResetSelection()
{
    MenuItems[CurrentSelection]->Unselect();

    CurrentSelection = 0;
    while (!MenuItems[CurrentSelection]->Selectable && CurrentSelection < static_cast<int>(MenuItems.size()) - 1)
        ++CurrentSelection;

    for (auto &item : MenuItems)
        item->ResetSelection();

    MenuItems[CurrentSelection]->Select();
}

bool Menu::ButtonPressed(uint32_t *buttonState, uint32_t *lastButtonState, uint32_t device, uint32_t button)
{
    return (buttonState[device] & ButtonMapper::ButtonMapping[button]) &&
           (!(lastButtonState[device] & ButtonMapper::ButtonMapping[button]) ||
            buttonDownCount > MenuItems[CurrentSelection]->ScrollDelay);
}

void Menu::MoveSelection(int dir, bool onSelect)
{
    if (!HasSelectable())
        return;
    do
    {
        CurrentSelection += dir;
        if (CurrentSelection < 0)
            CurrentSelection = static_cast<int>(MenuItems.size()) - 1;
        if (CurrentSelection >= static_cast<int>(MenuItems.size()))
            CurrentSelection = 0;
    } while (!MenuItems[CurrentSelection]->Selectable);

    if (onSelect)
        MenuItems[CurrentSelection]->OnSelect(dir);
}

void Menu::HandlePointer(const MenuPointer &pointer)
{
    for (int i = 0; i < static_cast<int>(MenuItems.size()); ++i)
    {
        MenuItem &item = *MenuItems[i];
        if (!item.Visible || !item.HandlePointer(pointer))
            continue;
        if (item.Selectable && i != CurrentSelection)
        {
            MenuItems[CurrentSelection]->Unselect();
            CurrentSelection = i;
            item.Select();
        }
        return;
    }
}

void Menu::Update(uint32_t *buttonState, uint32_t *lastButtonState, float deltaSeconds)
{
    using namespace ButtonMapper;

    if (CaptureHook)
    {
        // The frame that completes a capture must be consumed too. In
        // particular, binding Up/Down/Left/Right must not immediately feed
        // that same edge into normal menu navigation below.
        // Keep a local copy alive because a completing hook may clear the
        // member from inside its callback.
        const auto captureHook = CaptureHook;
        captureHook(buttonState, lastButtonState);
        return;
    }

    MenuItems[CurrentSelection]->Unselect();

    if ((buttonState[DeviceGamepad] &
         (ButtonMapping[EmuButton_Up] | ButtonMapping[EmuButton_Down] | ButtonMapping[EmuButton_Left] | ButtonMapping[EmuButton_Right] |
          ButtonMapping[EmuButton_LeftStickUp] | ButtonMapping[EmuButton_LeftStickDown] | ButtonMapping[EmuButton_LeftStickLeft] |
          ButtonMapping[EmuButton_LeftStickRight])) ||
        (buttonState[DeviceLeftTouch] &
         (ButtonMapping[EmuButton_Up] | ButtonMapping[EmuButton_Down] | ButtonMapping[EmuButton_Left] | ButtonMapping[EmuButton_Right])) ||
        (buttonState[DeviceRightTouch] &
         (ButtonMapping[EmuButton_Up] | ButtonMapping[EmuButton_Down] | ButtonMapping[EmuButton_Left] | ButtonMapping[EmuButton_Right])))
    {
        // (capped: one long frame - a hitch - mustn't count as holding the
        // button long enough to repeat)
        buttonDownCount += std::min(deltaSeconds, 0.05f);
    }
    else
    {
        buttonDownCount = 0;
    }

    for (auto &item : MenuItems)
    {
        item->Update(buttonState, lastButtonState, deltaSeconds);
    }

    // A fresh press (not a held repeat) of a button on any of these devices.
    auto freshPress = [&](std::initializer_list<std::pair<uint32_t, uint32_t>> buttons)
    {
        for (const auto &[device, button] : buttons)
            if ((buttonState[device] & ButtonMapping[button]) && !(lastButtonState[device] & ButtonMapping[button]))
                return true;
        return false;
    };

    // Left/Right adjust the selected item's own value (e.g. save slot +/-).
    // No ClearButtonState - clearing would make the held key look freshly
    // pressed every frame, breaking the ScrollDelay repeat throttle.
    if (ButtonPressed(buttonState, lastButtonState, DeviceGamepad, EmuButton_Left) ||
        ButtonPressed(buttonState, lastButtonState, DeviceGamepad, EmuButton_LeftStickLeft) ||
        ButtonPressed(buttonState, lastButtonState, DeviceLeftTouch, EmuButton_Left) ||
        ButtonPressed(buttonState, lastButtonState, DeviceRightTouch, EmuButton_Left))
    {
        buttonDownCount -= MenuItems[CurrentSelection]->ScrollTimeH;
        if (MenuItems[CurrentSelection]->PressedLeft() == 0 && LeftEdge &&
            freshPress({{DeviceGamepad, EmuButton_Left}, {DeviceGamepad, EmuButton_LeftStickLeft},
                        {DeviceLeftTouch, EmuButton_Left}, {DeviceRightTouch, EmuButton_Left}}))
        {
            const auto leftEdge = LeftEdge;
            leftEdge();
        }
    }

    if (ButtonPressed(buttonState, lastButtonState, DeviceGamepad, EmuButton_Right) ||
        ButtonPressed(buttonState, lastButtonState, DeviceGamepad, EmuButton_LeftStickRight) ||
        ButtonPressed(buttonState, lastButtonState, DeviceLeftTouch, EmuButton_Right) ||
        ButtonPressed(buttonState, lastButtonState, DeviceRightTouch, EmuButton_Right))
    {
        buttonDownCount -= MenuItems[CurrentSelection]->ScrollTimeH;
        MenuItems[CurrentSelection]->PressedRight();
    }

    const bool selectPressed = ButtonPressed(buttonState, lastButtonState, DeviceGamepad, EmuButton_A) ||
                               ButtonPressed(buttonState, lastButtonState, DeviceRightTouch, EmuButton_A);

    if (selectPressed)
    {
        buttonDownCount -= MenuItems[CurrentSelection]->ScrollTimeH;
        MenuItems[CurrentSelection]->PressedEnter();
    }
    else if (ButtonPressed(buttonState, lastButtonState, DeviceGamepad, EmuButton_B) ||
             ButtonPressed(buttonState, lastButtonState, DeviceRightTouch, EmuButton_B))
    {
        if (BackPress != nullptr)
        {
            const auto backPress = BackPress;
            backPress();
        }
    }

    if (YPress && freshPress({{DeviceGamepad, EmuButton_Y}, {DeviceLeftTouch, EmuButton_Y}, {DeviceRightTouch, EmuButton_Y}}))
    {
        const auto yPress = YPress;
        yPress();
    }
    if (XPress && freshPress({{DeviceGamepad, EmuButton_X}, {DeviceLeftTouch, EmuButton_X}, {DeviceRightTouch, EmuButton_X}}))
    {
        const auto xPress = XPress;
        xPress();
    }

    if (ButtonPressed(buttonState, lastButtonState, DeviceGamepad, EmuButton_Up) ||
        ButtonPressed(buttonState, lastButtonState, DeviceGamepad, EmuButton_LeftStickUp) ||
        ButtonPressed(buttonState, lastButtonState, DeviceLeftTouch, EmuButton_Up) ||
        ButtonPressed(buttonState, lastButtonState, DeviceRightTouch, EmuButton_Up))
    {
        buttonDownCount -= MenuItems[CurrentSelection]->ScrollTimeV;
        if (MenuItems[CurrentSelection]->PressedUp() == 0)
        {
            MoveSelection(-1, true);
        }
    }

    if (ButtonPressed(buttonState, lastButtonState, DeviceGamepad, EmuButton_Down) ||
        ButtonPressed(buttonState, lastButtonState, DeviceGamepad, EmuButton_LeftStickDown) ||
        ButtonPressed(buttonState, lastButtonState, DeviceLeftTouch, EmuButton_Down) ||
        ButtonPressed(buttonState, lastButtonState, DeviceRightTouch, EmuButton_Down))
    {
        buttonDownCount -= MenuItems[CurrentSelection]->ScrollTimeV;
        if (MenuItems[CurrentSelection]->PressedDown() == 0)
        {
            MoveSelection(1, true);
        }
    }

    MenuItems[CurrentSelection]->Select();
}

void Menu::Draw(UiRenderer &ui, int transitionDirX, int transitionDirY, float moveProgress, float moveDist, float fadeProgress)
{
    for (auto &item : MenuItems)
    {
        item->Draw(ui, transitionDirX * moveProgress * moveDist, transitionDirY * moveProgress * moveDist, fadeProgress);
    }
}

// ---- MenuList ----

namespace
{
    XrColor4f WithAlpha(XrColor4f c, float alpha)
    {
        c.a *= alpha;
        return c;
    }

    // An amber-outlined, faintly filled rounded rect - the selected row/card.
    void DrawSelectionFrame(UiRenderer &ui, float x, float y, float w, float h, float radius, float alpha,
                            const XrColor4f &fillUnder)
    {
        ui.DrawQuadRounded(x - 1.5f, y - 1.5f, w + 3.0f, h + 3.0f, WithAlpha({1.0f, 0.79f, 0.34f, 0.12f}, alpha), radius + 1.5f);
        ui.DrawQuadRounded(x, y, w, h, WithAlpha(kMenuSelectionColor, alpha), radius);
        ui.DrawQuadRounded(x + 1.1f, y + 1.1f, w - 2.2f, h - 2.2f, WithAlpha(fillUnder, alpha), radius - 1.1f);
        ui.DrawQuadRounded(x + 1.1f, y + 1.1f, w - 2.2f, h - 2.2f, WithAlpha(kMenuSelectionFillColor, alpha), radius - 1.1f);
    }

    float TextY(UiRenderer &ui, UiFontHandle font, float centerY)
    {
        return centerY - ui.GetFontPHeight(font) / 2.0f - ui.GetFontPStart(font);
    }
} // namespace

MenuList::MenuList(UiRenderer &ui, UiFontHandle font, float posX, float posY, float width, float height, float itemHeight,
                   const UiIconSet *icons)
    : m_ui(&ui), m_font(font), m_boldFont(font), m_headerFont(font), m_icons(icons), m_posX(posX), m_posY(posY),
      m_width(width), m_height(height)
{
    m_itemHeight = itemHeight;
    Selectable = true;
    Color = kMenuTextColor;
    SelectionColor = kMenuSelectionColor;
    ui.EnsureGlyphsForText(font, "\xE2\x80\xB9\xE2\x80\xBA\xE2\x80\xA6\xC2\xB0\xC2\xB7"); // the chevrons (and ellipsis, degree, dot)
}

void MenuList::SetFonts(UiFontHandle boldFont, UiFontHandle headerFont)
{
    m_boldFont = boldFont;
    m_headerFont = headerFont;
    m_ui->EnsureGlyphsForText(m_boldFont, "\xE2\x80\xB9\xE2\x80\xBA\xE2\x80\xA6\xC2\xB0\xC2\xB7");
    for (const auto &entry : m_entries)
    {
        m_ui->EnsureGlyphsForText(m_boldFont, entry->twoColumn ? entry->caption : entry->text);
        if (entry->isHeader)
            m_ui->EnsureGlyphsForText(m_headerFont, entry->text);
    }
}

void MenuList::Entry::SetText(const std::string &newText)
{
    text = newText;
    // Bake any glyphs the new label needs (a no-op for pure ASCII, which is
    // always pre-baked). See UiFontManager::EnsureGlyphsForText.
    if (m_owner)
    {
        m_owner->m_ui->EnsureGlyphsForText(m_owner->m_font, newText);
        m_owner->m_ui->EnsureGlyphsForText(m_owner->m_boldFont, newText);
    }
}

void MenuList::Entry::SetValue(const std::string &newValue)
{
    value = newValue;
    if (m_owner)
        m_owner->m_ui->EnsureGlyphsForText(m_owner->m_font, newValue);
}

void MenuList::Entry::SetCaption(const std::string &newCaption)
{
    caption = newCaption;
    if (m_owner)
    {
        m_owner->m_ui->EnsureGlyphsForText(m_owner->m_font, newCaption);
        m_owner->m_ui->EnsureGlyphsForText(m_owner->m_boldFont, newCaption);
    }
}

void MenuList::Entry::SetSecondaryText(const std::string &newText)
{
    textSecondary = newText;
    if (m_owner)
    {
        m_owner->m_ui->EnsureGlyphsForText(m_owner->m_font, newText);
        m_owner->m_ui->EnsureGlyphsForText(m_owner->m_boldFont, newText);
    }
}

void MenuList::Entry::Select()
{
    if (!m_owner)
        return;
    for (size_t i = 0; i < m_owner->m_entries.size(); ++i)
    {
        if (m_owner->m_entries[i].get() == this)
        {
            m_owner->SelectIndex(static_cast<int>(i));
            return;
        }
    }
}

std::shared_ptr<MenuList::Entry> MenuList::AddEntry(const std::string &text,
                                                    std::function<void(MenuItem *)> press,
                                                    std::function<void(MenuItem *)> left,
                                                    std::function<void(MenuItem *)> right,
                                                    UiIconId icon,
                                                    AccessoryDrawFn accessoryDraw)
{
    // Row text isn't known upfront (ROM file names, etc.) - bake whatever
    // glyphs it needs now, not while drawing. See UiFontManager::EnsureGlyphsForText.
    m_ui->EnsureGlyphsForText(m_font, text);
    m_ui->EnsureGlyphsForText(m_boldFont, text);
    auto entry = std::make_shared<Entry>();
    entry->text = text;
    entry->pressFunction = std::move(press);
    entry->leftFunction = std::move(left);
    entry->rightFunction = std::move(right);
    entry->icon = icon;
    entry->accessoryDraw = std::move(accessoryDraw);
    entry->m_owner = this;
    m_entries.push_back(entry);
    // (the selection starts on the first row, not a header above it)
    if (!IsRow(m_selectedIndex))
        m_selectedIndex = static_cast<int>(m_entries.size()) - 1;
    return entry;
}

std::shared_ptr<MenuList::Entry> MenuList::AddSpacer(float height)
{
    auto entry = std::make_shared<Entry>();
    entry->isSpacer = true;
    entry->height = height;
    entry->m_owner = this;
    m_entries.push_back(entry);
    return entry;
}

std::shared_ptr<MenuList::Entry> MenuList::AddHeader(const std::string &text)
{
    auto entry = std::make_shared<Entry>();
    entry->isHeader = true;
    // Small caps, as the headers are drawn.
    entry->text = text;
    for (char &c : entry->text)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    m_ui->EnsureGlyphsForText(m_headerFont, entry->text);
    entry->m_owner = this;
    m_entries.push_back(entry);
    return entry;
}

bool MenuList::IsRow(int index) const
{
    const Entry &entry = *m_entries[index];
    return !entry.isSpacer && !entry.isHeader && entry.Visible;
}

void MenuList::Layout() const
{
    // Headers and spacers start a new card; a gap between cards.
    m_top.assign(m_entries.size(), 0.0f);
    float y = 0;
    bool any = false, gap = false;
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        const Entry &entry = *m_entries[i];
        if (entry.isHeader)
        {
            if (any)
                y += kGroupGap;
            gap = false;
            m_top[i] = y;
            y += kGroupHeaderHeight;
            continue;
        }
        if (entry.isSpacer)
        {
            gap = any;
            m_top[i] = y;
            continue;
        }
        if (!entry.Visible)
        {
            m_top[i] = y;
            continue;
        }
        if (gap)
            y += kGroupGap;
        gap = false;
        m_top[i] = y;
        y += m_itemHeight;
        any = true;
    }
    m_contentHeight = y;
}

float MenuList::ContentHeight() const
{
    Layout();
    return m_contentHeight;
}

void MenuList::ScrollToSelection(bool instant)
{
    if (m_entries.empty())
        return;
    Layout();
    float top = m_top[m_selectedIndex];
    const float bottom = top + m_itemHeight;
    // The first row of a group brings its header into view too.
    for (int j = m_selectedIndex - 1; j >= 0; --j)
    {
        const Entry &entry = *m_entries[j];
        if (entry.isHeader)
            top = m_top[j];
        if (entry.isHeader || IsRow(j))
            break;
    }
    constexpr float kMargin = 3.0f;
    if (top - kMargin < m_scrollTarget)
        m_scrollTarget = top - kMargin;
    if (bottom + kMargin > m_scrollTarget + m_height)
        m_scrollTarget = bottom + kMargin - m_height;
    m_scrollTarget = std::clamp(m_scrollTarget, 0.0f, std::max(0.0f, m_contentHeight - m_height));
    if (instant)
        m_scroll = m_scrollTarget;
}

void MenuList::Update(uint32_t *buttonState, uint32_t *lastButtonState, float deltaSeconds)
{
    MenuItem::Update(buttonState, lastButtonState, deltaSeconds);
    const float t = std::min(1.0f, deltaSeconds * 14.0f);
    m_scroll += (m_scrollTarget - m_scroll) * t;
    if (std::abs(m_scrollTarget - m_scroll) < 0.05f)
        m_scroll = m_scrollTarget;
}

void MenuList::ResetSelection()
{
    m_selectedIndex = 0;
    m_activeColumn = 0;
    while (m_selectedIndex < (int)m_entries.size() - 1 && !IsRow(m_selectedIndex))
        ++m_selectedIndex;
    m_scroll = m_scrollTarget = 0;
}

void MenuList::SelectIndex(int index)
{
    if (index < 0 || index >= (int)m_entries.size() || !IsRow(index))
        return;
    m_selectedIndex = index;
    ScrollToSelection(true);
}

int MenuList::PressedUp()
{
    if (m_entries.empty())
        return 0;
    const int start = m_selectedIndex;
    do
    {
        if (m_selectedIndex > 0)
            --m_selectedIndex;
        else
            m_selectedIndex = (int)m_entries.size() - 1; // wrap to bottom
    } while (!IsRow(m_selectedIndex) && m_selectedIndex != start);
    ScrollToSelection(false);
    return 1;
}

int MenuList::PressedDown()
{
    if (m_entries.empty())
        return 0;
    const int start = m_selectedIndex;
    do
    {
        if (m_selectedIndex < (int)m_entries.size() - 1)
            ++m_selectedIndex;
        else
            m_selectedIndex = 0; // wrap to top
    } while (!IsRow(m_selectedIndex) && m_selectedIndex != start);
    ScrollToSelection(false);
    return 1;
}

int MenuList::PressedLeft()
{
    if (m_entries.empty())
        return 0;
    const Entry &entry = *m_entries[m_selectedIndex];
    if (entry.twoColumn)
    {
        if (m_activeColumn == 0)
            return 0; // (on to the sidebar)
        m_activeColumn = 0;
        return 1;
    }
    if (entry.leftFunction)
    {
        entry.leftFunction(this);
        return 1;
    }
    return 0;
}

int MenuList::PressedRight()
{
    if (m_entries.empty())
        return 0;
    const Entry &entry = *m_entries[m_selectedIndex];
    if (entry.twoColumn)
    {
        m_activeColumn = 1; // move highlight to the second binding column
        return 1;
    }
    if (entry.rightFunction)
    {
        entry.rightFunction(this);
        return 1;
    }
    if (entry.opensPage && entry.pressFunction)
    {
        entry.pressFunction(this);
        return 1;
    }
    return 0;
}

int MenuList::PressedEnter()
{
    if (m_entries.empty())
        return 0;
    const Entry &entry = *m_entries[m_selectedIndex];
    if (entry.pressFunction)
    {
        entry.pressFunction(this);
        return 1;
    }
    return 0;
}

void MenuList::Draw(UiRenderer &ui, float offsetX, float offsetY, float alpha)
{
    if (!Visible || m_entries.empty())
        return;
    Layout();

    const bool scrollbar = m_contentHeight > m_height + 0.5f;
    const float x = m_posX + offsetX;
    const float w = m_width - (scrollbar ? 4.0f : 0.0f);
    const float top = m_posY + offsetY - m_scroll;
    const bool focused = Selected;
    auto rowY = [&](int i) { return top + m_top[i]; };
    auto shown = [&](float y, float h) { return y + h > m_posY + offsetY - 4.0f && y < m_posY + offsetY + m_height + 4.0f; };

    ui.SetClipRect(m_posX + offsetX - 4.0f, m_posY + offsetY - 3.0f, m_width + 8.0f, m_height + 6.0f);

    // Cards: one behind each run of rows, a hairline between rows.
    for (size_t i = 0; i < m_entries.size();)
    {
        if (!IsRow(static_cast<int>(i)))
        {
            ++i;
            continue;
        }
        size_t last = i;
        for (size_t j = i + 1; j < m_entries.size(); ++j)
        {
            if (m_entries[j]->isHeader || m_entries[j]->isSpacer)
                break;
            if (IsRow(static_cast<int>(j)))
                last = j;
        }
        const float cardTop = rowY(static_cast<int>(i));
        const float cardBottom = rowY(static_cast<int>(last)) + m_itemHeight;
        const bool action = m_entries[i]->centered && i == last;
        if (shown(cardTop, cardBottom - cardTop))
        {
            ui.DrawQuadRounded(x, cardTop, w, cardBottom - cardTop,
                               WithAlpha(action ? XrColor4f{0.16f, 0.17f, 0.23f, 1.0f} : kMenuCardColor, alpha), kCardRadius);
            for (size_t j = i + 1; j <= last; ++j)
            {
                if (!IsRow(static_cast<int>(j)))
                    continue;
                const bool nextToSelection = focused && (static_cast<int>(j) == m_selectedIndex ||
                                                         static_cast<int>(j) - 1 == m_selectedIndex);
                if (!nextToSelection)
                    ui.DrawQuad(x + kRowPad, rowY(static_cast<int>(j)) - 0.3f, w - kRowPad * 2, 0.6f, WithAlpha(kMenuLineColor, alpha));
            }
        }
        i = last + 1;
    }

    for (int i = 0; i < (int)m_entries.size(); ++i)
    {
        const Entry &entry = *m_entries[i];
        const float y = rowY(i);
        if (entry.isHeader)
        {
            if (shown(y, kGroupHeaderHeight))
                ui.DrawText(m_headerFont, entry.text, x + 2.0f, TextY(ui, m_headerFont, y + kGroupHeaderHeight / 2.0f - 0.5f),
                            1.0f, WithAlpha(kMenuDimTextColor, alpha));
            continue;
        }
        if (!IsRow(i) || !shown(y, m_itemHeight))
            continue;

        const float h = m_itemHeight;
        const bool sel = focused && i == m_selectedIndex;
        if (sel)
            DrawSelectionFrame(ui, x, y, w, h, 5.0f, alpha, kMenuCardColor);

        const UiFontHandle labelFont = sel ? m_boldFont : m_font;
        const XrColor4f textColor = WithAlpha(sel ? SelectionColor : Color, alpha);
        const XrColor4f dimColor = WithAlpha(sel ? XrColor4f{1.0f, 0.79f, 0.34f, 0.7f} : kMenuDimTextColor, alpha);
        const float centerY = y + h / 2.0f;
        const bool hasIcon = m_icons && entry.icon != UiIconId::None;
        const bool hasIconSpace = entry.reserveIconSpace || hasIcon;
        const XrColor4f iconTint = (sel && (TintIconOnSelect || entry.tintIconOnSelect)) ? SelectionColor : Color;
        const std::string &label = entry.twoColumn ? entry.caption : entry.text;

        if (entry.centered)
        {
            const float iconWidth = hasIcon ? kIconSize + kIconTextGap : 0.0f;
            const float groupX = x + (w - iconWidth - ui.GetTextWidth(labelFont, label)) / 2.0f;
            if (hasIcon)
                m_icons->Draw(ui, entry.icon, groupX, centerY - kIconSize / 2.0f, kIconSize, alpha, iconTint);
            ui.DrawText(labelFont, label, groupX + iconWidth, TextY(ui, labelFont, centerY), 1.0f, textColor);
            continue;
        }

        const float iconX = x + kRowPad;
        if (hasIcon)
            m_icons->Draw(ui, entry.icon, iconX, centerY - kIconSize / 2.0f, kIconSize, alpha, iconTint);
        const float labelX = iconX + (hasIconSpace ? kIconSize + kIconTextGap : 0.0f);
        ui.DrawText(labelFont, label, labelX, TextY(ui, labelFont, centerY), 1.0f, textColor);

        const float right = x + w - kRowPad;
        if (entry.twoColumn)
        {
            // Two chips on the right: the primary and secondary binding.
            const float chipW = std::min(80.0f, (w - 96.0f) / 2.0f);
            const float chipH = h - 5.0f;
            for (int column = 0; column < 2; ++column)
            {
                const float chipX = right - chipW - (1 - column) * (chipW + 5.0f);
                const float chipY = y + 2.5f;
                const bool active = sel && m_activeColumn == column;
                const std::string &txt = column == 0 ? entry.text : entry.textSecondary;
                if (active)
                    DrawSelectionFrame(ui, chipX, chipY, chipW, chipH, 3.5f, alpha, kMenuCardColor);
                else
                    ui.DrawQuadRounded(chipX, chipY, chipW, chipH, WithAlpha({0.2f, 0.215f, 0.28f, 0.6f}, alpha), 3.5f);
                const UiFontHandle chipFont = active ? m_boldFont : m_font;
                const float tw = ui.GetTextWidth(chipFont, txt);
                ui.DrawText(chipFont, txt, chipX + (chipW - tw) / 2.0f, TextY(ui, chipFont, centerY), 1.0f,
                            active ? textColor : WithAlpha(txt == "-" ? kMenuDimTextColor : Color, alpha));
            }
            continue;
        }

        if (entry.toggle)
        {
            const bool on = entry.toggle();
            constexpr float kTrackW = 18.0f, kTrackH = 9.0f, kKnob = 6.5f;
            const float trackX = right - kTrackW, trackY = centerY - kTrackH / 2.0f;
            ui.DrawQuadRounded(trackX, trackY, kTrackW, kTrackH,
                               WithAlpha(on ? kMenuSelectionColor : kMenuLineColor, alpha), kTrackH / 2.0f);
            const float knobX = on ? trackX + kTrackW - kKnob - 1.25f : trackX + 1.25f;
            ui.DrawQuadRounded(knobX, centerY - kKnob / 2.0f, kKnob, kKnob,
                               WithAlpha(on ? XrColor4f{1.0f, 1.0f, 1.0f, 1.0f} : kMenuDimTextColor, alpha), kKnob / 2.0f);
            continue;
        }

        const bool adjustable = entry.leftFunction || entry.rightFunction;
        if (adjustable || entry.opensPage)
        {
            const float chevronX = right - kChevronWidth / 2.0f - ui.GetTextWidth(m_font, "\xE2\x80\xBA") / 2.0f;
            ui.DrawText(m_font, "\xE2\x80\xBA", chevronX, TextY(ui, m_font, centerY), 1.0f, dimColor);
        }
        const float valueRight = (adjustable || entry.opensPage) ? right - kChevronWidth : right;
        if (!entry.value.empty())
        {
            const float vw = ui.GetTextWidth(m_font, entry.value);
            ui.DrawText(m_font, entry.value, valueRight - vw, TextY(ui, m_font, centerY), 1.0f, textColor);
            if (sel && adjustable)
            {
                const float lw = ui.GetTextWidth(m_font, "\xE2\x80\xB9");
                ui.DrawText(m_font, "\xE2\x80\xB9", valueRight - vw - kChevronWidth / 2.0f - lw / 2.0f - 1.0f,
                            TextY(ui, m_font, centerY), 1.0f, dimColor);
            }
        }
        if (entry.accessoryDraw)
            entry.accessoryDraw(ui, x, y, w, h, alpha);
    }

    ui.ResetClipRect();

    if (scrollbar)
    {
        const float trackX = m_posX + offsetX + m_width - kScrollbarWidth;
        const float trackY = m_posY + offsetY;
        ui.DrawQuadRounded(trackX, trackY, kScrollbarWidth, m_height, WithAlpha(kMenuLineColor, alpha), kScrollbarWidth / 2.0f);
        const float thumbH = std::max(8.0f, m_height * m_height / m_contentHeight);
        const float thumbY = trackY + (m_height - thumbH) * (m_scroll / std::max(1.0f, m_contentHeight - m_height));
        ui.DrawQuadRounded(trackX, thumbY, kScrollbarWidth, thumbH, WithAlpha(kMenuDimTextColor, alpha), kScrollbarWidth / 2.0f);
    }
}

bool MenuList::HandlePointer(const MenuPointer &pointer)
{
    const float tx = pointer.TargetX(), ty = pointer.TargetY();
    if (!Visible || m_entries.empty() || tx < m_posX - 2.0f || tx > m_posX + m_width + 4.0f || ty < m_posY ||
        ty > m_posY + m_height)
        return false;
    Layout();
    const float maxScroll = std::max(0.0f, m_contentHeight - m_height);
    if (pointer.scroll != 0.0f)
        m_scrollTarget = std::clamp(m_scrollTarget + pointer.scroll * m_itemHeight, 0.0f, maxScroll);
    if (pointer.dragging)
    {
        // The rows follow the laser (no easing while it's held).
        m_scrollTarget = m_scroll = std::clamp(m_scroll - pointer.dragY, 0.0f, maxScroll);
        return true;
    }
    if (pointer.flingY != 0.0f)
        m_scrollTarget = std::clamp(m_scrollTarget - pointer.flingY, 0.0f, maxScroll);
    int hit = -1;
    for (int i = 0; i < (int)m_entries.size(); ++i)
    {
        const float y = m_posY - m_scroll + m_top[i];
        if (IsRow(i) && pointer.y >= y && pointer.y < y + m_itemHeight)
            hit = i;
    }
    if (hit < 0 || (!pointer.moved && !pointer.clicked))
        return true;

    const Entry &entry = *m_entries[hit];
    m_selectedIndex = hit;
    const bool scrollbar = m_contentHeight > m_height + 0.5f;
    const float right = m_posX + m_width - (scrollbar ? 4.0f : 0.0f) - kRowPad;
    if (entry.twoColumn)
    {
        const float chipW = std::min(80.0f, (m_width - (scrollbar ? 4.0f : 0.0f) - 96.0f) / 2.0f);
        m_activeColumn = pointer.x >= right - chipW - 2.5f ? 1 : 0;
    }
    if (!pointer.clicked)
        return true;

    const bool adjustable = entry.leftFunction || entry.rightFunction;
    if (adjustable && !entry.twoColumn)
    {
        // The value's left half (and its "<") steps down, the rest up.
        const float valueRight = right - kChevronWidth;
        const float vw = entry.value.empty() ? 30.0f : m_ui->GetTextWidth(m_font, entry.value);
        const float mid = valueRight - vw / 2.0f;
        const float leftEdge = valueRight - vw - kChevronWidth - 4.0f;
        if (pointer.x < mid && pointer.x >= leftEdge && entry.leftFunction)
            entry.leftFunction(this);
        else if (entry.rightFunction)
            entry.rightFunction(this);
        else if (entry.pressFunction)
            entry.pressFunction(this);
        return true;
    }
    if (entry.pressFunction)
        entry.pressFunction(this);
    return true;
}
