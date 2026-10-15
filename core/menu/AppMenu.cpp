#include "menu/AppMenu.h"
#include "emu/Emulator.h"
#include "io/Platform.h"
#include "io/Settings.h"

#include <stb_image.h> // (implementation in VulkanRenderer.cpp)

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace
{
    // A log line: logcat on Android (tag VBoyColor), stderr elsewhere.
    void LogLine(const char *fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
#ifdef __ANDROID__
        __android_log_vprint(ANDROID_LOG_INFO, "VBoyColor", fmt, args);
#else
        std::vfprintf(stderr, fmt, args);
        std::fputc('\n', stderr);
#endif
        va_end(args);
    }

    constexpr XrColor4f kClearColor = {0.0f, 0.0f, 0.0f, 1.0f};

    // The logo's colors (Juan's VBoy Color logo): "VBOY" red, then "COLOR"
    // a letter at a time - also the stripe under the sidebar's logo.
    XrColor4f TitleLetterColor(int i)
    {
        static constexpr uint8_t kLetters[5][3] = {{205, 37, 57}, {88, 81, 166}, {163, 198, 24}, {215, 179, 1}, {0, 155, 166}};
        const uint8_t *c = kLetters[i % 5];
        return XrColor4f{c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, 1.0f};
    }

    // The sidebar's logo: assets/runtime/logo/vboycolor_icon.png (Juan's
    // stacked "VBOY / COLOR" logo, transparent), this tall in menu units -
    // resampled for each menu scale up to kLogoMaxScale (the desktop window
    // fullscreen at 4K), so it's drawn 1:1 and stays sharp.
    constexpr float kLogoHeight = 18.0f;
    constexpr float kLogoX = 8.0f, kLogoY = 11.0f;
    constexpr int kLogoMaxScale = 12;

    // The sidebar's pages.
    constexpr float kNavTop = 46.0f;
    constexpr float kNavPitch = 18.0f; // (room for Close game, Support and Quit above the clock)
    constexpr float kNavSubtitleExtra = 7.0f; // (Resume shows the game under it)
    constexpr float kNavIconSize = 10.0f;
    // A notice over the game (an achievement unlocked), in the menu's logical units.
    constexpr float kNoticeWidth = 236.0f, kNoticeHeight = 40.0f;

    // The fonts (see AppMenuLayout.h): which resource, bold, size.
    struct FontSpec
    {
        UiFontHandle UiMenuResources::*field;
        bool bold;
        float size;
    };
    constexpr FontSpec kFonts[] = {
        {&UiMenuResources::titleFont, true, kTitleFontSize},
        {&UiMenuResources::bodyFont, false, kBodyFontSize},
        {&UiMenuResources::bodyBoldFont, true, kBodyFontSize},
        {&UiMenuResources::cardFont, false, kCardFontSize},
        {&UiMenuResources::cardBoldFont, true, kCardFontSize},
        {&UiMenuResources::captionFont, false, kCaptionFontSize},
        {&UiMenuResources::captionBoldFont, true, kCaptionFontSize},
        {&UiMenuResources::smallFont, true, static_cast<float>(kSmallFontSize)},
    };

    XrColor4f WithAlpha(XrColor4f c, float alpha)
    {
        c.a *= alpha;
        return c;
    }

    float TextY(UiRenderer &ui, UiFontHandle font, float centerY)
    {
        return centerY - ui.GetFontPHeight(font) / 2.0f - ui.GetFontPStart(font);
    }

    std::string CurrentTimeString()
    {
        const std::time_t t = std::time(nullptr);
        std::tm tmv{};
#if defined(_WIN32)
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char buf[6]; // "HH:MM\0"
        std::snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        return buf;
    }

    std::string WithoutRegion(const std::string &name) { return name.substr(0, name.find(" (")); }
} // namespace

// The sidebar's one menu item: Up/Down move between its pages, A/Right go in.
class AppMenu::Sidebar : public MenuItem
{
public:
    explicit Sidebar(AppMenu &menu) : m_menu(menu)
    {
        Selectable = true;
        ScrollTimeV = 0.12f;
    }
    int PressedUp() override
    {
        m_menu.MoveSidebar(-1);
        return 1;
    }
    int PressedDown() override
    {
        m_menu.MoveSidebar(1);
        return 1;
    }
    int PressedRight() override
    {
        m_menu.ActivateSidebar(false);
        return 1;
    }
    int PressedEnter() override
    {
        m_menu.ActivateSidebar(true);
        return 1;
    }

private:
    AppMenu &m_menu;
};

// -----------------------------------------------------------------------
// Initialise

void AppMenu::LoadFonts(UiRenderer &ui, bool rebake)
{
    const std::vector<uint8_t> regular = m_resources.platform->LoadAssetBytes("fonts/Roboto-Regular.ttf");
    const std::vector<uint8_t> bold = m_resources.platform->LoadAssetBytes("fonts/Roboto-Bold.ttf");
    // Glyphs baked at physical resolution (the scale x the logical size) for
    // crisp text - see UiFontManager::LoadFont's renderScale doc comment.
    for (const FontSpec &spec : kFonts)
    {
        const int pixels = static_cast<int>(std::lround(spec.size * m_menuScale));
        UiFontHandle &font = m_resources.*spec.field;
        if (rebake)
            ui.RebakeFont(font, spec.bold ? bold : regular, pixels, m_menuScale);
        else
            font = ui.LoadFont(spec.bold ? bold : regular, pixels, m_menuScale);
    }
}

void AppMenu::Initialize(UiRenderer &ui, VkFormat targetFormat, Emulator &emulator, AppSettings &settings,
                         Platform &platform, ButtonMappingProfile mappingProfile, bool passthroughSupported)
{
    m_ui = &ui;
    m_resources.platform = &platform;
    LoadFonts(ui, false);
    ui.EnsureGlyphsForText(m_resources.captionFont, "\xC2\xB7\xE2\x80\xA6"); // (the clock line's dot, ellipses)

    m_icons.Load(ui, platform, m_menuScale);
    m_resources.icons = &m_icons;

    LoadLogo(ui);

    // The version beside the logo: "dev.155" over "v1.0.0" (a dev build),
    // "beta.2" over "v1.0.0", or just "v1.0.0".
    {
        const std::string version = kVersionString;
        const size_t dash = version.find('-');
        if (dash == std::string::npos)
            m_versionLine1 = version;
        else
        {
            m_versionLine1 = version.substr(dash + 1);
            const size_t dirty = m_versionLine1.find("-dirty");
            if (dirty != std::string::npos)
                m_versionLine1.erase(dirty);
            m_versionLine2 = version.substr(0, dash);
        }
    }

    m_resources.emulator = &emulator;
    m_resources.appMenu = this;
    m_resources.settings = &settings;
    // The window app's hotkeys start as their defaults (once - after that
    // they're the player's, cleared ones too).
    if (mappingProfile == ButtonMappingProfile::Desktop && !settings.hotkeysSet)
    {
        for (int i = 0; i < kHotkeySlots; ++i)
            settings.hotkeys[i] = i < kHotkeyActionCount ? DefaultHotkey(static_cast<HotkeyAction>(i)) : Hotkey{};
        settings.hotkeysSet = true;
    }
    m_resources.buttonMappingProfile = mappingProfile;
    m_resources.passthroughSupported = passthroughSupported;
    m_thumbnails.Init(ui, platform, emulator, settings);
    m_resources.thumbnails = &m_thumbnails;
    m_resources.gameControls = &m_gameControls;
    m_updater.Init(platform, kVersionString);
    m_resources.updater = &m_updater;
    // Physical pixel size - kMenuWidth/kMenuHeight are logical units (see
    // AppMenuLayout.h); RenderToBuffer maps them onto this full-resolution
    // texture via BeginOffscreenFrame's logicalWidth/logicalHeight, so the
    // menu rasterizes crisp at m_menuScale regardless of how small the
    // logical layout numbers are.
    m_offscreenTexture = ui.CreateRenderTexture(static_cast<uint32_t>(kMenuWidth * m_menuScale),
                                                static_cast<uint32_t>(kMenuHeight * m_menuScale), targetFormat);
    m_noticeTexture = ui.CreateRenderTexture(static_cast<uint32_t>(kNoticeWidth * m_menuScale),
                                             static_cast<uint32_t>(kNoticeHeight * m_menuScale), targetFormat);

    InitPages(ui);

    m_sidebarMenu.MenuItems.push_back(std::make_shared<Sidebar>(*this));
    m_sidebarMenu.BackPress = [this]()
    {
        if (m_resources.emulator->HasGame())
            Hide();
    };
    m_sidebarMenu.Init();

    // Start in the library - on its games, or (none yet) in the sidebar.
    m_currentPage = &m_libraryPage;
    m_currentPage->OnShow();
    if (m_libraryPage.HasFocusable())
        FocusContent();
    else
        FocusSidebar();
}

