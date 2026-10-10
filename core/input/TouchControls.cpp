#include "input/TouchControls.h"
#include "emu/Emulator.h" // VBButtonBit
#include "io/Platform.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace
{
    constexpr float kVbWidth = 384.0f, kVbHeight = 224.0f;

    constexpr uint32_t Bit(uint32_t vbBit) { return 1u << vbBit; }

    const XrColor4f kFill{1.0f, 1.0f, 1.0f, 0.13f};
    const XrColor4f kFillDark{0.0f, 0.0f, 0.0f, 0.22f};
    const XrColor4f kPressed{0.97f, 0.77f, 0.27f, 0.60f};
    const XrColor4f kLabel{1.0f, 1.0f, 1.0f, 0.78f};

    bool InCircle(float cx, float cy, float r, float x, float y) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; }

    float TextY(UiRenderer &ui, UiFontHandle font, float centerY)
    {
        return centerY - ui.GetFontPHeight(font) / 2.0f - ui.GetFontPStart(font);
    }
} // namespace

void TouchControls::Initialize(UiRenderer &ui, Platform &platform)
{
    m_ui = &ui;
    m_fontBytes = platform.LoadAssetBytes("fonts/Roboto-Bold.ttf");
    if (m_fontBytes.empty())
        return;
    // (sizes for a typical phone - Layout rebakes them for the real density)
    m_font = ui.LoadFont(m_fontBytes, 52);
    m_smallFont = ui.LoadFont(m_fontBytes, 28);
    m_fontDp = 2.625f;
    ui.EnsureGlyphsForText(m_font, "ABLR");
    ui.EnsureGlyphsForText(m_smallFont, "SELECTSTART");
}

bool TouchControls::UsesRightDpad(const std::string &romName)
{
    std::string name = romName;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char *game : {"red alarm", "teleroboxer", "3-d tetris", "3d tetris", "golf"})
        if (name.find(game) != std::string::npos)
            return true;
    return false;
}

void TouchControls::Layout(float w, float h, float dp, bool rightDpad)
{
    m_w = w;
    m_h = h;
    m_dp = dp;
    m_rightDpad = rightDpad;
    if (m_ui && m_font.IsValid() && std::fabs(dp - m_fontDp) > 0.01f)
    {
        // (labels as sharp as the screen's density allows)
        m_fontDp = dp;
        m_ui->RebakeFont(m_font, m_fontBytes, static_cast<int>(std::lround(20.0f * dp)));
        m_ui->RebakeFont(m_smallFont, m_fontBytes, static_cast<int>(std::lround(10.5f * dp)));
        m_ui->EnsureGlyphsForText(m_font, "ABLR");
        m_ui->EnsureGlyphsForText(m_smallFont, "SELECTSTART");
    }

    const float pad = 132.0f * dp; // a D-pad's size
    const float margin = 12.0f * dp;
    const float shoulderW = 88.0f * dp, shoulderH = 36.0f * dp;
    const float pillW = 60.0f * dp, pillH = 28.0f * dp;
    const float buttonR = 29.0f * dp;

    if (h > w)
    {
        // Portrait: the screen on top, the controller below it.
        const float s = w / kVbWidth;
        m_screenW = w;
        m_screenH = kVbHeight * s;
        m_screenX = 0.0f;
        m_screenY = std::max(24.0f * dp, std::min(h * 0.08f, h - m_screenH - pad - 120.0f * dp));
        const float top = m_screenY + m_screenH + margin;
        const float lx = w * 0.27f, rx = w * 0.73f;
        m_l = {margin, top, shoulderW, shoulderH};
        m_r = {w - margin - shoulderW, top, shoulderW, shoulderH};
        const float cy = std::min(top + shoulderH + margin + pad / 2.0f + 8.0f * dp, h - pad / 2.0f - pillH - 3 * margin);
        m_leftPad = {lx, cy, pad / 2.0f};
        m_select = {lx - pillW - 6.0f * dp, cy + pad / 2.0f + margin, pillW, pillH};
        m_start = {lx + 6.0f * dp, cy + pad / 2.0f + margin, pillW, pillH};
        m_rightPad = {rx, cy - (rightDpad ? 30.0f * dp : 0.0f), pad / 2.0f * 0.85f};
        const float by = rightDpad ? m_rightPad.y + m_rightPad.r + buttonR + 10.0f * dp : cy;
        m_b = {rx - buttonR - 8.0f * dp, by + 14.0f * dp, buttonR};
        m_a = {rx + buttonR + 8.0f * dp, by - 14.0f * dp, buttonR};
        m_menu = {w / 2.0f, top + shoulderH / 2.0f, 17.0f * dp};
        return;
    }

    // Landscape: the screen between the two sides, as big as fits - or, on a
    // screen too square for that (a tablet), as big as the whole screen with
    // the controls over its sides.
    const float side = pad + 28.0f * dp;
    float s = std::min((w - 2.0f * side) / kVbWidth, h / kVbHeight);
    const bool overlay = s * kVbHeight < 0.72f * h;
    if (overlay)
        s = std::min(w / kVbWidth, h / kVbHeight);
    m_screenW = std::floor(kVbWidth * s);
    m_screenH = std::floor(kVbHeight * s);
    m_screenX = std::floor((w - m_screenW) / 2.0f);
    m_screenY = std::floor((h - m_screenH) / 2.0f);

    const float lx = overlay ? side / 2.0f : std::max(side / 2.0f, m_screenX / 2.0f);
    const float rx = w - lx;
    m_l = {lx - shoulderW / 2.0f, margin, shoulderW, shoulderH};
    m_r = {rx - shoulderW / 2.0f, margin, shoulderW, shoulderH};

    // Left: the D-pad, Select and Start below it.
    const float below = pillH + 2.0f * margin;
    const float topRoom = margin + shoulderH + margin;
    const float ly = std::clamp(h * 0.50f, topRoom + pad / 2.0f, h - below - pad / 2.0f);
    m_leftPad = {lx, ly, pad / 2.0f};
    const float pillY = std::min(ly + pad / 2.0f + margin, h - pillH - margin);
    m_select = {lx - pillW - 5.0f * dp, pillY, pillW, pillH};
    m_start = {lx + 5.0f * dp, pillY, pillW, pillH};

    // Right: the D-pad (the games that use it), B and A below it.
    const float smallPad = pad * 0.9f;
    m_rightPad = {rx, topRoom + smallPad / 2.0f, smallPad / 2.0f};
    float by = rightDpad ? std::min(m_rightPad.y + m_rightPad.r + buttonR + 16.0f * dp, h - buttonR - margin - 14.0f * dp)
                         : std::clamp(h * 0.55f, topRoom + buttonR + 14.0f * dp, h - buttonR - margin - 14.0f * dp);
    m_b = {rx - buttonR - 7.0f * dp, by + 14.0f * dp, buttonR};
    m_a = {rx + buttonR + 7.0f * dp, by - 14.0f * dp, buttonR};

    // The menu: small, at the top in the middle.
    m_menu = {w / 2.0f, margin + 15.0f * dp, 16.0f * dp};
}

