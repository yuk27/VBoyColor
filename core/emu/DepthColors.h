#pragma once

#include <array>
#include <cstdint>
#include <vector>

// Auto colors for what no tile drew: pixels the game's CPU wrote straight
// into the frame buffer (Red Alarm's wireframe world and its whole screen,
// Bound High's playfield, Virtual Lab's title and pause screens). The tile
// tracker knows nothing about them, so the layer and sprite ramps of
// AutoColors.h can't tell them apart - but the two eyes still say how far
// away each thing is: the right picture shows it shifted from the left one
// by its disparity (right x - left x: negative in front of the screen,
// positive behind it). So they're colored by depth - far indigo, violet,
// rose, orange, near gold - each shade its own step of that color.
//
// The disparity of the left picture's pixels:
//  1. per 8x8 block, how well the two pictures line up (the same shade at
//     the same spot) at each disparity - in a 24x24 window around the block
//     and an 8-row strip 88 wide (text and tile patterns repeat; the wide
//     strip sees past a repetition);
//  2. smoothed over the block grid (semi-global matching: neighbouring
//     blocks mostly lie at the same depth, but depth may jump at an edge),
//     with a pull towards the frame's few main depths (most things a game
//     draws sit on a handful of planes) and towards the last frame's;
//  3. per pixel: of the disparities of its block and the 8 around it and
//     the main depths, the one that lines up best in a 7x7 window around it,
//     counting its own shade only (lines of different depths crossing in a
//     block; a score in one shade in front of a block field in others) - and
//     a pixel that hasn't changed keeps its disparity while it still fits as
//     well;
//  4. inside an area of one shade, where any shift lines up, the disparities
//     at the area's edges, blended across it.
// The right picture's pixels take the disparity of the left pixel they show
// (the same shade where that disparity puts it), so both eyes always show
// the same colors; the few it doesn't show take their row's neighbours'.
//
// Nothing is worked out while both pictures stay the same (most 3D games
// draw a new picture every other frame or less).
class DepthColors
{
public:
    static constexpr int kWidth = 384, kHeight = 224;
    static constexpr int kMaxDisparity = 40;
    static constexpr int kUnknown = 0x7FFF;

    // shades[eye]: the eye's picture, a shade 0-3 per pixel (0: nothing
    // there), row-major kWidth x kHeight. want[eye]: 1 where a pixel needs a
    // depth color. Works out those pixels' disparities.
    void Update(const uint8_t *const shades[2], const uint8_t *const want[2]);
    // A wanted pixel's disparity as last worked out (kUnknown: none).
    int Disparity(unsigned eye, unsigned x, unsigned y) const { return m_disparity[eye][y * kWidth + x]; }
    // The color (R, G, B) for a disparity and shade 1-3.
    static void Color(int disparity, unsigned shade, uint8_t rgb[3]);
    // Forget everything (a new game).
    void Reset();

private:
    static constexpr int kDisparities = 2 * kMaxDisparity + 1;
    static constexpr int kBlocksX = kWidth / 8, kBlocksY = kHeight / 8, kBlocks = kBlocksX * kBlocksY;
    // A row of one shade's pixels as bits (pixel x at bit x + 64): a guard
    // word before and two after, so a shift by up to 64 either way stays inside.
    static constexpr int kRowWords = kWidth / 64, kGuarded = kRowWords + 3;
    using Row = std::array<uint64_t, kGuarded>;

    struct State
    {
        std::vector<int16_t> cost;   // per disparity and block, as last smoothed from - for the next frame
        std::vector<int16_t> best;   // per block: its disparity (an index into the disparities)
        std::array<int, 4> planes{}; // the frame's main disparities (indices; -1 none)
        bool estimated = false;
    };
    void Estimate(const uint8_t *const shades[2], const uint8_t *const want[2]);
    int Votes(unsigned s, const unsigned left[7], int x, int y, int d) const;
    void LeftWindow(unsigned s, int x, int y, unsigned left[7]) const;

    std::vector<Row> m_bits[2][3]; // per eye and shade 1-3: kHeight rows
    State m_state;
    std::vector<uint8_t> m_lastShades[2], m_lastWant[2];
    std::vector<int16_t> m_disparity[2];     // per pixel (kUnknown: none)
    std::vector<int16_t> m_lastDisparity;    // the left eye's, as last worked out (a pixel that hasn't changed keeps it)
    bool m_haveLast = false;

    // Scratch (kept to avoid reallocating every frame).
    std::vector<uint8_t> m_matches, m_inside;             // per disparity, block: matching / countable pixels
    std::vector<int16_t> m_costByDisparity, m_cost, m_sum; // the costs per disparity and block; per block and disparity; smoothed
    std::vector<int32_t> m_blockPixels;                   // per block: pixels in the 3x3 blocks around it
};
