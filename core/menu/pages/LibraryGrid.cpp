#include "menu/pages/LibraryGrid.h"

#include "menu/ThumbnailLibrary.h"
#include "menu/pages/AppMenuLayout.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr float kGap = 7.0f;            // between cards
    constexpr float kThumbRadius = 5.0f;    // a thumbnail's corners
    constexpr float kLabelHeight = 25.0f;   // name + region under a card
    constexpr float kListRowHeight = 30.0f; // list view
    constexpr float kListThumbWidth = 46.0f;
    constexpr const char *kEllipsis = "\xE2\x80\xA6";

    XrColor4f WithAlpha(XrColor4f c, float alpha)
    {
        c.a *= alpha;
        return c;
    }

    float TextY(UiRenderer &ui, UiFontHandle font, float centerY)
    {
        return centerY - ui.GetFontPHeight(font) / 2.0f - ui.GetFontPStart(font);
    }

    // The "has a color pack" badge: three dots on a dark pill.
    void DrawPackBadge(UiRenderer &ui, float right, float top, float alpha)
    {
        constexpr float kW = 18.5f, kH = 8.0f, kDot = 3.8f;
        ui.DrawQuadRounded(right - kW, top, kW, kH, WithAlpha({0.07f, 0.075f, 0.105f, 0.85f}, alpha), kH / 2.0f);
        const XrColor4f dots[3] = {{0.95f, 0.3f, 0.3f, 1.0f}, {0.35f, 0.8f, 0.45f, 1.0f}, {0.35f, 0.55f, 1.0f, 1.0f}};
        for (int i = 0; i < 3; ++i)
            ui.DrawQuadRounded(right - kW + 3.6f + i * 5.2f - 0.25f, top + (kH - kDot) / 2.0f, kDot, kDot,
                               WithAlpha(dots[i], alpha), kDot / 2.0f);
    }

    // A soft amber glow and outline around a card (the selected one).
    void DrawSelectedOutline(UiRenderer &ui, float x, float y, float w, float h, float radius, float alpha)
    {
        for (int i = 3; i >= 1; --i)
        {
            const float s = 1.2f + i * 1.1f;
            ui.DrawQuadRounded(x - s, y - s, w + s * 2, h + s * 2, WithAlpha({1.0f, 0.79f, 0.34f, 0.07f}, alpha), radius + s);
        }
        ui.DrawQuadRounded(x - 1.4f, y - 1.4f, w + 2.8f, h + 2.8f, WithAlpha(kMenuSelectionColor, alpha), radius + 1.4f);
    }
} // namespace

LibraryGrid::LibraryGrid(UiRenderer &ui, const UiMenuResources &resources, ThumbnailLibrary &library, float x, float y,
                         float width, float height)
    : m_ui(&ui), m_resources(&resources), m_library(&library), m_x(x), m_y(y), m_width(width), m_height(height)
{
    Selectable = true;
    // Rows step a little slower than list rows - a row of cards is a big jump.
    ScrollTimeV = 0.12f;
    ScrollTimeH = 0.09f;
    RefreshLabels();
}

int LibraryGrid::Count() const { return static_cast<int>(m_library->Games().size()); }

float LibraryGrid::CardWidth() const
{
    return m_listView ? m_width - 4.0f : (m_width - 4.0f - kGap * (Columns() - 1)) / Columns();
}

float LibraryGrid::ThumbHeight() const
{
    return m_listView ? kListThumbWidth * ThumbnailLibrary::kHeight / ThumbnailLibrary::kWidth
                      : CardWidth() * ThumbnailLibrary::kHeight / ThumbnailLibrary::kWidth;
}

float LibraryGrid::RowHeight() const { return m_listView ? kListRowHeight + 3.0f : ThumbHeight() + kLabelHeight; }

float LibraryGrid::ContentHeight() const
{
    const int rows = (Count() + Columns() - 1) / Columns();
    return rows * RowHeight();
}