void AppMenu::SetMenuScale(UiRenderer &ui, float scale)
{
    if (scale == m_menuScale)
        return;
    m_menuScale = scale;
    m_icons.SetMenuScale(ui, *m_resources.platform, m_menuScale);

    ui.ResizeRenderTexture(m_offscreenTexture, static_cast<uint32_t>(kMenuWidth * m_menuScale),
                           static_cast<uint32_t>(kMenuHeight * m_menuScale));
    ui.ResizeRenderTexture(m_noticeTexture, static_cast<uint32_t>(kNoticeWidth * m_menuScale),
                           static_cast<uint32_t>(kNoticeHeight * m_menuScale));
    LoadFonts(ui, true);
    LoadLogo(ui); // (in case it didn't load the first time)
    RebuildLogo(ui);
}

// The logo, decoded once (RGBA) and resampled per menu scale (RebuildLogo).
void AppMenu::LoadLogo(UiRenderer &ui)
{
    if (m_logo.texture.IsValid() || m_logoTries >= 3)
        return;
    ++m_logoTries;
    const std::vector<uint8_t> png = m_resources.platform->LoadAssetBytes("logo/vboycolor_icon.png");
    int w = 0, h = 0, channels = 0;
    stbi_uc *pixels = png.empty() ? nullptr : stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &channels, 4);
    if (!pixels || w <= 0 || h <= 0)
    {
        LogLine("[Menu] The logo didn't load (logo/vboycolor_icon.png: %zu bytes, %s)", png.size(),
                png.empty() ? "not found" : stbi_failure_reason());
        if (pixels)
            stbi_image_free(pixels);
        return;
    }
    m_logo.pixels.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
    m_logo.width = static_cast<uint32_t>(w);
    m_logo.height = static_cast<uint32_t>(h);
    stbi_image_free(pixels);
    m_logo.texHeight = static_cast<uint32_t>(std::ceil(kLogoHeight * kLogoMaxScale));
    m_logo.texWidth = static_cast<uint32_t>(std::ceil(m_logo.texHeight * static_cast<float>(w) / h)) + 1;
    m_logo.texture = ui.CreateStreamingImage(m_logo.texWidth, m_logo.texHeight, VK_FORMAT_B8G8R8A8_SRGB);
    RebuildLogo(ui);
}

void AppMenu::RebuildLogo(UiRenderer &ui)
{
    Logo &logo = m_logo;
    if (!logo.texture.IsValid() || logo.pixels.empty())
        return;
    // Exactly as many pixels as the logo covers at this scale (at most the
    // texture's), each the average of the source pixels under it - in
    // premultiplied alpha, so the edges don't darken.
    const float scale = std::min(m_menuScale, static_cast<float>(kLogoMaxScale));
    const uint32_t h = std::max(1u, std::min(logo.texHeight, static_cast<uint32_t>(std::lround(kLogoHeight * scale))));
    const uint32_t w = std::max(1u, std::min(logo.texWidth, static_cast<uint32_t>(std::lround(h * static_cast<float>(logo.width) / logo.height))));
    std::vector<uint8_t> bgra(static_cast<size_t>(logo.texWidth) * logo.texHeight * 4, 0);
    const float sx = static_cast<float>(logo.width) / w, sy = static_cast<float>(logo.height) / h;
    for (uint32_t y = 0; y < h; ++y)
    {
        const float y0 = y * sy, y1 = (y + 1) * sy;
        for (uint32_t x = 0; x < w; ++x)
        {
            const float x0 = x * sx, x1 = (x + 1) * sx;
            float sum[4] = {0, 0, 0, 0}, area = 0;
            for (uint32_t iy = static_cast<uint32_t>(y0); iy < logo.height && iy < y1; ++iy)
            {
                const float wy = std::min(y1, iy + 1.0f) - std::max(y0, static_cast<float>(iy));
                for (uint32_t ix = static_cast<uint32_t>(x0); ix < logo.width && ix < x1; ++ix)
                {
                    const float wgt = wy * (std::min(x1, ix + 1.0f) - std::max(x0, static_cast<float>(ix)));
                    const uint8_t *p = &logo.pixels[(static_cast<size_t>(iy) * logo.width + ix) * 4];
                    const float a = p[3] / 255.0f * wgt;
                    sum[0] += p[0] * a;
                    sum[1] += p[1] * a;
                    sum[2] += p[2] * a;
                    sum[3] += a;
                    area += wgt;
                }
            }
            uint8_t *out = &bgra[(static_cast<size_t>(y) * logo.texWidth + x) * 4];
            if (sum[3] > 0.0f && area > 0.0f)
            {
                out[0] = static_cast<uint8_t>(std::lround(std::min(255.0f, sum[2] / sum[3])));
                out[1] = static_cast<uint8_t>(std::lround(std::min(255.0f, sum[1] / sum[3])));
                out[2] = static_cast<uint8_t>(std::lround(std::min(255.0f, sum[0] / sum[3])));
                out[3] = static_cast<uint8_t>(std::lround(std::min(255.0f, sum[3] / area * 255.0f)));
            }
        }
    }
    ui.UpdateStreamingImage(logo.texture, bgra.data(), bgra.size());
    logo.drawWidth = w;
    logo.drawHeight = h;
}

