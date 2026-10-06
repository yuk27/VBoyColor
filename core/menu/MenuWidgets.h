#pragma once

#include "input/ButtonMapping.h"
#include "gfx/UiIconSet.h"
#include "gfx/UiRenderer.h"

#include <openxr/openxr.h> // for XrColor4f

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// A mouse (desktop) or controller laser (Quest) over the menu, in its
// logical units - see AppMenu::SetPointer.
struct MenuPointer
{
    float x = 0, y = 0;
    bool moved = false;   // it moved since last frame: hovering selects
    bool clicked = false; // pressed this frame
    float scroll = 0;     // wheel/stick scroll this frame, in rows (+ = down)
};

// Ported from FrontendGo's MenuHelper.h/.cpp - the navigation/selection
// state machine (Menu::Update/MoveSelection/ButtonPressed) is carried over
// near-verbatim (it only ever touched the abstract uint[3] button-bitmask
// model, never VrApi/GL directly). The draw methods are rewritten against
// UiRenderer instead of DrawHelper/FontManager. The original's standalone
// MenuImage widget and its list widget (MenuList<T>, dead/unfinished code -
// its draw methods were declared but never defined) are dropped; MenuList
// below is a new fixed-rect scrolling list, not a port. Its rows can carry
// an optional UiIconId (see AddEntry) - the closest equivalent of the
// original's per-MenuButton IconId.
class MenuItem
{
public:
    bool Selectable = false;
    bool Selected = false;
    bool Visible = true;
    float PosX = 100, PosY = 100;

    float ScrollDelay = 0.3f;
    float ScrollTimeV = 0.075f;
    float ScrollTimeH = 0.075f;

    int Tag = 0;
    int Tag2 = 0;

    XrColor4f Color{1, 1, 1, 1};
    XrColor4f SelectionColor{1, 0.85f, 0.1f, 1};

    std::function<void(MenuItem *item, int direction)> OnSelectFunction;
    std::function<void(MenuItem *item, uint32_t *buttonState, uint32_t *lastButtonState)> UpdateFunction;

    MenuItem() = default;
    virtual ~MenuItem() = default;

    virtual void Update(uint32_t *buttonState, uint32_t *lastButtonState, float deltaSeconds);

    virtual int PressedUp();
    virtual int PressedDown();
    virtual int PressedLeft();
    virtual int PressedRight();
    virtual int PressedEnter();

    virtual void OnSelect(int direction);
    virtual void Select();
    virtual void Unselect();

    // Restores the item's internal cursor to its start - e.g. MenuList's
    // scroll position - so re-entering a page doesn't leave it wherever the
    // user last left it. No-op for items without one of their own.
    virtual void ResetSelection() {}

    virtual void Draw(UiRenderer &ui, float offsetX, float offsetY, float alpha);

    // The pointer over this item: hovering moves the item's own selection,
    // a click acts on what it's over. True if it was over this item (the
    // Menu then makes it the current item).
    virtual bool HandlePointer(const MenuPointer & /*pointer*/) { return false; }
};

class Menu
{
public:
    std::vector<std::shared_ptr<MenuItem>> MenuItems;

    int CurrentSelection = 0;
    float buttonDownCount = 0;

    std::function<void()> BackPress;
    // Left pressed (freshly - not held) where the current item has nothing
    // further left: AppMenu moves the focus to its sidebar.
    std::function<void()> LeftEdge;
    // The Y / X face buttons (a gamepad's, or the left Quest controller's).
    std::function<void()> YPress;
    std::function<void()> XPress;

    // If set, called each Update() instead of normal navigation. The entire
    // capture frame is consumed even when the hook completes, so a captured
    // direction cannot also move the menu selection. Used by remap pages.
    std::function<bool(uint32_t *buttonState, uint32_t *lastButtonState)> CaptureHook;
    std::function<void(const ButtonMapper::MappedButton &)> RawCaptureHook;