std::string LibraryGrid::Fit(UiFontHandle font, const std::string &text, float width) const
{
    if (m_ui->GetTextWidth(font, text) <= width)
        return text;
    // Cut on UTF-8 character boundaries until it fits with the ellipsis.
    std::string cut = text;
    while (!cut.empty())
    {
        do
            cut.pop_back();
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80);
        if (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0x80))
            cut.pop_back();
        while (!cut.empty() && cut.back() == ' ')
            cut.pop_back();
        if (m_ui->GetTextWidth(font, cut + kEllipsis) <= width)
            return cut + kEllipsis;
    }
    return kEllipsis;
}

void LibraryGrid::RefreshLabels()
{
    const auto &games = m_library->Games();
    if (m_labelsFor == m_library->Version() && m_labelsList == m_listView)
        return;
    m_labelsFor = m_library->Version();
    m_labelsList = m_listView;
    const UiMenuResources &r = *m_resources;
    const UiFontHandle titleFont = m_listView ? r.bodyFont : r.cardFont;
    const UiFontHandle titleBold = m_listView ? r.bodyBoldFont : r.cardBoldFont;
    const float width = m_listView ? CardWidth() - kListThumbWidth - 40.0f : CardWidth() - 2.0f;
    m_titles.clear();
    m_titlesBold.clear();
    m_details.clear();
    for (const auto &game : games)
    {
        for (UiFontHandle font : {titleFont, titleBold, r.captionFont})
            m_ui->EnsureGlyphsForText(font, game.rom.name + kEllipsis);
        m_titles.push_back(Fit(titleFont, game.title, width));
        m_titlesBold.push_back(Fit(titleBold, game.title, width));
        m_details.push_back(Fit(r.captionFont, game.details, width));
    }
}

void LibraryGrid::SetListView(bool listView)
{
    if (m_listView == listView)
        return;
    m_listView = listView;
    RefreshLabels();
    ScrollToSelected(true);
}

void LibraryGrid::SetSelectedGame(int index, bool instant)
{
    if (Count() == 0)
        return;
    m_selected = std::clamp(index, 0, Count() - 1);
    ScrollToSelected(instant);
}

void LibraryGrid::ScrollToSelected(bool instant)
{
    const int row = m_selected / Columns();
    const float top = row * RowHeight();
    const float bottom = top + RowHeight() - (m_listView ? 3.0f : kGap);
    constexpr float kMargin = 4.0f;
    if (top - kMargin < m_scrollTarget)
        m_scrollTarget = top - kMargin;
    if (bottom + kMargin > m_scrollTarget + m_height)
        m_scrollTarget = bottom + kMargin - m_height;
    m_scrollTarget = std::clamp(m_scrollTarget, 0.0f, std::max(0.0f, ContentHeight() - m_height));
    if (instant)
        m_scroll = m_scrollTarget;
}

void LibraryGrid::Move(int delta)
{
    const int next = m_selected + delta;
    if (next < 0 || next >= Count())
        return;
    m_selected = next;
    ScrollToSelected(false);
}

int LibraryGrid::PressedUp()
{
    if (m_selected - Columns() >= 0)
        Move(-Columns());
    return 1;
}

int LibraryGrid::PressedDown()
{
    if (m_selected + Columns() < Count())
        Move(Columns());
    else if (m_selected / Columns() < (Count() - 1) / Columns())
        Move(Count() - 1 - m_selected); // (the last row isn't full: its last card)
    return 1;
}

int LibraryGrid::PressedLeft()
{
    if (m_listView || m_selected % Columns() == 0)
        return 0; // (on to the sidebar)
    Move(-1);
    return 1;
}

int LibraryGrid::PressedRight()
{
    if (!m_listView && m_selected % Columns() < Columns() - 1)
        Move(1);
    return 1;
}

int LibraryGrid::PressedEnter()
{
    if (onPlay && m_selected < Count())
        onPlay(m_selected);
    return 1;
}