void AppMenu::InitPages(UiRenderer &ui)
{
    MenuPage *pages[] = {&m_libraryPage,      &m_saveStatesPage, &m_settingsPage, &m_emulatorButtonMapPage,
                         &m_moveScreenPage,   &m_folderPage,     &m_gameControlsPage, &m_aboutPage,
                         &m_achievementsPage, &m_textInputPage,  &m_hotkeysPage,      &m_supportPage};
    for (MenuPage *page : pages)
    {
        page->Navigate = [this](MenuPage *target, int dir) { StartTransition(target, dir); };
        page->Attach(m_resources);
    }

    m_libraryPage.sidebarItem = SidebarLibrary;
    m_saveStatesPage.sidebarItem = SidebarSaveStates;
    m_settingsPage.sidebarItem = SidebarSettings;
    m_emulatorButtonMapPage.sidebarItem = SidebarSettings;
    m_moveScreenPage.sidebarItem = SidebarSettings;
    m_folderPage.sidebarItem = SidebarSettings;
    m_gameControlsPage.sidebarItem = SidebarLibrary;
    m_aboutPage.sidebarItem = SidebarAbout;
    m_achievementsPage.sidebarItem = SidebarAchievements;
    m_textInputPage.sidebarItem = SidebarAchievements;
    m_hotkeysPage.sidebarItem = SidebarSettings;
    m_supportPage.sidebarItem = SidebarSupport;

    // Cross-page links
    m_settingsPage.emulatorButtonMapPage = &m_emulatorButtonMapPage;
    m_settingsPage.moveScreenPage = &m_moveScreenPage;
    m_emulatorButtonMapPage.settingsPage = &m_settingsPage;
    m_moveScreenPage.settingsPage = &m_settingsPage;
    m_settingsPage.folderPage = &m_folderPage;
    m_settingsPage.hotkeysPage = &m_hotkeysPage;
    m_hotkeysPage.settingsPage = &m_settingsPage;
    m_libraryPage.gameControlsPage = &m_gameControlsPage;
    m_gameControlsPage.libraryPage = &m_libraryPage;
    m_gameControlsPage.buttonMapPage = &m_emulatorButtonMapPage;
    m_folderPage.settingsPage = &m_settingsPage;
    m_achievementsPage.textInput = &m_textInputPage;
    // A kind's folder changed: the games folder's games, and the settings
    // written where they now live.
    m_folderPage.onChanged = [this](DataKind kind)
    {
        if (kind == DataKind::Games && m_resources.thumbnails)
            m_resources.thumbnails->Rescan();
        if ((kind == DataKind::Games || kind == DataKind::Settings) && m_resources.settings && m_resources.platform)
            m_resources.settings->Save(*m_resources.platform);
    };

    for (MenuPage *page : pages)
    {
        page->Init(ui, m_resources);
        page->SetLeftEdge([this]() { FocusSidebar(); });
    }
    // The sidebar's own pages: B goes back to the game (or the sidebar).
    for (MenuPage *page : {static_cast<MenuPage *>(&m_libraryPage), static_cast<MenuPage *>(&m_saveStatesPage),
                           static_cast<MenuPage *>(&m_achievementsPage), static_cast<MenuPage *>(&m_settingsPage),
                           static_cast<MenuPage *>(&m_aboutPage), static_cast<MenuPage *>(&m_supportPage)})
        page->SetBackPress([this]() { BackFromPage(); });
}

// -----------------------------------------------------------------------
// Navigation

MenuPage *AppMenu::PageFor(int item)
{
    switch (item)
    {
    case SidebarLibrary:
        return &m_libraryPage;
    case SidebarSaveStates:
        return &m_saveStatesPage;
    case SidebarAchievements:
        return &m_achievementsPage;
    case SidebarSettings:
        return &m_settingsPage;
    case SidebarAbout:
        return &m_aboutPage;
    case SidebarSupport:
        return &m_supportPage;
    default:
        return nullptr;
    }
}

bool AppMenu::SidebarVisible(int item) const
{
    if (item == SidebarCloseGame)
        return m_resources.emulator && m_resources.emulator->HasGame();
    if (item == SidebarQuit)
        return m_resources.platform && m_resources.platform->CanQuit();
    return item >= 0 && item < SidebarCount;
}

bool AppMenu::SidebarEnabled(int item) const
{
    if (item == SidebarResume || item == SidebarSaveStates)
        return m_resources.emulator && m_resources.emulator->HasGame();
    return SidebarVisible(item);
}

void AppMenu::StartTransition(MenuPage *target, int dir, bool vertical)
{
    if (!target)
        return;

    if (!m_open)
    {
        // Closing (e.g. a page hides the menu then navigates in the same
        // callback) - the close animation still shows whatever
        // m_currentPage is for the next ~kOpenCloseSpeed seconds (see
        // IsVisible()), so switching pages right now would flash the wrong
        // page as the menu disappears. Defer the switch instead: apply it
        // only once the menu actually reopens (see Update()).
        m_pendingPage = target;
        return;
    }

    // (one already sliding in lands at once)
    if (m_nextPage)
    {
        m_currentPage = m_nextPage;
        m_nextPage = nullptr;
        m_transitionState = 0.0f;
    }
    if (target == m_currentPage)
        return;
    m_nextPage = target;
    m_transitionDir = dir;
    m_transitionVertical = vertical;
    m_transitionState = 1.0f;
    target->OnShow();
    target->SetFocused(!m_sidebarFocus);
}

void AppMenu::GoTo(MenuPage *target, int dir) { StartTransition(target, dir, true); }

void AppMenu::MoveSidebar(int dir)
{
    int next = m_sidebarCursor;
    do
        next += dir;
    while (next >= 0 && next < SidebarCount && !SidebarEnabled(next));
    if (next < 0 || next >= SidebarCount)
        return;
    m_sidebarCursor = next;
    m_confirmItem = -1; // (moved away: not asked any more)
    // The page shows as soon as the cursor's on it.
    MenuPage *page = PageFor(next);
    if (page && TargetPage()->sidebarItem != next)
        GoTo(page, next > TargetPage()->sidebarItem ? 1 : -1);
}

void AppMenu::ActivateSidebar(bool pressedA)
{
    if (m_sidebarCursor == SidebarResume)
    {
        if (pressedA && SidebarEnabled(SidebarResume))
            Hide();
        return;
    }
    if (m_sidebarCursor == SidebarCloseGame || m_sidebarCursor == SidebarQuit)
    {
        // (A only - Right just moves on; asked first, done on a second press)
        if (!pressedA || !SidebarEnabled(m_sidebarCursor))
            return;
        if (m_confirmItem != m_sidebarCursor)
        {
            m_confirmItem = m_sidebarCursor;
            m_confirmSeconds = 3.0f;
            return;
        }
        m_confirmItem = -1;
        if (m_sidebarCursor == SidebarCloseGame)
            CloseGame();
        else
            Quit();
        return;
    }
    MenuPage *page = PageFor(m_sidebarCursor);
    if (!page)
        return;
    if (TargetPage()->sidebarItem != m_sidebarCursor)
        GoTo(page, m_sidebarCursor > TargetPage()->sidebarItem ? 1 : -1);
    if (TargetPage()->HasFocusable())
        FocusContent();
}

void AppMenu::CloseGame()
{
    if (!m_resources.emulator || !m_resources.emulator->CloseGame())
        return;
    // The library, nothing running.
    m_sidebarCursor = SidebarLibrary;
    GoTo(&m_libraryPage, -1);
    if (m_libraryPage.HasFocusable())
        FocusContent();
    else
        FocusSidebar();
    Show();
}

void AppMenu::Quit()
{
    if (!m_resources.platform || !m_resources.platform->CanQuit())
        return;
    // (the game's battery save now - the rest is written as on any close)
    if (m_resources.emulator && m_resources.emulator->HasGame())
        m_resources.emulator->SaveRamNow();
    if (m_resources.settings)
        m_resources.settings->Save(*m_resources.platform);
    m_resources.platform->RequestQuit();
}

void AppMenu::FocusSidebar()
{
    m_sidebarFocus = true;
    m_sidebarCursor = TargetPage() ? TargetPage()->sidebarItem : SidebarLibrary;
    if (TargetPage())
        TargetPage()->SetFocused(false);
}

void AppMenu::FocusContent()
{
    m_sidebarFocus = false;
    if (TargetPage())
        TargetPage()->SetFocused(true);
}

void AppMenu::BackFromPage()
{
    if (m_resources.emulator && m_resources.emulator->HasGame())
        Hide();
    else
        FocusSidebar();
}

// -----------------------------------------------------------------------
// Update

void AppMenu::SetPointer(bool present, float x, float y, bool down, float scroll, bool drawDot, bool dragToScroll)
{
    m_pointer.present = present;
    m_pointer.x = x;
    m_pointer.y = y;
    m_pointer.down = down;
    m_pointer.scroll += scroll;
    m_pointer.drawDot = drawDot;
    m_pointer.dragToScroll = dragToScroll;
}

float AppMenu::SidebarItemHeight(int item) const
{
    if (!SidebarVisible(item))
        return 0.0f;
    return kNavPitch + ((item == SidebarResume && SidebarEnabled(SidebarResume)) ||
                                (item == SidebarAbout && m_updater.UpdateAvailable()) || item == m_confirmItem
                            ? kNavSubtitleExtra
                            : 0.0f);
}

