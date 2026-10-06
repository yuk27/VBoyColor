#pragma once

#include "Version.h" // kGeneratedVersionString - build-generated, see cmake/GenerateVersion.cmake

#include <openxr/openxr.h> // XrColor4f

// Layout constants shared across all menu pages, mirroring AppMenu's static
// members so page .cpp files don't need the full AppMenu header.
//
// All values are logical units, not physical pixels. AppMenu renders into an
// offscreen texture sized kMenuWidth*kMenuScale x kMenuHeight*kMenuScale but
// maps this logical 384x240 space across the whole physical viewport (see
// UiRenderer::BeginOffscreenFrame), so a platform can raise the scale for a
// sharper VR panel without touching any layout math below.
//
// The menu: a sidebar on the left (the logo, the pages, the clock - drawn by
// AppMenu) and the current page right of it: its title (and a subtitle) on
// top, its content below, the button hints along the bottom.
inline constexpr int kMenuWidth = 384;
inline constexpr int kMenuHeight = 240;
inline constexpr float kMenuScale = 2.0f;

inline constexpr float kSidebarWidth = 86.0f;
inline constexpr float kContentX = 96.0f;
inline constexpr float kContentRight = kMenuWidth - 6.0f;
inline constexpr float kContentWidth = kContentRight - kContentX;
inline constexpr float kPageTitleY = 8.0f;     // the page title's line (kTitleFontSize)
inline constexpr float kPageTitleHeight = 18.0f;
inline constexpr float kContentTop = 32.0f;
inline constexpr float kHintsHeight = 17.0f; // the bottom row of button hints
inline constexpr float kContentBottom = kMenuHeight - kHintsHeight - 2.0f;
inline constexpr float kContentHeight = kContentBottom - kContentTop;

// Settings-style lists (MenuList): rows on rounded cards, grouped under
// small headers.
inline constexpr float kRowHeight = 17.0f;
inline constexpr float kGroupHeaderHeight = 12.0f;
inline constexpr float kGroupGap = 6.0f;
inline constexpr float kCardRadius = 6.0f;

// Buffer-space slide distance for the page transition (see AppMenu::RenderContent).
inline constexpr float kTransitionSlideDistance = 24.0f;

// Font sizes (logical units). Title: the page's name; body: list rows and
// the sidebar; card: game names in the library; caption: small print
// (regions, group headers, hints, the clock).
inline constexpr float kTitleFontSize = 14.0f;
inline constexpr float kBodyFontSize = 8.5f;
inline constexpr float kCardFontSize = 7.5f;
inline constexpr float kCaptionFontSize = 6.5f;
inline constexpr int kSmallFontSize = 8;

// VBoy Color's theme: deep blue-gray panels, light text, what's selected in
// amber.
inline constexpr XrColor4f kMenuTextColor = {0.84f, 0.86f, 0.92f, 1.0f};
inline constexpr XrColor4f kMenuDimTextColor = {0.55f, 0.58f, 0.66f, 1.0f};
inline constexpr XrColor4f kMenuSelectionColor = {1.0f, 0.79f, 0.34f, 1.0f};
inline constexpr XrColor4f kMenuHighlightColor = {0.75f, 0.8f, 1.0f, 0.09f};
inline constexpr XrColor4f kMenuSelectionFillColor = {1.0f, 0.79f, 0.34f, 0.12f};

inline constexpr XrColor4f kMenuBodyColor = {0.07f, 0.075f, 0.105f, 1.0f};
inline constexpr XrColor4f kMenuSidebarColor = {0.11f, 0.118f, 0.16f, 1.0f};
inline constexpr XrColor4f kMenuCardColor = {0.135f, 0.145f, 0.195f, 1.0f};
inline constexpr XrColor4f kMenuLineColor = {0.2f, 0.215f, 0.28f, 1.0f};
// (the old header/bottom bar color - still the save preview's frame)
inline constexpr XrColor4f kMenuOverlayColor = {0.13f, 0.14f, 0.19f, 0.985f};

inline constexpr XrColor4f kMenuVersionColor = kMenuDimTextColor;
inline constexpr const char *kVersionString = kGeneratedVersionString;