    void SubmitRawCaptureInput(const ButtonMapper::MappedButton &button)
    {
        if (RawCaptureHook)
            RawCaptureHook(button);
    }

    void Init();

    bool ButtonPressed(uint32_t *buttonState, uint32_t *lastButtonState, uint32_t device, uint32_t button);

    void MoveSelection(int dir, bool onSelect);

    void Update(uint32_t *buttonState, uint32_t *lastButtonState, float deltaSeconds);

    void Draw(UiRenderer &ui, int transitionDirX, int transitionDirY, float moveProgress, float moveDist, float fadeProgress);

    // Hands the pointer to the items (see MenuItem::HandlePointer).
    void HandlePointer(const MenuPointer &pointer);
    bool HasSelectable() const;

    // Resets the top-level cursor to the first selectable item and resets
    // every item's own internal selection (e.g. MenuList's scroll cursor).
    void ResetSelection();
};

class MenuLabel : public MenuItem
{
public:
    MenuLabel(UiRenderer &ui, UiFontHandle font, const std::string &text, float posX, float posY, float width, float height,
              XrColor4f color);

    void SetText(const std::string &newText);

    void Draw(UiRenderer &ui, float offsetX, float offsetY, float alpha) override;

private:
    UiRenderer *m_ui;
    UiFontHandle m_font;
    float m_containerX, m_containerY, m_containerWidth, m_containerHeight;
    std::string m_text;
};

class MenuButton : public MenuItem
{
public:
    std::string Text;

    MenuButton(UiRenderer &ui, UiFontHandle font, const std::string &text, float posX, float posY, float width, float height,
               std::function<void(MenuItem *item)> pressFunction, std::function<void(MenuItem *item)> leftFunction = nullptr,
               std::function<void(MenuItem *item)> rightFunction = nullptr);

    // Left-aligned, no vertical/horizontal centering - matches the
    // original's main-menu-page constructor (MenuHelper.cpp's 5-posarg
    // MenuButton ctor), used for plain top-to-bottom stacked menu lists.
    MenuButton(UiRenderer &ui, UiFontHandle font, const std::string &text, float posX, float posY,
               std::function<void(MenuItem *item)> pressFunction, std::function<void(MenuItem *item)> leftFunction = nullptr,
               std::function<void(MenuItem *item)> rightFunction = nullptr);

    void SetText(const std::string &newText);

    int PressedLeft() override;
    int PressedRight() override;
    int PressedEnter() override;

    void Draw(UiRenderer &ui, float offsetX, float offsetY, float alpha) override;

private:
    UiRenderer *m_ui;
    UiFontHandle m_font;
    std::function<void(MenuItem *item)> m_pressFunction;
    std::function<void(MenuItem *item)> m_leftFunction;
    std::function<void(MenuItem *item)> m_rightFunction;
    float m_containerWidth = 0;
    float m_offsetX = 0;
};

// A fixed-rect image backed by its own streaming texture, not selectable.
// Used for the save-slot preview: Draw shows the last SetImage'd image, or
// an "Empty Slot" placeholder after Clear().
class MenuImage : public MenuItem
{
public:
    // textureWidth/textureHeight size the backing streaming texture; width/
    // height are the on-screen draw size (stretched to fit). tintProvider,
    // if given, is called fresh every draw to tint the image live (e.g. the
    // VB color palette). patternIndexProvider, if given and returning 0-5,
    // draws through screen_pattern.frag's multi-hue gradient instead (see
    // UiRenderer::DrawImageRegionPattern) and tintProvider is ignored for
    // that frame - same tint-vs-pattern split as Emulator::DrawScreen, kept
    // in sync here so the save-slot preview matches AppSettings::selectedPattern
    // instead of always falling back to the (possibly stale) flat tint.
    MenuImage(UiRenderer &ui, UiFontHandle font, uint32_t textureWidth, uint32_t textureHeight, float posX, float posY,
              float width, float height, std::function<XrColor4f()> tintProvider = nullptr,
              std::function<int()> patternIndexProvider = nullptr);