float AppMenu::SidebarItemY(int item) const
{
    float y = kNavTop;
    for (int i = 0; i < item; ++i)
        y += SidebarItemHeight(i);
    return y;
}

void AppMenu::HandlePointer(float deltaSeconds)
{
    Pointer &p = m_pointer;
    // (just arrived - e.g. the app starting under a resting mouse - isn't a move)
    if (p.present && p.lastX < 0.0f)
    {
        p.lastX = p.x;
        p.lastY = p.y;
    }
    bool moved = p.present && (std::abs(p.x - p.lastX) > 0.01f || std::abs(p.y - p.lastY) > 0.01f);
    if (p.navHeld)
    {
        constexpr float kNavRelease = 12.0f; // menu units: about two degrees of a laser
        if (!p.present || std::hypot(p.x - p.navX, p.y - p.navY) > kNavRelease)
            p.navHeld = false;
        else
            moved = false;
    }
    bool clicked = p.present && p.down && !p.wasDown;
    const bool pressedNow = clicked; // (the moment it's pressed, whatever a laser's click waits for)
    bool longPressed = p.present && p.secondaryDown && !p.secondaryWasDown; // (the mouse's right button)
    p.secondaryWasDown = p.secondaryDown;
    float dragY = 0.0f, flingY = 0.0f;
    if (p.dragToScroll)
    {
        // A laser: a press on the menu starts a gesture; let go before it
        // moved far (about a degree and a half - a trigger pull shakes the
        // laser a little) it's a click, where it was let go; moved further
        // while held, a drag - never a click.
        constexpr float kDragStart = 8.0f; // menu units
        // Held still that long: a long press (a library card's options) - and
        // letting go then isn't a click.
        constexpr float kLongPress = 0.55f;
        clicked = false;
        if (p.present && p.down && !p.wasDown)
        {
            p.pressed = true;
            p.dragging = false;
            p.pressX = p.x;
            p.pressY = p.y;
            p.velocityY = 0.0f;
            p.heldSeconds = 0.0f;
            p.longFired = false;
        }
        if (p.pressed && !p.present)
            p.pressed = p.dragging = false; // (the laser left the menu: cancelled)
        if (p.pressed && p.down && !p.dragging && !p.longFired)
        {
            p.heldSeconds += deltaSeconds;
            if (p.heldSeconds >= kLongPress && std::hypot(p.x - p.pressX, p.y - p.pressY) <= kDragStart)
                longPressed = p.longFired = true;
        }
        if (p.pressed && p.down)
        {
            if (!p.dragging && !p.longFired && std::hypot(p.x - p.pressX, p.y - p.pressY) > kDragStart)
            {
                p.dragging = true;
                dragY = p.y - p.pressY; // (from where it was pressed: no lost motion)
            }
            else if (p.dragging)
                dragY = p.y - p.lastY;
            p.velocityY = p.velocityY * 0.5f + dragY * 0.5f;
        }
        if (p.pressed && !p.down)
        {
            if (p.dragging)
                flingY = p.velocityY * 8.0f; // (a flick keeps it going a little)
            else if (!p.longFired)
                clicked = true;
            p.pressed = p.dragging = false;
        }
        if (p.dragging)
            moved = false; // (hovering doesn't select while dragging)
    }
    const float scroll = p.scroll;
    p.wasDown = p.down;
    p.lastX = p.x;
    p.lastY = p.y;
    p.scroll = 0.0f;
    m_sidebarHover = -1;
    if (!p.present)
    {
        p.lastX = p.lastY = -1.0f;
        return;
    }
    // Which part of the menu a drag (or its fling) belongs to: where it started.
    const bool fromPress = p.dragging || flingY != 0.0f;
    const float px = fromPress ? p.pressX : p.x, py = fromPress ? p.pressY : p.y;

    if (px < kSidebarWidth)
    {
        for (int i = 0; i < SidebarCount; ++i)
        {
            const float y = SidebarItemY(i) - 3.0f;
            if (SidebarEnabled(i) && !fromPress && p.y >= y && p.y < y + SidebarItemHeight(i) - 2.0f)
                m_sidebarHover = i;
        }
        if (clicked && m_sidebarHover >= 0)
        {
            FocusSidebar();
            m_sidebarCursor = m_sidebarHover;
            ActivateSidebar(true);
        }
        return;
    }

    if (py >= kMenuHeight - kHintsHeight)
    {
        // The hints are buttons too: B, Y and X.
        for (const HintRect &hint : m_hintRects)
        {
            if (!clicked || p.x < hint.x0 || p.x > hint.x1)
                continue;
            if (hint.icon == UiIconId::ButtonB)
            {
                if (m_sidebarFocus)
                {
                    if (m_sidebarMenu.BackPress)
                        m_sidebarMenu.BackPress();
                }
                else if (TargetPage())
                    TargetPage()->PressBack();
            }
            else if (hint.icon == UiIconId::ButtonY && !m_sidebarFocus && TargetPage())
                TargetPage()->PressY();
            else if (hint.icon == UiIconId::ButtonX && !m_sidebarFocus && TargetPage())
                TargetPage()->PressX();
        }
        return;
    }

    MenuPage *page = TargetPage();
    if (!page || m_nextPage)
        return;
    if ((moved || clicked || pressedNow) && m_sidebarFocus && page->HasFocusable())
        FocusContent();
    MenuPointer pointer;
    pointer.x = p.x;
    pointer.y = p.y;
    pointer.moved = moved;
    pointer.clicked = clicked;
    pointer.pressed = pressedNow && !p.dragging;
    pointer.longPressed = longPressed;
    pointer.scroll = scroll;
    pointer.dragging = p.dragging;
    pointer.pressX = px;
    pointer.pressY = py;
    pointer.dragY = dragY;
    pointer.flingY = flingY;
    page->HandlePointer(pointer);
}

