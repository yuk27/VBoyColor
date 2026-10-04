#pragma once

#include "emu/ShadeColorizer.h"
#include "emu/TileColorPack.h"

#include <array>
#include <cstddef>
#include <cstdint>
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
    };

    void ResolveSlots(const uint32_t *hashes);

    const TileColorPack *m_pack = nullptr;
    std::vector<uint32_t> m_cellStart;  // 65537 entries: cell n's CellTiles are [m_cellStart[n], m_cellStart[n + 1])
    std::vector<uint8_t> m_fillCells;
    std::unordered_set<uint32_t> m_layered; // hashes with per-layer colors
    std::array<Slot, 2048> m_slots{};
    const uint32_t *m_resolvedFor = nullptr; // the hash table the slots were last checked against
    int m_fade[64] = {}; // brightness level -> 0-256, relative to the pack's reference brightness
};