    // rgba must be exactly textureWidth*textureHeight*4 bytes.
    void SetImage(const std::vector<uint8_t> &rgba);
    void Clear();

    void Draw(UiRenderer &ui, float offsetX, float offsetY, float alpha) override;

private:
    UiRenderer *m_ui;
    UiFontHandle m_font;
    UiImageHandle m_texture;
    float m_width, m_height;
    bool m_hasImage = false;
    std::function<XrColor4f()> m_tintProvider;
    std::function<int()> m_patternIndexProvider;
};

// A vertically scrollable list that fills a fixed content rect: rows on
// rounded cards, grouped under small headers (AddHeader) or split by
// spacers (AddSpacer), each row a label on the left and - optionally - a
// value, a toggle switch or an accessory (e.g. color swatches) on the right.
// Handles its own up/down selection, scrolls smoothly to keep it in view and
// draws a scrollbar when entries don't fit. Pages add entries via AddEntry()
// without managing Y positions at all.
class MenuList : public MenuItem
{
public:
    // posX/posY/width/height define the bounding rect the list fills.
    // icons may be null for pages that don't pass any entries an icon.
    // itemHeight: each row's height. font: the rows' labels and values.
    MenuList(UiRenderer &ui, UiFontHandle font, float posX, float posY, float width, float height, float itemHeight,
             const UiIconSet *icons = nullptr);

    // The selected row's label (default: font) and the group headers'
    // (default: font).
    void SetFonts(UiFontHandle boldFont, UiFontHandle headerFont);

    // Whether a row's icon also tints to SelectionColor while selected (like
    // its text already does). On by default.
    bool TintIconOnSelect = true;
    // Unused (kept for pages that still set it): the selected row is drawn
    // amber-outlined instead.
    XrColor4f HighlightColor{0.0f, 0.0f, 0.0f, 0.0f};

    // Draws an arbitrary accessory (e.g. color swatches) into a row's
    // value area - rowX/rowY/rowW/rowH are the row's full content-space
    // bounds; accessories right-align against rowX + rowW - kValueRightPad.
    using AccessoryDrawFn = std::function<void(UiRenderer &ui, float rowX, float rowY, float rowW, float rowH, float alpha)>;

    // Icon size/gap every row uses ahead of its label, and the right
    // margin of the value area.
    static constexpr float kIconSize = 10.0f;
    static constexpr float kIconTextGap = 5.0f;
    static constexpr float kRowPad = 8.0f;
    static constexpr float kChevronWidth = 9.0f;
    static constexpr float kValueRightPad = kRowPad + kChevronWidth;

    // One row of the list. AddEntry/AddSpacer/AddHeader hand it back as a
    // shared_ptr so a page can keep it and change any property later: text
    // via SetText/SetValue (re-bakes glyphs, so a raw text = ... would miss
    // new ones), everything else (icon, callbacks, ...) by direct
    // assignment. Read live every frame by Draw - no separate "apply" step.
    class Entry
    {
    public:
        std::string text;
        std::function<void(MenuItem *)> pressFunction;
        std::function<void(MenuItem *)> leftFunction;
        std::function<void(MenuItem *)> rightFunction;
        UiIconId icon = UiIconId::None;
        AccessoryDrawFn accessoryDraw;
        bool isSpacer = false;
        bool isHeader = false;
        float height = 0; // only used when isSpacer
        // Set false to collapse this row to zero height and skip it during
        // Up/Down navigation - toggleable at runtime. Used by SettingsPage
        // to hide the R/G/B rows while a screen pattern (as opposed to a
        // flat tint) is selected.
        bool Visible = true;

        // Right-aligned value (e.g. "Auto"). With left/rightFunction the
        // row shows it between chevrons (adjust with Left/Right).
        std::string value;
        // A toggle switch on the right instead of a value: its state.
        std::function<bool()> toggle;
        // A "›" on the right: pressing opens another page.
        bool opensPage = false;