void AppMenu::Update(uint32_t buttonStates[3], uint32_t lastButtonStates[3], float deltaSeconds)
{
    // The game playing's own button mapping, if it has one (read when the game changes).
    if (m_resources.platform && m_resources.emulator)
        m_gameControls.SetGame(*m_resources.platform, m_resources.emulator->RomName());

    // Per-bit, not all-or-nothing: releasing one of two suppressed buttons
    // must free it up again even while the other is still held.
    for (int device = 0; device < 3; ++device)
        m_suppressedMenuButtons[device] &= buttonStates[device];

    // Animate the open/close fade regardless of m_open, so Hide() eases the
    // panel out instead of popping it away the instant gameplay input
    // resumes (see IsOpen() vs IsVisible()'s doc comment).
    const float visibilityTarget = m_open ? 1.0f : 0.0f;
    if (m_visibility != visibilityTarget)
    {
        const float step = deltaSeconds / kOpenCloseSpeed;
        m_visibility = (m_visibility < visibilityTarget) ? std::min(visibilityTarget, m_visibility + step)
                                                          : std::max(visibilityTarget, m_visibility - step);
    }

    // Thumbnails are made while the menu is open (the game paused).
    // (not while Yuno's Adventure is being played: decoding and uploading them makes it stutter)
    m_thumbnails.Update(m_open && !m_supportPage.Playing());

    // RetroAchievements: its web requests, and its notices over the game.
    if (m_resources.emulator)
        m_resources.emulator->GetAchievements().Update(deltaSeconds, !m_open && m_resources.emulator->HasGame());
    UpdateNotice(deltaSeconds);

    // (Close game / Quit asked: forgotten after a while, or with the sidebar left)
    if (m_confirmItem >= 0)
    {
        m_confirmSeconds -= deltaSeconds;
        if (m_confirmSeconds <= 0.0f || !m_open || !m_sidebarFocus || m_sidebarCursor != m_confirmItem)
            m_confirmItem = -1;
    }

    // A newer VBoy Color: one look a few seconds in (a development build
    // only when asked - see Updater).
    m_updater.Update();
    m_sinceStart += deltaSeconds;
    if (!m_autoChecked && m_sinceStart > 3.0f)
    {
        if (!m_resources.settings || !m_resources.settings->checkForUpdates || m_updater.IsDevBuild() || !m_updater.Supported())
            m_autoChecked = true;
        else
        {
            m_updater.Check();
            m_autoChecked = m_updater.GetState() != Updater::State::Idle; // (else the download slot was busy - next frame)
        }
    }
    // ... and when that check finds one, a notice says so (once).
    if (m_autoChecked && !m_updateNoticeShown && m_updater.GetState() == Updater::State::Available && !m_updater.IsDevBuild())
    {
        m_updateNoticeShown = true;
        Achievements::Notice notice;
        notice.caption = "Update available";
        notice.title = "VBoy Color " + m_updater.LatestName() + " is out";
        notice.text = "About > Update installs it";
        notice.seconds = 7.0f;
        m_appNotices.push_back({std::move(notice), UiIconId::Load, false});
    }

    if (m_open && m_pendingPage)
    {
        // Apply a page switch deferred by StartTransition while closed (see
        // its doc comment) - now that we're open again, swap before this
        // frame renders so the reopen shows the target page directly
        // instead of briefly flashing whatever was showing when it closed.
        m_currentPage = m_pendingPage;
        m_pendingPage = nullptr;
        m_currentPage->ResetSelection();
        m_currentPage->OnShow();
        m_currentPage->SetFocused(!m_sidebarFocus);
    }

    if (!m_open)
    {
        m_pointer.wasDown = m_pointer.down;
        m_pointer.scroll = 0.0f;
        m_pointer.pressed = m_pointer.dragging = false;
        return; // closed - no page should react to input meant for gameplay
    }

    // (the sidebar's cursor stays on something there - e.g. no game: no Resume)
    if (!SidebarEnabled(m_sidebarCursor))
        m_sidebarCursor = TargetPage() ? TargetPage()->sidebarItem : SidebarLibrary;

    if (m_transitionState > 0.0f)
    {
        m_transitionState -= deltaSeconds / kTransitionSpeed;
        if (m_transitionState <= 0.0f)
        {
            m_transitionState = 0.0f;
            m_currentPage = m_nextPage;
            m_nextPage = nullptr;
        }
    }

    // A stick or D-pad held: the pointer leaves the selection to it (see
    // Pointer::navHeld).
    {
        using namespace ButtonMapper;
        const uint32_t nav = ButtonMapping[EmuButton_Up] | ButtonMapping[EmuButton_Down] | ButtonMapping[EmuButton_Left] |
                             ButtonMapping[EmuButton_Right];
        if ((buttonStates[0] | buttonStates[1] | buttonStates[2]) & nav)
        {
            m_pointer.navHeld = true;
            m_pointer.navX = m_pointer.x;
            m_pointer.navY = m_pointer.y;
        }
    }
    HandlePointer(deltaSeconds);

    const bool wasOpen = m_open;
    if (m_sidebarFocus)
        m_sidebarMenu.Update(buttonStates, lastButtonStates, deltaSeconds);
    else if (m_transitionState <= 0.0f && m_currentPage)
    {
        m_currentPage->Update(buttonStates, lastButtonStates, deltaSeconds);
        if (m_sidebarFocus) // (it moved to the sidebar meanwhile)
            m_currentPage->SetFocused(false);
    }
    if (wasOpen && !m_open)
    {
        // Both the press that picked a menu entry (A, or a pointer's
        // trigger) and the one that backed out of the menu (B) are still
        // held as gameplay resumes.
        const uint32_t closeMask = ButtonMapper::ButtonMapping[ButtonMapper::EmuButton_A] |
                                   ButtonMapper::ButtonMapping[ButtonMapper::EmuButton_B] |
                                   ButtonMapper::ButtonMapping[ButtonMapper::EmuButton_Trigger];
        for (int device = 0; device < 3; ++device)
            m_suppressedMenuButtons[device] = buttonStates[device] & closeMask;
    }

    // (the subtitle may name a game - its glyphs baked before drawing)
    if (m_ui && TargetPage())
        m_ui->EnsureGlyphsForText(m_resources.cardFont, TargetPage()->Subtitle());
    if (m_ui && m_resources.emulator)
        m_ui->EnsureGlyphsForText(m_resources.captionFont, m_resources.emulator->RomName() + "\xE2\x80\xA6");
}

void AppMenu::ApplyGameplayInputSuppression(uint32_t buttonStates[3]) const
{
    for (int device = 0; device < 3; ++device)
        buttonStates[device] &= ~m_suppressedMenuButtons[device];
}

void AppMenu::SubmitRawMappingInput(const ButtonMapper::MappedButton &button)
{
    if (m_open && m_currentPage && m_transitionState <= 0.0f && !m_sidebarFocus)
        m_currentPage->SubmitRawCaptureInput(button);
}

// -----------------------------------------------------------------------
// Rendering

XrColor4f AppMenu::GetBackgroundColor() const { return kClearColor; }

std::vector<MenuHint> AppMenu::CurrentHints() const
{
    const bool game = m_resources.emulator && m_resources.emulator->HasGame();
    if (m_sidebarFocus)
    {
        std::vector<MenuHint> hints;
        if (m_sidebarCursor == SidebarResume)
            hints.push_back({UiIconId::ButtonA, "Resume"});
        else if (m_sidebarCursor == SidebarCloseGame || m_sidebarCursor == SidebarQuit)
        {
            const bool asked = m_confirmItem == m_sidebarCursor, quit = m_sidebarCursor == SidebarQuit;
            hints.push_back({UiIconId::ButtonA, asked ? (quit ? "Yes, quit" : "Yes, close it") : quit ? "Quit" : "Close game"});
        }
        else if (TargetPage() && TargetPage()->HasFocusable())
            hints.push_back({UiIconId::ButtonA, "Open"});
        if (game)
            hints.push_back({UiIconId::ButtonB, "Resume"});
        return hints;
    }
    MenuPage *page = TargetPage();
    if (!page)
        return {};
    std::vector<MenuHint> hints = page->Hints();
    // On the sidebar's own pages B goes back to the game.
    const bool topLevel = page == &m_libraryPage || page == &m_saveStatesPage || page == &m_achievementsPage ||
                          page == &m_settingsPage || page == &m_aboutPage || page == &m_supportPage;
    for (MenuHint &hint : hints)
        if (hint.icon == UiIconId::ButtonB && topLevel && game)
            hint.label = "Resume";
    return hints;
}

void AppMenu::DrawHints(UiRenderer &ui)
{
    const float lineY = kMenuHeight - kHintsHeight;
    ui.DrawQuad(kContentX - 2.0f, lineY, kContentRight - kContentX + 4.0f, 0.6f, kMenuLineColor);
    constexpr float kIcon = 10.0f, kIconGap = 3.0f, kGroupGap = 9.0f;
    const float centerY = lineY + kHintsHeight / 2.0f + 0.5f;
    const UiFontHandle font = m_resources.cardFont;
    float x = kContentX;
    m_hintRects.clear();
    for (const MenuHint &hint : CurrentHints())
    {
        const float x0 = x;
        m_icons.Draw(ui, hint.icon, x, centerY - kIcon / 2.0f, kIcon, 1.0f, kMenuTextColor);
        x += kIcon + kIconGap;
        ui.DrawText(font, hint.label, x, TextY(ui, font, centerY), 1.0f, kMenuDimTextColor);
        x += ui.GetTextWidth(font, hint.label);
        m_hintRects.push_back({hint.icon, x0 - 2.0f, x + 2.0f});
        x += kGroupGap;
    }
}

