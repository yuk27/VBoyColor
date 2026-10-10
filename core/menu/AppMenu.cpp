#include "menu/AppMenu.h"
#include "emu/Emulator.h"
#include "io/Platform.h"
#include "io/Settings.h"

#include <stb_image.h> // (implementation in VulkanRenderer.cpp)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace
{
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
    constexpr float kNavPitch = 20.0f;
    constexpr float kNavSubtitleExtra = 7.0f; // (Resume shows the game under it)
    constexpr float kNavIconSize = 10.0f;

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

    // The logo, decoded once (RGBA) and resampled per menu scale.
    {
        const std::vector<uint8_t> png = platform.LoadAssetBytes("logo/vboycolor_icon.png");
        int w = 0, h = 0, channels = 0;
        if (stbi_uc *pixels = png.empty() ? nullptr : stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &channels, 4))
        {
            m_logo.pixels.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
            m_logo.width = static_cast<uint32_t>(w);
            m_logo.height = static_cast<uint32_t>(h);
            stbi_image_free(pixels);
            m_logo.texHeight = static_cast<uint32_t>(std::ceil(kLogoHeight * kLogoMaxScale));
            m_logo.texWidth = static_cast<uint32_t>(std::ceil(m_logo.texHeight * static_cast<float>(w) / h)) + 1;
            m_logo.texture = ui.CreateStreamingImage(m_logo.texWidth, m_logo.texHeight, VK_FORMAT_B8G8R8A8_SRGB);
            RebuildLogo(ui);
        }
    }

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
    m_resources.buttonMappingProfile = mappingProfile;
    m_resources.passthroughSupported = passthroughSupported;
    m_thumbnails.Init(ui, platform, emulator, settings);
    m_resources.thumbnails = &m_thumbnails;
    // Physical pixel size - kMenuWidth/kMenuHeight are logical units (see
    // AppMenuLayout.h); RenderToBuffer maps them onto this full-resolution
    // texture via BeginOffscreenFrame's logicalWidth/logicalHeight, so the
    // menu rasterizes crisp at m_menuScale regardless of how small the
    // logical layout numbers are.
    m_offscreenTexture = ui.CreateRenderTexture(static_cast<uint32_t>(kMenuWidth * m_menuScale),
                                                static_cast<uint32_t>(kMenuHeight * m_menuScale), targetFormat);

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
    LoadFonts(ui, true);
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
    MenuPage *pages[] = {&m_libraryPage, &m_saveStatesPage, &m_settingsPage, &m_emulatorButtonMapPage, &m_moveScreenPage,
                         &m_aboutPage};
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
    m_aboutPage.sidebarItem = SidebarAbout;

    // Cross-page links
    m_settingsPage.emulatorButtonMapPage = &m_emulatorButtonMapPage;
    m_settingsPage.moveScreenPage = &m_moveScreenPage;
    m_emulatorButtonMapPage.settingsPage = &m_settingsPage;
    m_moveScreenPage.settingsPage = &m_settingsPage;

    for (MenuPage *page : pages)
    {
        page->Init(ui, m_resources);
        page->SetLeftEdge([this]() { FocusSidebar(); });
    }
    // The sidebar's own pages: B goes back to the game (or the sidebar).
    for (MenuPage *page : {static_cast<MenuPage *>(&m_libraryPage), static_cast<MenuPage *>(&m_saveStatesPage),
                           static_cast<MenuPage *>(&m_settingsPage), static_cast<MenuPage *>(&m_aboutPage)})
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
    case SidebarSettings:
        return &m_settingsPage;
    case SidebarAbout:
        return &m_aboutPage;
    default:
        return nullptr;
    }
}

bool AppMenu::SidebarEnabled(int item) const
{
    if (item == SidebarResume || item == SidebarSaveStates)
        return m_resources.emulator && m_resources.emulator->HasGame();
    return item >= 0 && item < SidebarCount;
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
    MenuPage *page = PageFor(m_sidebarCursor);
    if (!page)
        return;
    if (TargetPage()->sidebarItem != m_sidebarCursor)
        GoTo(page, m_sidebarCursor > TargetPage()->sidebarItem ? 1 : -1);
    if (TargetPage()->HasFocusable())
        FocusContent();
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
    return kNavPitch + (item == SidebarResume && SidebarEnabled(SidebarResume) ? kNavSubtitleExtra : 0.0f);
}

