#pragma once

#include "emu/TileColorPack.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

// Collects what a color pack doesn't cover yet into paint-ready sheets
// (experimental, desktop F7).
//
// While on, every frame is checked for lit pixels the pack has no color for.
// Each such object (a sprite, or a patch of background) is cropped out - only
// its own layer, at full brightness, painted parts in their pack colors and
// the rest in the active Multicolor palette - unless the tile pixels it's
// missing were already collected from an earlier frame. A frame showing a lot
// of new background is kept whole instead (easier to paint with the whole
// scene in view). Crops are then laid out on 384x224 sheets with the same
// per-pixel tile records as an F10 capture, so a painted sheet imports like
// any other painting.
class UncoloredCollector
{
public:
    struct Sheet
    {
        std::vector<uint8_t> rgb;    // 384x224, 3 bytes per pixel
        std::vector<uint64_t> tiles; // 384x224 tile records (0 = background)
    };

    void Reset();

    // tiles: the tile tracker's left-eye records. raw: the core's frame
    // (B,G,R,tag bytes), rawStride pixels per row, left eye at x 0.
    // palette: 0-255 RGB for background and the 3 shades (unpainted pixels).
    // Returns how many crops this frame added.
    int AddFrame(const uint64_t *tiles, const uint8_t *raw, size_t rawStride, const TileColorPack &pack,
                 const std::array<std::array<uint8_t, 3>, 4> &palette);

    size_t PendingCrops() const { return m_crops.size(); }
    size_t CollectedTilePixels() const { return m_collected.size(); }

    // Lays the pending crops out on as few sheets as fit and forgets them
    // (what was collected stays known, so it isn't collected again).
    std::vector<Sheet> TakeSheets(const std::array<uint8_t, 3> &background);

private:
    struct Crop
    {
        int w = 0, h = 0;
        std::vector<uint8_t> rgb;    // w * h * 3
        std::vector<uint64_t> tiles; // w * h
        std::vector<uint64_t> keys;  // the uncolored tile pixels it shows
    };

    Crop MakeCrop(const uint64_t *tiles, const uint8_t *raw, size_t rawStride, const TileColorPack &pack,
                  const std::array<std::array<uint8_t, 3>, 4> &palette, const std::vector<uint8_t> &state, int x0, int y0,
                  int x1, int y1, int world) const;
    void Keep(Crop &&crop);

    std::vector<Crop> m_crops;
    // A new scene is kept once the screen settles (transitions draw it over
    // several frames, sometimes over the previous screen) - or, if it never
    // does, the most complete frame seen.
    Crop m_scene;
    size_t m_sceneFresh = 0;
    int m_sceneFramesLeft = 0, m_sceneStill = 0;
    std::vector<uint32_t> m_lastHashes;
    std::unordered_set<uint64_t> m_collected; // (hash << 6 | tile pixel)
    std::vector<int> m_label, m_queue;
};