void AppMenu::DrawSidebar(UiRenderer &ui)
{
    ui.DrawQuad(0, 0, kSidebarWidth, kMenuHeight, kMenuSidebarColor);
    ui.DrawQuad(kSidebarWidth - 0.6f, 0, 0.6f, kMenuHeight, kMenuLineColor);

    // The logo, 1:1 with the texture's pixels, and the version beside it.
    float textX = kLogoX;
    if (m_logo.texture.IsValid() && m_logo.drawHeight > 0)
    {
        const float w = m_logo.drawWidth / m_menuScale, h = m_logo.drawHeight / m_menuScale;
        const float x = std::round(kLogoX * m_menuScale) / m_menuScale;
        const float y = std::round(kLogoY * m_menuScale) / m_menuScale;
        ui.DrawImageRegion(m_logo.texture, x, y, w, h, 0.0f, 0.0f, static_cast<float>(m_logo.drawWidth) / m_logo.texWidth,
                           static_cast<float>(m_logo.drawHeight) / m_logo.texHeight);
        textX = kLogoX + w + 5.0f;
    }
    else
    {
        // (no logo: the name, the version under it - not over each other)
        ui.DrawText(m_resources.bodyBoldFont, "VBoy Color", kLogoX, TextY(ui, m_resources.bodyBoldFont, kLogoY + 3.0f), 1.0f,
                    kMenuTextColor);
        const std::string version = m_versionLine2.empty() ? m_versionLine1 : m_versionLine1 + "  " + m_versionLine2;
        ui.DrawText(m_resources.captionFont, version, kLogoX, TextY(ui, m_resources.captionFont, kLogoY + 14.0f), 1.0f,
                    kMenuDimTextColor);
    }
    const float logoCenter = kLogoY + kLogoHeight / 2.0f;
    if (textX == kLogoX)
    {
        // (no logo: drawn with the name, above)
    }
    else if (m_versionLine2.empty())
        ui.DrawText(m_resources.cardBoldFont, m_versionLine1, textX, TextY(ui, m_resources.cardBoldFont, logoCenter), 1.0f,
                    kMenuTextColor);
    else
    {
        ui.DrawText(m_resources.cardBoldFont, m_versionLine1, textX, TextY(ui, m_resources.cardBoldFont, logoCenter - 4.5f),
                    1.0f, kMenuTextColor);
        ui.DrawText(m_resources.captionFont, m_versionLine2, textX, TextY(ui, m_resources.captionFont, logoCenter + 4.5f),
                    1.0f, kMenuDimTextColor);
    }
    // A thin stripe in the logo's colors.
    {
        constexpr float kStripeY = 35.0f, kStripeH = 1.2f, kSegment = 14.0f;
        for (int i = 0; i < 5; ++i)
            ui.DrawQuad(kLogoX + kSegment * i, kStripeY, kSegment + (i < 4 ? 0.3f : 0.0f), kStripeH,
                        WithAlpha(TitleLetterColor(i), 0.9f));
    }

    // The pages.
    static constexpr const char *kNames[SidebarCount] = {"Resume", "Library",    "Save states", "Achievements", "Settings",
                                                         "About",  "Close game", "Support",     "Quit"};
    // (Support: a cup of coffee, drawn - see SupportPage::DrawCoffeeCup)
    static constexpr UiIconId kIcons[SidebarCount] = {UiIconId::Resume,   UiIconId::RomList, UiIconId::SaveSlot,
                                                      UiIconId::Trophy,   UiIconId::Settings, UiIconId::Header,
                                                      UiIconId::Back,     UiIconId::None,     UiIconId::Exit};
    const int active = TargetPage() ? TargetPage()->sidebarItem : -1;
    for (int i = 0; i < SidebarCount; ++i)
    {
        if (!SidebarVisible(i))
            continue;
        const float y = SidebarItemY(i);
        const float h = SidebarItemHeight(i) - 4.0f;
        const bool enabled = SidebarEnabled(i);
        const bool cursor = m_sidebarFocus && i == m_sidebarCursor;
        const bool on = i == active;
        if (cursor)
        {
            ui.DrawQuadRounded(5.0f, y - 3.0f, 76.0f, h, kMenuSelectionColor, 5.0f);
            ui.DrawQuadRounded(6.1f, y - 1.9f, 73.8f, h - 2.2f, kMenuSidebarColor, 3.9f);
            ui.DrawQuadRounded(6.1f, y - 1.9f, 73.8f, h - 2.2f, {1.0f, 0.79f, 0.34f, 0.16f}, 3.9f);
        }
        else if (on)
        {
            ui.DrawQuadRounded(5.0f, y - 3.0f, 76.0f, h, kMenuSelectionFillColor, 5.0f);
            ui.DrawQuadRounded(5.0f, y, 2.0f, 10.0f, kMenuSelectionColor, 1.0f);
        }
        else if (i == m_sidebarHover)
            ui.DrawQuadRounded(5.0f, y - 3.0f, 76.0f, h, kMenuHighlightColor, 5.0f);
        const XrColor4f color = (cursor || on) ? kMenuSelectionColor : enabled ? kMenuTextColor : WithAlpha(kMenuDimTextColor, 0.6f);
        if (i == SidebarSupport)
            SupportPage::DrawCoffeeCup(ui, 12.0f, y, kNavIconSize, color);
        else
            m_icons.Draw(ui, kIcons[i], 12.0f, y, kNavIconSize, 1.0f, color);
        const UiFontHandle font = (cursor || on) ? m_resources.bodyBoldFont : m_resources.bodyFont;
        ui.DrawText(font, kNames[i], 27.0f, TextY(ui, font, y + kNavIconSize / 2.0f), 1.0f, color);
        if (i == SidebarResume && enabled)
        {
            // The game it goes back to.
            const std::string game = WithoutRegion(m_resources.emulator->RomName());
            std::string shown = game;
            while (shown.size() > 1 && ui.GetTextWidth(m_resources.captionFont, shown) > 52.0f)
                shown.pop_back();
            if (shown != game)
                shown += "\xE2\x80\xA6";
            ui.DrawText(m_resources.captionFont, shown, 27.0f, TextY(ui, m_resources.captionFont, y + 15.0f), 1.0f,
                        kMenuDimTextColor);
        }
        if (i == m_confirmItem)
            ui.DrawText(m_resources.captionFont, "Press again", 27.0f, TextY(ui, m_resources.captionFont, y + 15.0f), 1.0f,
                        kMenuSelectionColor);
        if (i == SidebarAbout && m_updater.UpdateAvailable())
            ui.DrawText(m_resources.captionFont, "Update available", 27.0f, TextY(ui, m_resources.captionFont, y + 15.0f), 1.0f,
                        kMenuSelectionColor);
    }

    // The clock (and battery).
    std::string status = CurrentTimeString();
    if (m_batteryPercent >= 0 && m_batteryPercent <= 100)
        status += "  \xC2\xB7  Battery " + std::to_string(m_batteryPercent) + "%";
    ui.DrawText(m_resources.captionFont, status, kLogoX, TextY(ui, m_resources.captionFont, kMenuHeight - 10.0f), 1.0f,
                kMenuDimTextColor);
}

