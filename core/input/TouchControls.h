#pragma once

#include "gfx/UiRenderer.h"

#include <cstdint>
#include <string>
#include <vector>

class Platform;

// The Virtual Boy controller on a touch screen (the phone app): laid out
// like the real one - the left D-pad with Select and Start below it, the
// right D-pad with B and A below it, L and R on top - with the game screen
// between the two sides. The right D-pad shows only in the games that use
// it (UsesRightDpad). A small button at the top opens the menu.
//
// Touches are read fresh every frame (Update), so a finger can slide from
// one button to another, and several fingers press several buttons.
// Hidden while a gamepad is in use (the phone app shows them again on the
// next touch).
class TouchControls
{
public:
    struct Touch
    {
        int id = 0;
        float x = 0, y = 0; // screen pixels
    };

    // Loads the font the labels are drawn with.
    void Initialize(UiRenderer &ui, Platform &platform);

    // Lays the controls out for a w x h screen (pixels, landscape or not),
    // pxPerDp: pixels per density-independent pixel (Android's dp - about
    // 1/160 inch); rightDpad: the game uses the right D-pad.
    void Layout(float w, float h, float pxPerDp, bool rightDpad);
    // Where the game screen goes: the space the controls leave (all of it
    // when they're hidden).
    void ScreenArea(float &x, float &y, float &w, float &h) const;

    // The Virtual Boy buttons the touches hold (bits of 1 << VBButtonBit::*),
    // and whether a touch just landed on the menu button.
    uint32_t Update(const std::vector<Touch> &touches, bool &menuPressed);
    // Buttons pressed this frame that weren't the last (for a tap of
    // haptic feedback).
    uint32_t NewlyPressed() const { return m_pressed & ~m_lastPressed; }

    void Draw(UiRenderer &ui) const;

    void SetVisible(bool visible) { m_visible = visible; }
    bool Visible() const { return m_visible; }

    // The games that use the right D-pad (by name): Red Alarm, Teleroboxer,
    // 3-D Tetris, Golf (and T&E Virtual Golf).
    static bool UsesRightDpad(const std::string &romName);

private:
    struct Circle
    {
        float x = 0, y = 0, r = 0;
    };
    struct Rect
    {
        float x = 0, y = 0, w = 0, h = 0;
    };
    // Which direction bits a touch at (x, y) presses on a D-pad, 0 if it's
    // not on it.
    uint32_t DpadBits(const Circle &pad, float x, float y, uint32_t up, uint32_t down, uint32_t left, uint32_t right) const;
    void DrawDpad(UiRenderer &ui, const Circle &pad, uint32_t up, uint32_t down, uint32_t left, uint32_t right) const;
    void DrawButton(UiRenderer &ui, const Circle &c, const char *label, bool pressed, UiFontHandle font) const;
    void DrawPill(UiRenderer &ui, const Rect &r, const char *label, bool pressed, UiFontHandle font) const;

    UiRenderer *m_ui = nullptr;
    std::vector<uint8_t> m_fontBytes;
    float m_fontDp = 0.0f; // the density the fonts are baked for
    UiFontHandle m_font, m_smallFont;
    bool m_visible = true;
    bool m_rightDpad = false;
    float m_dp = 1.0f;
    float m_w = 0, m_h = 0;
    float m_screenX = 0, m_screenY = 0, m_screenW = 0, m_screenH = 0;
    Circle m_leftPad, m_rightPad, m_a, m_b, m_menu;
    Rect m_l, m_r, m_select, m_start;
    uint32_t m_pressed = 0, m_lastPressed = 0;
    bool m_menuDown = false;
};