void LibraryGrid::Update(uint32_t *buttonState, uint32_t *lastButtonState, float deltaSeconds)
{
    MenuItem::Update(buttonState, lastButtonState, deltaSeconds);
    RefreshLabels();
    const float t = std::min(1.0f, deltaSeconds * 14.0f);
    m_scroll += (m_scrollTarget - m_scroll) * t;
    if (std::abs(m_scrollTarget - m_scroll) < 0.05f)
        m_scroll = m_scrollTarget;
    m_pulse = std::fmod(m_pulse + deltaSeconds, 2.0f);
}

int LibraryGrid::HitTest(float x, float y) const
{
    if (x < m_x || x > m_x + m_width || y < m_y || y > m_y + m_height)
        return -1;
    const float cy = y - m_y + m_scroll;
    const int row = static_cast<int>(cy / RowHeight());
    int column = 0;
    if (!m_listView)
    {
        const float cx = x - m_x;
        column = static_cast<int>(cx / (CardWidth() + kGap));
        if (column >= Columns() || cx - column * (CardWidth() + kGap) > CardWidth())
            return -1; // (between cards)
    }
    if (cy - row * RowHeight() > RowHeight() - (m_listView ? 3.0f : kGap))
        return -1;
    const int index = row * Columns() + column;
    return index < Count() ? index : -1;
}

bool LibraryGrid::HandlePointer(const MenuPointer &pointer)
{
    if (pointer.x < m_x - 2.0f || pointer.x > m_x + m_width + 6.0f || pointer.y < m_y || pointer.y > m_y + m_height)
        return false;
    if (pointer.scroll != 0.0f)
        m_scrollTarget = std::clamp(m_scrollTarget + pointer.scroll * RowHeight() * 0.5f, 0.0f,
                                    std::max(0.0f, ContentHeight() - m_height));
    const int hit = HitTest(pointer.x, pointer.y);
    if (hit >= 0 && (pointer.moved || pointer.clicked))
        m_selected = hit;
    if (hit >= 0 && pointer.clicked && onPlay)
        onPlay(hit);
    return true;
}

