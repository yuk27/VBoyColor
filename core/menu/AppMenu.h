#pragma once

#include "menu/MenuPage.h"
#include "menu/ThumbnailLibrary.h"
#include "menu/pages/AppMenuLayout.h"
#include "menu/pages/LibraryPage.h"
#include "menu/pages/SaveStatesPage.h"
#include "menu/pages/SettingsPage.h"
#include "menu/pages/EmulatorButtonMapPage.h"
#include "menu/pages/MoveScreenPage.h"
#include "menu/pages/AboutPage.h"
#include "gfx/UiRenderer.h"

#include <cstdint>
#include <memory>
#include <vector>

class Emulator;
struct AppSettings;
class Platform;

// Top-level menu system: a sidebar on the left (the VBoy Color logo, the
// pages - Library, Resume, Save states, Settings, About - and the clock)
// and the current page right of it, with the button hints along the bottom.
// The focus is either in the sidebar (Up/Down pick a page, which shows
// right away; A/Right go into it) or in the page (Left at its edge, or B,
// come back out - B resumes the game when one is loaded). Renders into an
// offscreen buffer each frame, then composites it onto the real target with
// rounded corners.
//
// Transition model (ported from FrontendGo's MenuGo):
//   StartTransition(target, dir)  ->  m_transitionState slides 1->0 over
//   kTransitionSpeed seconds using a sine-eased progress value.
//   dir: +1 = target slides in from right (or below), -1 = from left (above).
class AppMenu
{
public:
    // Layout constants are in pages/AppMenuLayout.h (shared with page files).
    static constexpr float kPanelCornerRadiusPx = 8.0f;
    static constexpr float kTransitionSpeed = 0.15f;
    static constexpr float kOpenCloseSpeed = 0.15f;

    enum SidebarItem
    {
        SidebarLibrary,
        SidebarResume,
        SidebarSaveStates,
        SidebarSettings,
        SidebarAbout,
        SidebarCount
    };

    void Initialize(UiRenderer &ui, VkFormat targetFormat, Emulator &emulator, AppSettings &settings,
                    Platform &platform, ButtonMappingProfile mappingProfile = ButtonMappingProfile::Vr,
                    bool passthroughSupported = false);
    void Update(uint32_t buttonStates[3], uint32_t lastButtonStates[3], float deltaSeconds);
    void SubmitRawMappingInput(const ButtonMapper::MappedButton &button);
    void RenderToBuffer(UiRenderer &ui);
    void Draw(UiRenderer &ui, float x, float y);

    XrColor4f GetBackgroundColor() const;

    // The mouse (desktop) or a controller's laser (Quest) - call before
    // Update each frame. present: it's over the menu, at x, y in the menu's
    // logical units (kMenuWidth x kMenuHeight); down: its button (mouse
    // button, trigger) is held; scroll: wheel/stick scroll this frame, in
    // rows (+ = down). drawDot: draw where it points (the laser's end -
    // a mouse has its own cursor).
    void SetPointer(bool present, float x, float y, bool down, float scroll = 0.0f, bool drawDot = false);

    // Changes the logical-to-physical scale (see AppMenuLayout.h's
    // kMenuScale doc comment) at runtime - e.g. a resizable window
    // recomputing the largest integer scale that still fits every frame.
    // No-op if scale already matches (cheap to call unconditionally every
    // frame); otherwise re-renders the offscreen texture and re-bakes fonts
    // at the new physical resolution so text stays crisp at any size.
    void SetMenuScale(UiRenderer &ui, float scale);
    float GetMenuScale() const { return m_menuScale; }

    // Battery level shown at the bottom of the sidebar. percent: 0-100
    // shows it: any value outside that range (default -1) hides it
    // entirely, so callers opt in explicitly instead of the indicator
    // silently showing a stale/fake reading. OpenXrApp polls the real device
    // battery via Platform::GetBatteryPercent (see
    // OpenXrApp::UpdateBatteryPercent); it stays hidden on both desktop
    // builds, which have no battery worth reading.
    void SetBatteryPercent(int percent) { m_batteryPercent = percent; }

