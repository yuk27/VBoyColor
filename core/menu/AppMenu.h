#pragma once

#include "menu/MenuPage.h"
#include "menu/ThumbnailLibrary.h"
#include "io/Updater.h"
#include "menu/pages/AppMenuLayout.h"
#include "menu/pages/LibraryPage.h"
#include "menu/pages/SaveStatesPage.h"
#include "menu/pages/SettingsPage.h"
#include "menu/pages/EmulatorButtonMapPage.h"
#include "menu/pages/GameControlsPage.h"
#include "io/GameControls.h"
#include "menu/pages/MoveScreenPage.h"
#include "menu/pages/FolderPage.h"
#include "menu/pages/AboutPage.h"
#include "menu/pages/AchievementsPage.h"
#include "menu/pages/TextInputPage.h"
#include "menu/pages/HotkeysPage.h"
#include "menu/pages/SupportPage.h"
#include "input/Hotkeys.h"
#include "achievements/Achievements.h"
#include "gfx/UiRenderer.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

class Emulator;
struct AppSettings;
class Platform;

// Top-level menu system: a sidebar on the left (the VBoy Color logo, the
// pages - Library, Resume, Save states, Settings, About - then Close game and
// Quit where they apply, and the clock)
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

    // (in the order they're listed: back to the game first)
    enum SidebarItem
    {
        SidebarResume,
        SidebarLibrary,
        SidebarSaveStates,
        SidebarAchievements,
        SidebarSettings,
        SidebarAbout,
        SidebarCloseGame, // (while a game runs: back to the library, nothing running)
        SidebarSupport,   // (a coffee, and Yuno's Adventure)
        SidebarQuit,      // (where the app can close itself - Platform::CanQuit)
        SidebarCount
    };

    void Initialize(UiRenderer &ui, VkFormat targetFormat, Emulator &emulator, AppSettings &settings,
                    Platform &platform, ButtonMappingProfile mappingProfile = ButtonMappingProfile::Vr,
                    bool passthroughSupported = false);
    void Update(uint32_t buttonStates[3], uint32_t lastButtonStates[3], float deltaSeconds);
    void SubmitRawMappingInput(const ButtonMapper::MappedButton &button);
    void RenderToBuffer(UiRenderer &ui);
    // squeezeX/Y: drawn that much narrower/shorter from (x, y) - once per eye's
    // half of a 3D TV picture (Settings > Screen > 3D, side by side or top
    // and bottom), which the TV stretches back.
    void Draw(UiRenderer &ui, float x, float y, float squeezeX = 1.0f, float squeezeY = 1.0f);

    XrColor4f GetBackgroundColor() const;

    // The mouse (desktop) or a controller's laser (Quest) - call before
    // Update each frame. present: it's over the menu, at x, y in the menu's
    // logical units (kMenuWidth x kMenuHeight); down: its button (mouse
    // button, trigger) is held; scroll: wheel/stick scroll this frame, in
    // rows (+ = down). drawDot: draw where it points (the laser's end -
    // a mouse has its own cursor). dragToScroll (the lasers): a quick press
    // clicks (on release); held and moved, it drags - lists and the library
    // follow it - and doesn't click. Without it (the mouse) a press clicks.
    void SetPointer(bool present, float x, float y, bool down, float scroll = 0.0f, bool drawDot = false,
                    bool dragToScroll = false);
    // The mouse's right button (held) - a click with it is what a long press
    // is to a finger: a library card's options. Call before Update.
    void SetPointerSecondary(bool down) { m_pointer.secondaryDown = down; }

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
    // The phone app's touch controls editor (Settings > Touch controls).
    void SetEditTouchControls(std::function<void(const std::string &game)> edit,
                              std::function<void(const std::string &game)> forget)
    {
        m_resources.editTouchControls = std::move(edit);
        m_resources.forgetTouchControls = std::move(forget);
    }
    // The games' own button mappings: the mapping in force for the game
    // playing is Controls().Buttons(settings).
    GameControls &Controls() { return m_gameControls; }
    const GameControls &Controls() const { return m_gameControls; }

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

    // Anything to draw this frame: the menu, or a notice over the game
    // (an achievement unlocked - see Achievements::TakeNotice). Platforms call
    // RenderToBuffer and Draw while this is true (not only while visible).
    bool NeedsDraw() const { return IsVisible() || NoticeShowing(); }
    bool NoticeShowing() const { return m_notice.active && (!IsVisible() || m_notice.overMenu); }

    // Typing on a real keyboard (the window app): while a text field is open
    // (WantsText), its characters (UTF-8), Backspace and Enter go to it - and
    // the keys the menu otherwise uses (S, A, W, D, Backspace) don't. Paste
    // and Copy: Ctrl+V and Ctrl+C (the system clipboard, Platform's).
    bool WantsText() const;
    void TypeText(const std::string &utf8);
    void TypeBackspace();
    void TypeEnter();
    void TypePaste();
    void TypeCopy();

    // The window app's hotkeys (Settings > Hotkeys): the action a key press
    // (with Ctrl / Alt / Shift, kHotkey*) runs, if any - HotkeyAction::Menu and
    // ::Fullscreen are the platform's own (it toggles the menu, the window),
    // the rest RunHotkeyAction does. While a key is being bound (or text
    // typed) there's none.
    bool HotkeyFor(int key, int mods, HotkeyAction &action) const;
    // Save state, Load state, the slot, Screenshot, Reset, Close game, Quit -
    // with a notice saying what happened. False for one it doesn't do.
    bool RunHotkeyAction(HotkeyAction action);
    // A key pressed with these modifiers (the window app, every key) - for
    // Settings > Hotkeys while it's waiting for one.
    void SubmitRawKey(int key, int mods);
    // The game's picture to the screenshots folder, and a notice saying so.
    void TakeScreenshot();
    // A notice of the app's own ("Screenshot saved"), over the game or the
    // menu - one showing already is replaced (an achievement's waits).
    void ShowNotice(const std::string &caption, const std::string &title, const std::string &text, UiIconId icon,
                    float seconds = 2.5f);

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
    // Close game and Quit aren't there at all where they don't apply.
    bool SidebarVisible(int item) const;
    void CloseGame();
    void Quit();
    void MoveSidebar(int dir);
    void ActivateSidebar(bool pressedA);
    void FocusSidebar();
    void FocusContent();
    void BackFromPage();
    void HandlePointer(float deltaSeconds);
    float SidebarItemY(int item) const;
    float SidebarItemHeight(int item) const;
    std::vector<MenuHint> CurrentHints() const;

    void RenderContent(UiRenderer &ui);
    void DrawSidebar(UiRenderer &ui);
    void DrawHints(UiRenderer &ui);
    void LoadLogo(UiRenderer &ui);
    void RebuildLogo(UiRenderer &ui);
    // Notices over the game (see NoticeShowing).
    void UpdateNotice(float deltaSeconds);
    void RenderNotice(UiRenderer &ui);
    void DrawNotice(UiRenderer &ui, float x, float y, float squeezeX, float squeezeY);
    int m_logoTries = 0;

    LibraryPage m_libraryPage;
    SaveStatesPage m_saveStatesPage;
    SettingsPage m_settingsPage;
    EmulatorButtonMapPage m_emulatorButtonMapPage;
    MoveScreenPage m_moveScreenPage;
    FolderPage m_folderPage;
    GameControlsPage m_gameControlsPage;
    AboutPage m_aboutPage;
    AchievementsPage m_achievementsPage;
    TextInputPage m_textInputPage;
    HotkeysPage m_hotkeysPage;
    SupportPage m_supportPage;
    GameControls m_gameControls;

    ThumbnailLibrary m_thumbnails;
    // A newer VBoy Color: checked once a few seconds after the start (if
    // Settings say so - About has the switch), or from About.
    Updater m_updater;
    float m_sinceStart = 0.0f;
    bool m_autoChecked = false;

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
    // Close game / Quit: the first press asks ("Press again"), a second one
    // within a few seconds does it - moving away forgets it.
    int m_confirmItem = -1;
    float m_confirmSeconds = 0.0f;

    // The pointer (see SetPointer).
    struct Pointer
    {
        bool present = false, down = false, wasDown = false, drawDot = false;
        float x = 0, y = 0, lastX = -1, lastY = -1, scroll = 0;
        // dragToScroll's gesture: pressed on the menu (not yet a click or a
        // drag), where, and whether it moved far enough to be a drag.
        bool dragToScroll = false, pressed = false, dragging = false;
        float pressX = 0, pressY = 0, velocityY = 0;
        bool secondaryDown = false, secondaryWasDown = false; // the mouse's right button
        float heldSeconds = 0;  // how long it's been held still
        bool longFired = false; // a long press was reported for this press
        // A stick or D-pad stepped the selection: where the pointer was then.
        // Until it moves well away from there it doesn't select by hovering -
        // a laser held in a hand always moves a little, and would otherwise
        // take the selection straight back from the stick.
        bool navHeld = false;
        float navX = 0, navY = 0;
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
    // The notice showing (an achievement unlocked...), rendered into its own
    // texture and drawn at the top of where the menu would be.
    struct ShownNotice
    {
        bool active = false;
        Achievements::Notice notice;
        float age = 0; // seconds shown
        // A notice of the app's own (a newer VBoy Color): its icon instead of
        // a badge, and shown over the menu too (at its bottom) - an
        // achievement's waits for the game.
        UiIconId icon = UiIconId::None;
        bool overMenu = false;
        bool replaceable = false; // (see AppNotice)
    } m_notice;
    // A newer VBoy Color turned up on the check at start: tell once.
    bool m_updateNoticeShown = false;
    // Notices of the app's own waiting to show (ShowNotice).
    struct AppNotice
    {
        Achievements::Notice notice;
        UiIconId icon;
        bool replaceable = true; // (a key's - the next press's replaces it; not the update's)
    };
    std::deque<AppNotice> m_appNotices;
    bool m_noticeQuickIn = false;
    UiImageHandle m_noticeTexture;
    float m_menuScale = kMenuScale; // see SetMenuScale
    std::string m_versionLine1, m_versionLine2;

    int m_batteryPercent = -1;
    bool m_open = true;
    uint32_t m_suppressedMenuButtons[3]{};
    float m_visibility = 1.0f; // see IsVisible/GetVisibility - starts matching m_open, no animation at boot
};
