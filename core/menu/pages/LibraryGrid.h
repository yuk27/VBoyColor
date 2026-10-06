#pragma once

#include "menu/MenuWidgets.h"
#include "menu/UiMenuResources.h"

#include <functional>
#include <string>
#include <vector>

class ThumbnailLibrary;

// The library's games as cards - each game's thumbnail (see
// ThumbnailLibrary) with its name and region under it, three to a row - or
// as a list (listView): a small thumbnail beside each name. Moves with the
// D-pad/sticks (Left at the first column: on to the sidebar), A plays, and
// the pointer (mouse / Quest laser) hovers, clicks and scrolls.
class LibraryGrid : public MenuItem
{
public:
    LibraryGrid(UiRenderer &ui, const UiMenuResources &resources, ThumbnailLibrary &library, float x, float y,
                float width, float height);

    std::function<void(int game)> onPlay;

    int SelectedGame() const { return m_selected; }
    void SetSelectedGame(int index, bool instant);
    bool IsListView() const { return m_listView; }
    void SetListView(bool listView);
    // Re-measures the names (call outside a frame - it may bake glyphs)
    // when the games changed.
    void RefreshLabels();

    int PressedUp() override;
    int PressedDown() override;
    int PressedLeft() override;
    int PressedRight() override;
    int PressedEnter() override;
    void ResetSelection() override {}
    void Update(uint32_t *buttonState, uint32_t *lastButtonState, float deltaSeconds) override;
    void Draw(UiRenderer &ui, float offsetX, float offsetY, float alpha) override;
    bool HandlePointer(const MenuPointer &pointer) override;

private:
    int Columns() const { return m_listView ? 1 : 3; }
    float RowHeight() const;
    float CardWidth() const;
    float ThumbHeight() const;
    float ContentHeight() const;
    int Count() const;
    void Move(int delta);
    void ScrollToSelected(bool instant);
    int HitTest(float x, float y) const;
    std::string Fit(UiFontHandle font, const std::string &text, float width) const;

    UiRenderer *m_ui;
    const UiMenuResources *m_resources;
    ThumbnailLibrary *m_library;
    float m_x, m_y, m_width, m_height;
    bool m_listView = false;
    int m_selected = 0;
    float m_scroll = 0, m_scrollTarget = 0;
    float m_pulse = 0; // (placeholder cards breathe while thumbnails are made)

    // Names fitted to the cards: regular, bold (selected), region.
    std::vector<std::string> m_titles, m_titlesBold, m_details;
    size_t m_labelsFor = static_cast<size_t>(-1);
    bool m_labelsList = false;
};
