#pragma once

#include "emu/ShadeColorizer.h"
#include "emu/TileColorPack.h"
#include "emu/vbgo_tiletrack.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Paints a color pack (see TileColorPack.h) over a frame the shade
// colorizer has already colored, from the tile tracker's per-pixel tags (see
// vbgo_tiletrack.h). Kept free of any GPU/UI code so tools and tests can use
// it as-is.
//
// Cost is kept per tile and per map cell rather than per pixel: a character
// slot's colors (tile, palette variants) are looked up only when the slot's
// contents change, and map cells index straight into a table built once
// when the pack is set - so a pixel is a few array reads, no hash lookups.
class ColorPackRenderer
{
public:
    // Builds the lookup tables (call again whenever the pack changes; the
    // pack must outlive its use here). nullptr / an empty pack disables.
    void SetPack(const TileColorPack *pack);
    bool Active() const { return m_pack != nullptr; }

    // The map cells the pack has fill colors for, as the 65536-bit map
    // vbgo_tiletrack_set_fill_cells takes; empty if there are none.
    const std::vector<uint8_t> &FillCells() const { return m_fillCells; }

    // frame: B,G,R,A bytes as the shade colorizer left them; raw: the core's
    // frame (B,G,R,tag); both fbWidth pixels per row, the eyes side by side
    // starting at eyeOffset[0/1]. background: the Multicolor palette's
    // background, which painted colors fade toward with the game's
    // brightness (same as the palette itself - see ShadeColorizer).
    void Paint(uint8_t *frame, const uint8_t *raw, uint32_t fbWidth, const uint32_t eyeOffset[2], const ShadeRgb &background);

private:
    struct Slot
    {
        uint32_t hash = 0;
        bool resolved = false;
        bool layered = false;                        // the tile has per-layer colors - look them up
        const TileColorPack::Tile *base = nullptr;   // the tile's own colors
        const TileColorPack::Tile *palette[4] = {};  // per palette: its own colors, else base
        uint64_t markerBits = 0;                     // context groups this tile is a marker of
        uint32_t contextFirst = 0, contextCount = 0; // its context variants in the pack's ContextTiles()
        bool ambiguous = false;                      // painted two ways in a frame: takes its surroundings' colors
    };

    void ResolveSlots(const uint32_t *hashes);

    // Pixels of ambiguous tiles (see TileColorPack::IsAmbiguous) painted this
    // eye, (x, y) - they take the color the same shade has next to them.
    std::vector<std::pair<uint16_t, uint16_t>> m_ambiguousPixels;
    void PaintAmbiguous(uint8_t *frame, uint32_t fbWidth, uint32_t eyeOffset, const vbgo_tt_eye_view &view);

    // Gives the right eye the left eye's colors (see the .cpp).
    void MatchEyes(uint8_t *frame, const uint8_t *raw, uint32_t fbWidth, const uint32_t eyeOffset[2]);
    static constexpr int kMaxDisparity = 64, kUnknownDisparity = 0x7FFF;
    static constexpr int kWindowWidth = 15, kWindowHeight = 3; // what's compared around a pixel (odd sizes; width <= 32)
    static constexpr int kCloseMatch = 2, kFairMatch = 9;       // of the window's pixels, at most this many differ
    static constexpr int kSearchBudget = 3000;                  // pixels a frame that search all disparities
    static constexpr int kResearchEvery = 16;                   // frames between searches for a pixel that found nothing close (power of 2)
    static constexpr uint8_t kNotSearched = 0xFF, kNoMatch = 0xFE;
    std::vector<uint8_t> m_eyeCostAt; // per right-eye pixel: how far off last frame's match by looks was (or one of those)
    uint32_t m_eyeFrame = 0;
    std::vector<uint32_t> m_eyeTags;   // both eyes' tags, row-major, packed (see MatchEyes)
    std::vector<uint64_t> m_eyeShades; // both eyes' shades, 2 bits per pixel, rows with guard pixels (see MatchEyes)
    std::vector<int16_t> m_eyeDisparityAt; // per right-eye pixel: the disparity its match had last frame
    std::array<uint64_t, (1u << 18) / 64> m_leftShows{}; // per (tile name, pixel, sprite): the left eye shows it this frame
    std::array<int16_t, 32> m_eyeDisparity = MakeUnknownDisparities(); // per world: right eye x -> left eye x
    std::vector<int16_t> m_eyeRowDisparity = std::vector<int16_t>(VBGO_TT_HEIGHT * 32, kUnknownDisparity); // per row, per world
    struct TileName
    {
        uint32_t hash = 0;
        uint16_t name = 0;
        uint32_t generation = 0;
    };
    static constexpr unsigned kTileNameBits = 13, kTileNameSlots = 1u << kTileNameBits; // for up to 2 eyes x 2048 tiles
    std::vector<TileName> m_tileNames; // tile hash -> its name this frame (open addressing; current generation only)
    uint32_t m_tileNameGeneration = 0;
    static constexpr std::array<int16_t, 32> MakeUnknownDisparities()
    {
        std::array<int16_t, 32> a{};
        for (auto &d : a)
            d = kUnknownDisparity;
        return a;
    }

    // Context (see TileColorPack.h): where markers were drawn, per eye, on a
    // grid of 8x8-pixel screen cells - the last frame's and the one being
    // painted. A shared tile takes a group's colors if one of its markers
    // is within kContextReach cells. Sprites' groups only color sprites;
    // layer-bound groups (background figures) only their own layer, by a
    // marker drawn on it (each cell remembers the layer its last such
    // marker was drawn on).
    static constexpr int kGridW = VBGO_TT_WIDTH / 8, kGridH = VBGO_TT_HEIGHT / 8, kContextReach = 4;
    std::unordered_map<uint32_t, uint64_t> m_markerBits;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> m_contextRange; // hash -> (first, count)
    std::vector<uint64_t> m_markerGrid[2][2];
    std::vector<uint8_t> m_markerLayer[2][2];
    uint64_t m_layerBound = 0;
    unsigned m_gridCurrent[2] = {};
    uint64_t Near(unsigned eye, int x, int y, unsigned world) const;

    const TileColorPack *m_pack = nullptr;
    std::vector<uint32_t> m_cellStart;  // 65537 entries: cell n's CellTiles are [m_cellStart[n], m_cellStart[n + 1])
    std::vector<uint8_t> m_fillCells;
    std::unordered_set<uint32_t> m_layered; // hashes with per-layer colors
    std::array<Slot, 2048> m_slots{};
    const uint32_t *m_resolvedFor = nullptr; // the hash table the slots were last checked against
    int m_fade[64] = {}; // brightness level -> 0-256, relative to the pack's reference brightness
};