        // Two-column rows (twoColumn == true) draw `caption` as the label and
        // `text` / `textSecondary` as two side-by-side chips on the right -
        // Left/Right move the highlight between the two (see
        // MenuList::GetActiveColumn) instead of calling left/rightFunction,
        // and pressFunction fires for whichever column is active. Used by
        // the button-mapping page's per-button primary + secondary bindings.
        bool twoColumn = false;
        std::string caption;
        std::string textSecondary;
        bool centered = false;         // an action row: its label centered (e.g. "Reset mapping")
        bool tintIconOnSelect = false; // per-entry override when the list disables tint
        bool reserveIconSpace = false; // align text with icon rows without drawing an icon

        // Sets the (primary) label and bakes any glyphs it needs. Prefer this
        // over assigning text directly for anything but pure ASCII (always
        // pre-baked) - see UiFontManager::EnsureGlyphsForText. Call outside a
        // frame only (it may re-upload the font atlas).
        void SetText(const std::string &newText);
        void SetValue(const std::string &newValue);
        void SetCaption(const std::string &newCaption);
        // Second-column label for a twoColumn row (same baking rules).
        void SetSecondaryText(const std::string &newText);

        // Moves the list's highlight to this row, scrolling it into view.
        void Select();

    private:
        friend class MenuList;
        MenuList *m_owner = nullptr;
    };

    // Adds a row and returns it. Optional press/left/right callbacks, an
    // icon, and an accessoryDraw (see above) - all also settable later on
    // the returned Entry.
    std::shared_ptr<Entry> AddEntry(const std::string &text,
                                    std::function<void(MenuItem *)> press = nullptr,
                                    std::function<void(MenuItem *)> left = nullptr,
                                    std::function<void(MenuItem *)> right = nullptr,
                                    UiIconId icon = UiIconId::None,
                                    AccessoryDrawFn accessoryDraw = nullptr);

    // Adds (and returns) a gap between two cards of rows (height: kept for
    // callers, the gap is always the same).
    std::shared_ptr<Entry> AddSpacer(float height);
    // Adds a small header above the next card of rows.
    std::shared_ptr<Entry> AddHeader(const std::string &text);

    // Which column (0 or 1) is highlighted on the current two-column row -
    // read by a twoColumn entry's pressFunction to know which slot to act on.
    int GetActiveColumn() const { return m_activeColumn; }

    int PressedUp() override;
    int PressedDown() override;
    int PressedLeft() override;
    int PressedRight() override;
    int PressedEnter() override;

    void ResetSelection() override;
    void Update(uint32_t *buttonState, uint32_t *lastButtonState, float deltaSeconds) override;
    void Draw(UiRenderer &ui, float offsetX, float offsetY, float alpha) override;
    bool HandlePointer(const MenuPointer &pointer) override;

private:
    // Moves the highlight straight to index, scrolling it into view.
    void SelectIndex(int index);
    bool IsRow(int index) const;
    // Lays the entries out (top of each, from the list's top) - cheap,
    // done whenever needed.
    void Layout() const;
    void ScrollToSelection(bool instant);
    float ContentHeight() const;

    UiRenderer *m_ui;
    UiFontHandle m_font, m_boldFont, m_headerFont;
    const UiIconSet *m_icons;
    float m_posX, m_posY, m_width, m_height;
    float m_itemHeight;
    int m_selectedIndex = 0;
    int m_activeColumn = 0; // 0/1 highlight within a two-column row (see GetActiveColumn)
    float m_scroll = 0, m_scrollTarget = 0;
    std::vector<std::shared_ptr<Entry>> m_entries;
    mutable std::vector<float> m_top; // see Layout
    mutable float m_contentHeight = 0;

    static constexpr float kScrollbarWidth = 1.5f;
};