void AppMenu::RenderContent(UiRenderer &ui)
{
    ui.DrawQuad(0, 0, kMenuWidth, kMenuHeight, kMenuBodyColor);

    // Draw current page + next page (sliding in/out) - sideways when a page
    // opens another, up/down when the sidebar switches pages.
    // Matches the reference (FrontendGo MenuGo::DrawMenu):
    //   - current page starts at its natural position (offset 0) and slides away
    //   - next page starts displaced by dist and slides in to offset 0
    //   - dist is small so neither page ever leaves the visible area
    if (m_transitionState > 0.0f && m_nextPage)
    {
        const float rawProgress = m_transitionState; // 1.0 -> 0.0
        const float eased = std::sin(rawProgress * (3.14159265f / 2.0f));
        const float dist = kTransitionSlideDistance;
        const int dx = m_transitionVertical ? 0 : m_transitionDir;
        const int dy = m_transitionVertical ? m_transitionDir : 0;
        m_currentPage->Draw(ui, -dx, -dy, 1.0f - eased, dist, rawProgress);
        m_nextPage->Draw(ui, dx, dy, eased, dist, 1.0f - rawProgress);
    }
    else if (m_currentPage)
    {
        m_currentPage->Draw(ui, 0, 0, 0.0f, 0, 1.0f);
    }

    DrawHints(ui);
    DrawSidebar(ui);

    // Where the laser points.
    if (m_pointer.present && m_pointer.drawDot)
    {
        ui.DrawQuadRounded(m_pointer.x - 3.0f, m_pointer.y - 3.0f, 6.0f, 6.0f, {1.0f, 1.0f, 1.0f, 0.25f}, 3.0f);
        ui.DrawQuadRounded(m_pointer.x - 1.6f, m_pointer.y - 1.6f, 3.2f, 3.2f, {1.0f, 1.0f, 1.0f, 1.0f}, 1.6f);
    }
}

void AppMenu::RenderToBuffer(UiRenderer &ui)
{
    if (IsVisible())
    {
        ui.BeginOffscreenFrame(m_offscreenTexture, XrColor4f{0.0f, 0.0f, 0.0f, 0.0f}, kMenuWidth, kMenuHeight);
        RenderContent(ui);
        ui.EndFrame();
    }
    if (NoticeShowing())
        RenderNotice(ui);
}

void AppMenu::Draw(UiRenderer &ui, float x, float y, float squeezeX, float squeezeY)
{
    if (NoticeShowing() && !IsVisible())
        DrawNotice(ui, x, y, squeezeX, squeezeY);
    if (m_visibility <= 0.0f)
        return; // fully closed - nothing to composite

    // Open/close animation: fades in/out while growing in from kMinScale (and
    // shrinking back down on close), eased the same way as the page-slide
    // transition (see StartTransition's doc comment) so both feel consistent.
    constexpr float kMinScale = 0.9f;
    const float eased = std::sin(m_visibility * (3.14159265f / 2.0f));
    const float scale = kMinScale + (1.0f - kMinScale) * eased;

    // 1:1 at scale=1 - m_offscreenTexture is already the full kMenuWidth*
    // m_menuScale physical size, so no scaling happens at composite time
    // beyond the open/close animation above (see Initialize).
    // kPanelCornerRadiusPx itself DOES need scaling here though - this draw
    // call composites onto the real (physical) target, not into the
    // logical-space offscreen buffer.
    const float fullW = kMenuWidth * m_menuScale;
    const float fullH = kMenuHeight * m_menuScale;
    const float w = fullW * scale;
    const float h = fullH * scale;
    ui.DrawImageRounded(m_offscreenTexture, x + (fullW - w) / 2.0f * squeezeX, y + (fullH - h) / 2.0f * squeezeY,
                        w * squeezeX, h * squeezeY, kPanelCornerRadiusPx * m_menuScale * std::min(squeezeX, squeezeY),
                        eased);
    // (a notice of the app's own: on top of the menu, at its bottom)
    if (NoticeShowing())
        DrawNotice(ui, x, y, squeezeX, squeezeY);
}

// -----------------------------------------------------------------------
// Notices over the game (RetroAchievements' - see Achievements::TakeNotice):
// one at a time, at the top of where the menu would be; they wait while the
// menu is open.

void AppMenu::UpdateNotice(float deltaSeconds)
{
    if (!m_ui)
        return;
    // (one of the app's own showing gives way to the next at once - "Saved to slot 3" after "Slot 3")
    if (m_notice.active && m_notice.replaceable && !m_appNotices.empty())
    {
        m_notice.active = false;
        m_noticeQuickIn = true;
    }
    if (!m_notice.active)
    {
        Achievements::Notice notice;
        if (!m_appNotices.empty())
        {
            // The app's own (a newer VBoy Color, a screenshot saved...).
            notice = std::move(m_appNotices.front().notice);
            m_notice.icon = m_appNotices.front().icon;
            m_notice.overMenu = true;
            m_notice.replaceable = m_appNotices.front().replaceable;
            m_appNotices.pop_front();
        }
        else if (!m_resources.emulator || !m_resources.emulator->GetAchievements().TakeNotice(notice))
            return;
        else
        {
            m_notice.icon = UiIconId::None;
            m_notice.overMenu = false;
            m_notice.replaceable = false;
        }
        m_notice.active = true;
        m_notice.notice = std::move(notice);
        m_notice.age = m_noticeQuickIn ? 0.25f : 0.0f; // (replacing one: no slide in again)
        m_noticeQuickIn = false;
        // (glyphs baked now, outside any frame)
        m_ui->EnsureGlyphsForText(m_resources.captionBoldFont, m_notice.notice.caption);
        m_ui->EnsureGlyphsForText(m_resources.bodyBoldFont, m_notice.notice.title + "\xE2\x80\xA6");
        m_ui->EnsureGlyphsForText(m_resources.captionFont, m_notice.notice.text + "\xE2\x80\xA6");
        return;
    }
    if (IsVisible() && !m_notice.overMenu)
        return; // (waits for the game)
    m_notice.age += deltaSeconds;
    if (m_notice.age >= m_notice.notice.seconds)
        m_notice.active = false;
}

void AppMenu::RenderNotice(UiRenderer &ui)
{
    const Achievements::Notice &n = m_notice.notice;
    ui.BeginOffscreenFrame(m_noticeTexture, XrColor4f{0.0f, 0.0f, 0.0f, 0.0f}, kNoticeWidth, kNoticeHeight);
    ui.DrawQuadRounded(0.0f, 0.0f, kNoticeWidth, kNoticeHeight, {0.09f, 0.095f, 0.13f, 0.96f}, 8.0f);
    ui.DrawQuadRounded(0.6f, 0.6f, kNoticeWidth - 1.2f, kNoticeHeight - 1.2f, {1.0f, 0.79f, 0.34f, 0.10f}, 7.4f);
    // The badge (or a trophy while it's on its way).
    constexpr float kBadge = 28.0f, kPad = 6.0f;
    const Achievements *achievements = m_resources.emulator ? &m_resources.emulator->GetAchievements() : nullptr;
    float u0, v0, u1, v1;
    if (m_notice.icon != UiIconId::None)
    {
        ui.DrawQuadRounded(kPad, kPad, kBadge, kBadge, {0.2f, 0.215f, 0.28f, 1.0f}, 4.0f);
        m_icons.Draw(ui, m_notice.icon, kPad + 6.0f, kPad + 6.0f, kBadge - 12.0f, 1.0f, kMenuSelectionColor);
    }
    else if (achievements && achievements->BadgeUv(n.badge, u0, v0, u1, v1))
        ui.DrawImageRegion(achievements->BadgeAtlas(), kPad, kPad, kBadge, kBadge, u0, v0, u1, v1);
    else
    {
        ui.DrawQuadRounded(kPad, kPad, kBadge, kBadge, {0.2f, 0.215f, 0.28f, 1.0f}, 4.0f);
        m_icons.Draw(ui, UiIconId::Trophy, kPad + 7.0f, kPad + 7.0f, kBadge - 14.0f, 1.0f, kMenuSelectionColor);
    }
    const float textX = kPad + kBadge + 7.0f, room = kNoticeWidth - textX - kPad;
    auto fit = [&](UiFontHandle font, const std::string &text)
    {
        if (ui.GetTextWidth(font, text) <= room)
            return text;
        std::string cut = text;
        while (!cut.empty() && ui.GetTextWidth(font, cut + "\xE2\x80\xA6") > room)
        {
            cut.pop_back();
            while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80)
                cut.pop_back();
            if (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0xC0)
                cut.pop_back();
        }
        return cut + "\xE2\x80\xA6";
    };
    ui.DrawText(m_resources.captionBoldFont, fit(m_resources.captionBoldFont, n.caption), textX,
                TextY(ui, m_resources.captionBoldFont, 10.0f), 1.0f, kMenuSelectionColor);
    ui.DrawText(m_resources.bodyBoldFont, fit(m_resources.bodyBoldFont, n.title), textX,
                TextY(ui, m_resources.bodyBoldFont, 20.5f), 1.0f, {1.0f, 1.0f, 1.0f, 1.0f});
    ui.DrawText(m_resources.captionFont, fit(m_resources.captionFont, n.text), textX, TextY(ui, m_resources.captionFont, 31.0f),
                1.0f, kMenuTextColor);
    ui.EndFrame();
}