float AppMenu::SidebarItemY(int item) const
{
    float y = kNavTop;
    for (int i = 0; i < item; ++i)
        y += SidebarItemHeight(i);
    return y;
}

void AppMenu::HandlePointer()
{
    Pointer &p = m_pointer;
    // (just arrived - e.g. the app starting under a resting mouse - isn't a move)
    if (p.present && p.lastX < 0.0f)
    {
        p.lastX = p.x;
        p.lastY = p.y;
    }
    bool moved = p.present && (std::abs(p.x - p.lastX) > 0.01f || std::abs(p.y - p.lastY) > 0.01f);
    bool clicked = p.present && p.down && !p.wasDown;
    float dragY = 0.0f, flingY = 0.0f;
    if (p.dragToScroll)
    {
        // A laser: a press on the menu starts a gesture; let go before it
        // moved far (about a degree and a half - a trigger pull shakes the
        // laser a little) it's a click, where it was let go; moved further
        // while held, a drag - never a click.
        constexpr float kDragStart = 8.0f; // menu units
        clicked = false;
        if (p.present && p.down && !p.wasDown)
        {
            p.pressed = true;
            p.dragging = false;
            p.pressX = p.x;
            p.pressY = p.y;
            p.velocityY = 0.0f;
        }
        if (p.pressed && !p.present)
            p.pressed = p.dragging = false; // (the laser left the menu: cancelled)
        if (p.pressed && p.down)
        {
            if (!p.dragging && std::hypot(p.x - p.pressX, p.y - p.pressY) > kDragStart)
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
            else
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
    if ((moved || clicked) && m_sidebarFocus && page->HasFocusable())
        FocusContent();
    MenuPointer pointer;
    pointer.x = p.x;
    pointer.y = p.y;
    pointer.moved = moved;
    pointer.clicked = clicked;
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
    m_thumbnails.Update(m_open);

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

    HandlePointer();

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
    const bool topLevel = page == &m_libraryPage || page == &m_saveStatesPage || page == &m_settingsPage || page == &m_aboutPage;
    for (MenuHint &hint : hints)
        if (hint.icon == UiIconId::ButtonB && topLevel && game)
            hint.label = "Resume";
    return hints;
}

void AppMenu::DrawHints(UiRenderer &ui)
{
    const float lineY = kMenuHeight - kHintsHeight;
    ui.DrawQuad(kContentX - 2.0f, lineY, kContentRight - kContentX + 4.0f, 0.6f, kMenuLineColor);
    constexpr float kIcon = 8.0f, kIconGap = 2.5f, kGroupGap = 9.0f;
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
        ui.DrawText(m_resources.bodyBoldFont, "VBoy Color", kLogoX, TextY(ui, m_resources.bodyBoldFont, kLogoY + 5.0f), 1.0f,
                    kMenuTextColor);
    const float logoCenter = kLogoY + kLogoHeight / 2.0f;
    if (m_versionLine2.empty())
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
    static constexpr const char *kNames[SidebarCount] = {"Library", "Resume", "Save states", "Settings", "About"};
    static constexpr UiIconId kIcons[SidebarCount] = {UiIconId::RomList, UiIconId::Resume, UiIconId::SaveSlot,
                                                      UiIconId::Settings, UiIconId::Header};
    const int active = TargetPage() ? TargetPage()->sidebarItem : -1;
    for (int i = 0; i < SidebarCount; ++i)
    {
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
    ui.BeginOffscreenFrame(m_offscreenTexture, XrColor4f{0.0f, 0.0f, 0.0f, 0.0f}, kMenuWidth, kMenuHeight);
    RenderContent(ui);
    ui.EndFrame();
}

void AppMenu::Draw(UiRenderer &ui, float x, float y, float squeezeX, float squeezeY)
{
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
}