void LibraryGrid::Draw(UiRenderer &ui, float offsetX, float offsetY, float alpha)
{
    if (!Visible)
        return;
    const auto &games = m_library->Games();
    const UiMenuResources &r = *m_resources;
    const bool focused = Selected;
    const float x0 = m_x + offsetX, y0 = m_y + offsetY - m_scroll;
    const float cardW = CardWidth(), thumbH = ThumbHeight(), rowH = RowHeight();
    const float pulse = 0.75f + 0.25f * std::cos(m_pulse * 3.14159265f);
    const bool labelsOk = m_labelsFor == m_library->Version() && m_titles.size() == games.size();

    ui.SetClipRect(m_x + offsetX - 6.0f, m_y + offsetY - 4.0f, m_width + 10.0f, m_height + 4.0f);
    for (int i = 0; i < static_cast<int>(games.size()); ++i)
    {
        const auto &game = games[i];
        const int row = i / Columns(), column = i % Columns();
        const float x = x0 + column * (cardW + kGap);
        const float y = y0 + row * rowH;
        if (y + rowH < m_y + offsetY - 4.0f || y > m_y + offsetY + m_height + 4.0f)
            continue;
        const bool sel = focused && i == m_selected;
        const std::string &title = !labelsOk ? game.title : sel ? m_titlesBold[i] : m_titles[i];
        const std::string &details = labelsOk ? m_details[i] : game.details;

        // The thumbnail (or, until it's made, a placeholder card).
        const float tx = m_listView ? x + 4.0f : x;
        const float ty = m_listView ? y + (kListRowHeight - thumbH) / 2.0f : y;
        const float tw = m_listView ? kListThumbWidth : cardW;
        const float radius = m_listView ? 3.0f : kThumbRadius;
        if (m_listView)
        {
            if (sel)
            {
                DrawSelectedOutline(ui, x, y, cardW, kListRowHeight, 6.0f, alpha);
                ui.DrawQuadRounded(x, y, cardW, kListRowHeight, WithAlpha(kMenuCardColor, alpha), 6.0f);
                ui.DrawQuadRounded(x, y, cardW, kListRowHeight, WithAlpha(kMenuSelectionFillColor, alpha), 6.0f);
            }
            else
                ui.DrawQuadRounded(x, y, cardW, kListRowHeight, WithAlpha(kMenuCardColor, alpha), 6.0f);
        }
        else if (sel)
            DrawSelectedOutline(ui, tx, ty, tw, thumbH, radius, alpha);
        else
            ui.DrawQuadRounded(tx - 0.8f, ty - 0.8f, tw + 1.6f, thumbH + 1.6f, WithAlpha(kMenuLineColor, alpha), radius + 0.8f);
        if (game.ready)
            ui.DrawImageRounded(game.texture, tx, ty, tw, thumbH, radius, alpha);
        else
        {
            ui.DrawQuadRounded(tx, ty, tw, thumbH, WithAlpha({0.1f, 0.107f, 0.145f, 1.0f}, alpha), radius);
            const float iconSize = m_listView ? 9.0f : 14.0f;
            if (r.icons)
                r.icons->Draw(ui, UiIconId::VbCartridge, tx + (tw - iconSize) / 2.0f, ty + (thumbH - iconSize) / 2.0f, iconSize,
                              alpha * (game.failed ? 0.3f : 0.6f * pulse));
        }
        if (game.hasPack && !m_listView)
            DrawPackBadge(ui, tx + tw - 3.5f, ty + 3.5f, alpha);

        // Name and region.
        const XrColor4f titleColor = WithAlpha(sel ? kMenuSelectionColor : kMenuTextColor, alpha);
        const XrColor4f dim = WithAlpha(kMenuDimTextColor, alpha);
        if (m_listView)
        {
            const UiFontHandle font = sel ? r.bodyBoldFont : r.bodyFont;
            const float lx = tx + tw + 8.0f;
            ui.DrawText(font, title, lx, TextY(ui, font, y + 11.0f), 1.0f, titleColor);
            ui.DrawText(r.captionFont, details, lx, TextY(ui, r.captionFont, y + 21.0f), 1.0f, dim);
            if (game.hasPack)
                DrawPackBadge(ui, x + cardW - 8.0f, y + (kListRowHeight - 8.0f) / 2.0f, alpha);
        }
        else
        {
            const UiFontHandle font = sel ? r.cardBoldFont : r.cardFont;
            ui.DrawText(font, title, x + 1.0f, TextY(ui, font, y + thumbH + 7.5f), 1.0f, titleColor);
            ui.DrawText(r.captionFont, details, x + 1.0f, TextY(ui, r.captionFont, y + thumbH + 15.5f), 1.0f, dim);
        }
    }

    // More below: the cut-off row fades out.
    const float maxScroll = std::max(0.0f, ContentHeight() - m_height);
    if (m_scroll < maxScroll - 1.0f)
    {
        constexpr int kSteps = 8;
        constexpr float kFade = 14.0f;
        for (int s = 0; s < kSteps; ++s)
        {
            XrColor4f c = kMenuBodyColor;
            c.a = alpha * (s + 1) / static_cast<float>(kSteps);
            ui.DrawQuad(m_x + offsetX - 6.0f, m_y + offsetY + m_height - kFade + s * kFade / kSteps, m_width + 12.0f,
                        kFade / kSteps + 0.2f, c);
        }
    }
    ui.ResetClipRect();

    if (maxScroll > 0.0f)
    {
        constexpr float kBarW = 1.5f;
        const float trackX = m_x + offsetX + m_width + 2.5f, trackY = m_y + offsetY;
        ui.DrawQuadRounded(trackX, trackY, kBarW, m_height, WithAlpha(kMenuLineColor, alpha), kBarW / 2.0f);
        const float thumb = std::max(10.0f, m_height * m_height / ContentHeight());
        ui.DrawQuadRounded(trackX, trackY + (m_height - thumb) * (m_scroll / maxScroll), kBarW, thumb,
                           WithAlpha(kMenuDimTextColor, alpha), kBarW / 2.0f);
    }
}
