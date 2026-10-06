#pragma once

#include "menu/MenuWidgets.h"
#include "menu/UiMenuResources.h"
#include "gfx/UiRenderer.h"

#include <functional>
#include <string>
#include <vector>

// One button hint along the menu's bottom ("(A) Play").
struct MenuHint
{
    UiIconId icon;
    std::string label;
};

// Base class for a single menu screen - the content right of AppMenu's
// sidebar: a title (and subtitle) on top and the page's own items below.
// Each subclass owns a Menu (the widget container / navigation state
// machine) and a Navigate callback that AppMenu wires up at init time so
// pages can push/pop transitions without knowing about each other.
//
// Usage:
//   page.Navigate = [&](MenuPage* target, int dir) { StartTransition(target, dir); };
//   page.Attach(resources);
//   page.Init(ui, resources);
//   page.Update(btn, lastBtn, dt);
//   page.Draw(ui, transitionDirX, transitionDirY, moveProgress, moveDist, fadeProgress);
class MenuPage
{
public:
    virtual ~MenuPage() = default;

    // Called by AppMenu before Init: the fonts the title is drawn with.
    void Attach(const UiMenuResources &resources) { m_resources = &resources; }
    // Called once after resources are loaded and Navigate is set.
    virtual void Init(UiRenderer &ui, const UiMenuResources &resources) = 0;

    // Navigate to another page. Set by AppMenu before Init().
    // dir: 1 = slide next page in from right, -1 = from left.
    std::function<void(MenuPage *target, int dir)> Navigate;

    // Which of the sidebar's entries this page is under (AppMenu's
    // SidebarItem) - set by AppMenu.
    int sidebarItem = 0;

    // The page's title and the smaller text beside it.
    virtual std::string Title() const { return ""; }
    virtual std::string Subtitle() const { return ""; }
    // The bottom row's hints while this page has the focus.
    virtual std::vector<MenuHint> Hints() const
    {
        return {{UiIconId::ButtonA, "Select"}, {UiIconId::ButtonB, "Back"}};
    }
    // Whether there's anything to move to on this page (About has none:
    // the focus stays in the sidebar).
    virtual bool HasFocusable() const { return m_menu.HasSelectable(); }

    virtual void Update(uint32_t *buttonState, uint32_t *lastButtonState, float deltaSeconds)
    {
        m_menu.Update(buttonState, lastButtonState, deltaSeconds);
    }

    // Draws the title, then the page's items - offset by the transition.
    virtual void Draw(UiRenderer &ui, int transitionDirX, int transitionDirY, float moveProgress, float moveDist,
                      float fadeProgress);

    virtual void HandlePointer(const MenuPointer &pointer) { m_menu.HandlePointer(pointer); }

    // The page has the focus (else the sidebar has): only then does its
    // selection show.
    void SetFocused(bool focused)
    {
        if (m_menu.MenuItems.empty())
            return;
        if (focused)
            m_menu.MenuItems[m_menu.CurrentSelection]->Select();
        else
            m_menu.MenuItems[m_menu.CurrentSelection]->Unselect();
    }

    // Puts the cursor back on the first entry - called by AppMenu whenever
    // this page becomes current. Virtual so pages can also refresh here.
    virtual void ResetSelection() { m_menu.ResetSelection(); }

    // Called whenever this page is about to show (navigated to, or the menu
    // reopening on it) - for pages that show settings something else may
    // have changed meanwhile.
    virtual void OnShow() {}

    // B (back) on this page - see AppMenu, which sets it for the sidebar's
    // own pages.
    void SetBackPress(std::function<void()> back) { m_menu.BackPress = std::move(back); }
    bool HasBackAction() const { return static_cast<bool>(m_menu.BackPress); }
    // B / Y clicked in the hints row.
    void PressBack()
    {
        if (m_menu.BackPress)
        {
            const auto back = m_menu.BackPress;
            back();
        }
    }
    void PressY()
    {
        if (m_menu.YPress)
        {
            const auto y = m_menu.YPress;
            y();
        }
    }
    // Left at the page's left edge (to the sidebar).
    void SetLeftEdge(std::function<void()> leftEdge) { m_menu.LeftEdge = std::move(leftEdge); }

    // Suspends normal navigation to capture the next raw button press - see
    // Menu::CaptureHook.
    void SetCaptureHook(std::function<bool(uint32_t *, uint32_t *)> hook) { m_menu.CaptureHook = std::move(hook); }
    void SetRawCaptureHook(std::function<void(const ButtonMapper::MappedButton &)> hook) { m_menu.RawCaptureHook = std::move(hook); }
    void SubmitRawCaptureInput(const ButtonMapper::MappedButton &button) { m_menu.SubmitRawCaptureInput(button); }

protected:
    // A settings-style list filling the page's content area.
    std::shared_ptr<MenuList> MakeList(UiRenderer &ui, const UiMenuResources &resources, float x = 0, float width = 0);

    Menu m_menu;
    const UiMenuResources *m_resources = nullptr;
};