    // Menu open/closed - closing lets the emulator screen show unobstructed
    // instead of always sitting under the menu panel. "Resume" and picking a
    // ROM both close it; callers are responsible for wiring some way back
    // in (a controller button, a keyboard key - see OpenXrApp/pc2d Main.cpp)
    // since AppMenu itself only tracks the state, not any particular input.
    // IsOpen() flips immediately. The select/back press that closes the menu
    // is filtered from gameplay until release; other inputs pass immediately.
    // IsVisible() stays true until the fade-out animation finishes, so
    // callers doing the render-gating (RenderMenuLayer/pc2d's Main.cpp)
    // should check IsVisible(), not IsOpen(), or the close animation never
    // gets a frame to actually show.
    bool IsOpen() const { return m_open; }
    void ApplyGameplayInputSuppression(uint32_t buttonStates[3]) const;
    // For platforms that read raw keys instead of the bitmask (pc2d's
    // keyboard bindings), which ApplyGameplayInputSuppression can't reach.
    bool SuppressesDesktopKey(uint32_t emuButton) const
    {
        return (m_suppressedMenuButtons[ButtonMapper::DeviceRightTouch] & ButtonMapper::ButtonMapping[emuButton]) != 0;
    }
    bool IsVisible() const { return m_visibility > 0.0f; }
    void Show()
    {
        if (!m_open && m_currentPage)
            m_currentPage->OnShow();
        m_open = true;
    }
    void Hide() { m_open = false; }
    void ToggleOpen()
    {
        if (m_open)
            m_open = false;
        else
            Show();
    }

    // 0 (fully closed) .. 1 (fully open) - drives the panel's fade/scale.
    // Composite callers multiply this into the panel's draw alpha.
    float GetVisibility() const { return m_visibility; }

    ThumbnailLibrary &Thumbnails() { return m_thumbnails; }

private:
    class Sidebar;
    struct Logo
    {
        // The source image (RGBA), and a texture holding it resampled for
        // the current menu scale (see RebuildLogo).
        std::vector<uint8_t> pixels;
        uint32_t width = 0, height = 0;
        UiImageHandle texture;
        uint32_t texWidth = 0, texHeight = 0;
        uint32_t drawWidth = 0, drawHeight = 0;
    };

    void InitPages(UiRenderer &ui);
    void LoadFonts(UiRenderer &ui, bool rebake);
    void StartTransition(MenuPage *target, int dir, bool vertical = false);
    void GoTo(MenuPage *target, int dir);
    MenuPage *TargetPage() const { return m_nextPage ? m_nextPage : m_currentPage; }
    MenuPage *PageFor(int item);
    bool SidebarEnabled(int item) const;
    void MoveSidebar(int dir);
    void ActivateSidebar(bool pressedA);
    void FocusSidebar();
    void FocusContent();
    void BackFromPage();
    void HandlePointer();
    float SidebarItemY(int item) const;
    float SidebarItemHeight(int item) const;
    std::vector<MenuHint> CurrentHints() const;

    void RenderContent(UiRenderer &ui);
    void DrawSidebar(UiRenderer &ui);
    void DrawHints(UiRenderer &ui);
    void RebuildLogo(UiRenderer &ui);

    LibraryPage m_libraryPage;
    SaveStatesPage m_saveStatesPage;
    SettingsPage m_settingsPage;
    EmulatorButtonMapPage m_emulatorButtonMapPage;
    MoveScreenPage m_moveScreenPage;
    AboutPage m_aboutPage;

    ThumbnailLibrary m_thumbnails;

    MenuPage *m_currentPage = nullptr;
    MenuPage *m_nextPage = nullptr;
    float m_transitionState = 0.0f;
    int m_transitionDir = 1;
    bool m_transitionVertical = false;
    // A page switch requested while closed, applied on reopen instead of
    // immediately - see StartTransition/Update.
    MenuPage *m_pendingPage = nullptr;

    // The sidebar: has the focus (else the page has), and its cursor.
    Menu m_sidebarMenu;
    bool m_sidebarFocus = false;
    int m_sidebarCursor = SidebarLibrary;

    // The pointer (see SetPointer).
    struct Pointer
    {
        bool present = false, down = false, wasDown = false, drawDot = false;
        float x = 0, y = 0, lastX = -1, lastY = -1, scroll = 0;
    } m_pointer;
    int m_sidebarHover = -1;
    // The hints row's hints, where they were last drawn (clickable).
    struct HintRect
    {
        UiIconId icon;
        float x0, x1;
    };
    std::vector<HintRect> m_hintRects;

    UiRenderer *m_ui = nullptr;
    Logo m_logo;
    UiIconSet m_icons;
    UiMenuResources m_resources; // fonts/&m_icons - see UiMenuResources.h
    UiImageHandle m_offscreenTexture;
    float m_menuScale = kMenuScale; // see SetMenuScale
    std::string m_versionLine1, m_versionLine2;

    int m_batteryPercent = -1;
    bool m_open = true;
    uint32_t m_suppressedMenuButtons[3]{};
    float m_visibility = 1.0f; // see IsVisible/GetVisibility - starts matching m_open, no animation at boot
};