void TouchControls::ScreenArea(float &x, float &y, float &w, float &h) const
{
    if (m_visible)
    {
        x = m_screenX;
        y = m_screenY;
        w = m_screenW;
        h = m_screenH;
        return;
    }
    const float s = std::min(m_w / kVbWidth, m_h / kVbHeight);
    w = std::floor(kVbWidth * s);
    h = std::floor(kVbHeight * s);
    x = std::floor((m_w - w) / 2.0f);
    y = std::floor((m_h - h) / 2.0f);
}

uint32_t TouchControls::DpadBits(const Circle &pad, float x, float y, uint32_t up, uint32_t down, uint32_t left,
                                 uint32_t right) const
{
    const float dx = x - pad.x, dy = y - pad.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (dist > pad.r * 1.2f || dist < pad.r * 0.16f)
        return 0;
    // 8 directions, 45 degrees each - the diagonals press both.
    const float angle = std::atan2(dy, dx); // 0 right, +pi/2 down
    const int sector = static_cast<int>(std::lround(angle / (3.14159265f / 4.0f))) & 7;
    static constexpr int kDirs[8] = {0b0001, 0b0101, 0b0100, 0b0110, 0b0010, 0b1010, 0b1000, 0b1001}; // right/left/down/up bits
    const int d = kDirs[sector];
    return ((d & 0b0001) ? right : 0) | ((d & 0b0010) ? left : 0) | ((d & 0b0100) ? down : 0) | ((d & 0b1000) ? up : 0);
}

uint32_t TouchControls::Update(const std::vector<Touch> &touches, bool &menuPressed)
{
    menuPressed = false;
    m_lastPressed = m_pressed;
    m_pressed = 0;
    if (!m_visible)
    {
        m_menuDown = false;
        return 0;
    }
    using namespace VBButtonBit;
    const float slack = 10.0f * m_dp;
    auto inRect = [slack](const Rect &r, float x, float y)
    { return x >= r.x - slack && x <= r.x + r.w + slack && y >= r.y - slack && y <= r.y + r.h + slack; };
    bool menuDown = false;
    for (const Touch &t : touches)
    {
        m_pressed |= DpadBits(m_leftPad, t.x, t.y, Bit(LeftUp), Bit(LeftDown), Bit(LeftLeft), Bit(LeftRight));
        if (m_rightDpad)
            m_pressed |= DpadBits(m_rightPad, t.x, t.y, Bit(RightUp), Bit(RightDown), Bit(RightLeft), Bit(RightRight));
        if (InCircle(m_a.x, m_a.y, m_a.r * 1.3f, t.x, t.y))
            m_pressed |= Bit(A);
        if (InCircle(m_b.x, m_b.y, m_b.r * 1.3f, t.x, t.y))
            m_pressed |= Bit(B);
        if (inRect(m_l, t.x, t.y))
            m_pressed |= Bit(L);
        if (inRect(m_r, t.x, t.y))
            m_pressed |= Bit(R);
        if (inRect(m_select, t.x, t.y))
            m_pressed |= Bit(Select);
        if (inRect(m_start, t.x, t.y))
            m_pressed |= Bit(Start);
        if (InCircle(m_menu.x, m_menu.y, m_menu.r * 1.8f, t.x, t.y))
            menuDown = true;
    }
    menuPressed = menuDown && !m_menuDown;
    m_menuDown = menuDown;
    return m_pressed;
}