void AppMenu::DrawNotice(UiRenderer &ui, float x, float y, float squeezeX, float squeezeY)
{
    // Slides down and fades in, fades out at the end.
    const float age = m_notice.age, left = m_notice.notice.seconds - age;
    const float in = std::min(1.0f, age / 0.25f), out = std::min(1.0f, left / 0.4f);
    const float alpha = std::max(0.0f, std::min(in, out));
    const float slide = (1.0f - std::sin(in * 1.5707963f)) * -8.0f;
    const float w = kNoticeWidth * m_menuScale, h = kNoticeHeight * m_menuScale;
    const float nx = x + (kMenuWidth * m_menuScale - w) / 2.0f * squeezeX;
    // (over the game: at the top; over the menu: at its bottom, over the hints)
    const float top = IsVisible() ? kMenuHeight - kNoticeHeight - 6.0f - slide : 6.0f + slide;
    const float ny = y + top * m_menuScale * squeezeY;
    if (IsVisible()) // (over the menu: a solid backing, so the rows under it don't show through)
        ui.DrawQuadRounded(nx, ny, w * squeezeX, h * squeezeY, {0.09f, 0.095f, 0.13f, alpha * m_visibility},
                           8.0f * m_menuScale * std::min(squeezeX, squeezeY));
    ui.DrawImageRounded(m_noticeTexture, nx, ny, w * squeezeX, h * squeezeY, 8.0f * m_menuScale * std::min(squeezeX, squeezeY),
                        alpha);
}

// -----------------------------------------------------------------------
// Hotkeys (the window app - Settings > Hotkeys)

bool AppMenu::HotkeyFor(int key, int mods, HotkeyAction &action) const
{
    const AppSettings *settings = m_resources.settings;
    if (!settings || !settings->hotkeysSet || key < 0 || IsModifierKey(key) || WantsText())
        return false;
    if (m_open && TargetPage() && TargetPage()->Capturing())
        return false; // (a key being bound)
    // The painting tools have their keys to themselves while they're on
    // (Shift+F10 and Shift+F4 are theirs too).
    if (settings->paintingTools && IsPaintingToolKey(key) && (mods & ~kHotkeyShift) == 0)
        return false;
    for (int i = 0; i < kHotkeyActionCount; ++i)
        if (settings->hotkeys[i].key == key && settings->hotkeys[i].mods == mods)
        {
            action = static_cast<HotkeyAction>(i);
            return true;
        }
    return false;
}

bool AppMenu::RunHotkeyAction(HotkeyAction action)
{
    Emulator *emulator = m_resources.emulator;
    const bool game = emulator && emulator->HasGame();
    const std::string gameName = game ? WithoutRegion(emulator->RomName()) : "";
    const int slot = m_saveStatesPage.Slot();
    switch (action)
    {
    case HotkeyAction::Screenshot:
        TakeScreenshot();
        return true;
    case HotkeyAction::SaveState:
        if (game)
        {
            const bool saved = emulator->SaveState(slot);
            m_saveStatesPage.RefreshSavePreview();
            ShowNotice("Save state", saved ? "Saved to slot " + std::to_string(slot) : "Couldn't save to slot " + std::to_string(slot),
                       gameName, UiIconId::Save);
        }
        return true;
    case HotkeyAction::LoadState:
        if (game)
        {
            if (!emulator->SaveStateExists(slot))
                ShowNotice("Save state", "Slot " + std::to_string(slot) + " is empty", gameName, UiIconId::Load);
            else if (emulator->LoadState(slot))
            {
                Hide();
                ShowNotice("Save state", "Loaded slot " + std::to_string(slot), gameName, UiIconId::Load);
            }
            else
                ShowNotice("Save state", "Couldn't load slot " + std::to_string(slot), gameName, UiIconId::Load);
        }
        return true;
    case HotkeyAction::PreviousSlot:
    case HotkeyAction::NextSlot:
    {
        m_saveStatesPage.ChangeSaveSlot(action == HotkeyAction::NextSlot ? 1 : -1);
        const int now = m_saveStatesPage.Slot();
        ShowNotice("Save state", "Slot " + std::to_string(now),
                   !game ? "" : emulator->SaveStateExists(now) ? "Saved - Load state loads it" : "Empty",
                   UiIconId::SaveSlot, 1.5f);
        return true;
    }
    case HotkeyAction::Reset:
        if (game && emulator->ResetGame())
        {
            Hide();
            ShowNotice("Reset", gameName, "From the start", UiIconId::Reset, 1.5f);
        }
        return true;
    case HotkeyAction::CloseGame:
        if (game)
            CloseGame();
        return true;
    case HotkeyAction::Quit:
        Quit();
        return true;
    case HotkeyAction::Menu:
    case HotkeyAction::Fullscreen:
        break;
    }
    return false;
}

void AppMenu::SubmitRawKey(int key, int mods)
{
    if (!m_open || m_sidebarFocus || m_transitionState > 0.0f)
        return;
    if (m_currentPage == &m_hotkeysPage)
        m_hotkeysPage.SubmitKey(key, mods);
    else if (m_currentPage == &m_supportPage && mods == 0)
        m_supportPage.SubmitKey(key);
}

void AppMenu::TakeScreenshot()
{
    Emulator *emulator = m_resources.emulator;
    if (!emulator || !emulator->HasGame())
        return;
    const AppSettings *settings = m_resources.settings;
    const std::string name = emulator->SaveScreenshot(settings ? settings->ScreenTint() : XrColor4f{1.0f, 1.0f, 1.0f, 1.0f},
                                                      settings ? settings->ScreenPattern() : -1);
    if (name.empty())
        ShowNotice("Screenshot", "Couldn't save it", "Settings > Folders > Screenshots: can it be written?",
                   UiIconId::FlatScreen, 3.5f);
    else
        ShowNotice("Screenshot", "Saved", name, UiIconId::FlatScreen);
}

void AppMenu::ShowNotice(const std::string &caption, const std::string &title, const std::string &text, UiIconId icon,
                         float seconds)
{
    Achievements::Notice notice;
    notice.caption = caption;
    notice.title = title;
    notice.text = text;
    notice.seconds = seconds;
    // (only the newest of these waits - pressing a key again replaces it)
    m_appNotices.erase(std::remove_if(m_appNotices.begin(), m_appNotices.end(), [](const AppNotice &n) { return n.replaceable; }),
                       m_appNotices.end());
    m_appNotices.push_back({std::move(notice), icon == UiIconId::None ? UiIconId::Header : icon, true});
}

// -----------------------------------------------------------------------
// Typing on a real keyboard

bool AppMenu::WantsText() const
{
    return m_open && !m_sidebarFocus && TargetPage() == &m_textInputPage;
}

void AppMenu::TypeText(const std::string &utf8)
{
    if (WantsText())
        m_textInputPage.TypeText(utf8);
}

void AppMenu::TypeBackspace()
{
    if (WantsText())
        m_textInputPage.TypeBackspace();
}

void AppMenu::TypeEnter()
{
    if (WantsText())
        m_textInputPage.TypeEnter();
}

void AppMenu::TypePaste()
{
    if (WantsText())
        m_textInputPage.Paste();
}

void AppMenu::TypeCopy()
{
    if (WantsText())
        m_textInputPage.Copy();
}