void TouchControls::DrawDpad(UiRenderer &ui, const Circle &pad, uint32_t up, uint32_t down, uint32_t left,
                             uint32_t right) const
{
    const float r = pad.r, arm = r * 0.72f, round = arm * 0.22f;
    // A cross: the arms, and the middle once (not twice - it's see-through).
    ui.DrawQuadRounded(pad.x - r, pad.y - arm / 2.0f, r - arm / 2.0f, arm, kFill, round);
    ui.DrawQuadRounded(pad.x + arm / 2.0f, pad.y - arm / 2.0f, r - arm / 2.0f, arm, kFill, round);
    ui.DrawQuadRounded(pad.x - arm / 2.0f, pad.y - r, arm, 2.0f * r, kFill, round);
    ui.DrawQuadRounded(pad.x - arm * 0.22f, pad.y - arm * 0.22f, arm * 0.44f, arm * 0.44f, kFillDark, arm * 0.22f);
    const float inset = arm * 0.12f, len = r - arm / 2.0f - inset;
    if (m_pressed & up)
        ui.DrawQuadRounded(pad.x - arm / 2.0f + inset, pad.y - r + inset, arm - 2 * inset, len, kPressed, round);
    if (m_pressed & down)
        ui.DrawQuadRounded(pad.x - arm / 2.0f + inset, pad.y + arm / 2.0f, arm - 2 * inset, len, kPressed, round);
    if (m_pressed & left)
        ui.DrawQuadRounded(pad.x - r + inset, pad.y - arm / 2.0f + inset, len, arm - 2 * inset, kPressed, round);
    if (m_pressed & right)
        ui.DrawQuadRounded(pad.x + arm / 2.0f, pad.y - arm / 2.0f + inset, len, arm - 2 * inset, kPressed, round);
}

void TouchControls::DrawButton(UiRenderer &ui, const Circle &c, const char *label, bool pressed, UiFontHandle font) const
{
    ui.DrawQuadRounded(c.x - c.r, c.y - c.r, 2.0f * c.r, 2.0f * c.r, pressed ? kPressed : kFill, c.r);
    if (font.IsValid())
        ui.DrawText(font, label, c.x - ui.GetTextWidth(font, label) / 2.0f, TextY(ui, font, c.y), 1.0f, kLabel);
}

void TouchControls::DrawPill(UiRenderer &ui, const Rect &r, const char *label, bool pressed, UiFontHandle font) const
{
    ui.DrawQuadRounded(r.x, r.y, r.w, r.h, pressed ? kPressed : kFill, r.h / 2.0f);
    if (font.IsValid())
        ui.DrawText(font, label, r.x + (r.w - ui.GetTextWidth(font, label)) / 2.0f, TextY(ui, font, r.y + r.h / 2.0f), 1.0f,
                    kLabel);
}

void TouchControls::Draw(UiRenderer &ui) const
{
    if (!m_visible)
        return;
    using namespace VBButtonBit;
    DrawDpad(ui, m_leftPad, Bit(LeftUp), Bit(LeftDown), Bit(LeftLeft), Bit(LeftRight));
    if (m_rightDpad)
        DrawDpad(ui, m_rightPad, Bit(RightUp), Bit(RightDown), Bit(RightLeft), Bit(RightRight));
    DrawButton(ui, m_a, "A", m_pressed & Bit(A), m_font);
    DrawButton(ui, m_b, "B", m_pressed & Bit(B), m_font);
    DrawPill(ui, m_l, "L", m_pressed & Bit(L), m_font);
    DrawPill(ui, m_r, "R", m_pressed & Bit(R), m_font);
    DrawPill(ui, m_select, "SELECT", m_pressed & Bit(Select), m_smallFont);
    DrawPill(ui, m_start, "START", m_pressed & Bit(Start), m_smallFont);
    // The menu button: three bars.
    ui.DrawQuadRounded(m_menu.x - m_menu.r, m_menu.y - m_menu.r, 2 * m_menu.r, 2 * m_menu.r, m_menuDown ? kPressed : kFill,
                       m_menu.r);
    const float bw = m_menu.r * 0.9f, bh = std::max(2.0f, m_menu.r * 0.14f);
    for (int i = -1; i <= 1; ++i)
        ui.DrawQuadRounded(m_menu.x - bw / 2.0f, m_menu.y + i * m_menu.r * 0.34f - bh / 2.0f, bw, bh, kLabel, bh / 2.0f);
}
