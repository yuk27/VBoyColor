#include "emu/ColorPackRenderer.h"
#include "emu/vbgo_tiletrack.h"

#include <algorithm>
#include <cmath>

void ColorPackRenderer::SetPack(const TileColorPack *pack)
{
    m_pack = pack && !pack->Empty() ? pack : nullptr;
    m_cellStart.clear();
    m_fillCells.clear();
    m_layered.clear();
    for (Slot &slot : m_slots)
        slot = Slot{};
    m_resolvedFor = nullptr;
    if (!m_pack)
        return;
    // Same fade as the Multicolor palette (see ShadeColorizer): the brightest
    // shade's level from the core's tag byte, through the core's gamma -
    // relative to the brightness the paintings were made at, so a game that
    // normally runs below full brightness shows exactly the painted colors.
    const float reference = std::pow(m_pack->ReferenceLevel() / 63.0f, 1.0f / 2.2f);
    for (int i = 0; i < 64; ++i)
        m_fade[i] = static_cast<int>(std::lround(256.0f * std::min(1.0f, std::pow(static_cast<float>(i) / 63.0f, 1.0f / 2.2f) / reference)));

    const std::vector<TileColorPack::CellTile> &cells = m_pack->CellTiles(); // sorted by cell
    m_cellStart.assign(65537, 0);
    for (const TileColorPack::CellTile &cell : cells)
        ++m_cellStart[cell.cell + 1u];
    for (size_t i = 1; i < m_cellStart.size(); ++i)
        m_cellStart[i] += m_cellStart[i - 1];
    // Fills: a cell has some if any of its tile entries colors a pixel the
    // tile leaves transparent - simplest to mark every cell with an entry
    // (the tracker only uses this to decide where tagging fills is worth it).
    if (!cells.empty())
    {
        m_fillCells.assign(8192, 0);
        for (const TileColorPack::CellTile &cell : cells)
            if (cell.mask)
                m_fillCells[cell.cell >> 3] |= static_cast<uint8_t>(1u << (cell.cell & 7));
    }
    if (m_pack->HasLayerColors())
        for (const auto &tile : m_pack->Tiles())
            for (unsigned world = 0; world < 32; ++world)
                if (m_pack->FindLayer(tile.first, world))
                {
                    m_layered.insert(tile.first);
                    break;
                }
}

void ColorPackRenderer::ResolveSlots(const uint32_t *hashes)
{
    // Only slots whose contents changed since last time need a lookup -
    // usually a handful a frame (streamed animation tiles).
    for (unsigned c = 0; c < 2048; ++c)
    {
        Slot &slot = m_slots[c];
        if (slot.resolved && slot.hash == hashes[c])
            continue;
        slot.hash = hashes[c];
        slot.resolved = true;
        slot.base = m_pack->Find(slot.hash);
        for (unsigned p = 0; p < 4; ++p)
        {
            const TileColorPack::Tile *variant = m_pack->HasPaletteColors() ? m_pack->FindPalette(slot.hash, p) : nullptr;
            slot.palette[p] = variant ? variant : slot.base;
        }
        slot.layered = !m_layered.empty() && m_layered.count(slot.hash) != 0;
    }
    m_resolvedFor = hashes;
}

void ColorPackRenderer::Paint(uint8_t *frame, const uint8_t *raw, uint32_t fbWidth, const uint32_t eyeOffset[2],
                              const ShadeRgb &background)
{
    if (!m_pack)
        return;
    const int bg[3] = {static_cast<int>(background.b * 255.0f + 0.5f), static_cast<int>(background.g * 255.0f + 0.5f),
                       static_cast<int>(background.r * 255.0f + 0.5f)};
    const TileColorPack::CellTile *cellTiles = m_pack->CellTiles().data();
    const bool haveCells = !m_cellStart.empty();

    for (unsigned eye = 0; eye < 2; ++eye)
    {
        vbgo_tt_eye_view view;
        if (!vbgo_tiletrack_eye_view(eye, &view))
            continue;
        ResolveSlots(view.hashes);
        const uint64_t stamp = view.stamp;
        for (uint32_t x = 0; x < VBGO_TT_WIDTH; ++x)
        {
            const uint64_t *column = view.columns[x];
            if (!column)
                continue;
            const size_t base = (static_cast<size_t>(eyeOffset[eye]) + x) * 4;
            // Pixels next to each other mostly come from the same map cell -
            // remember the last one's lookup.
            uint32_t lastCellKey = ~0u;
            const TileColorPack::CellTile *lastCell = nullptr;
            for (uint32_t y = 0; y < VBGO_TT_HEIGHT; ++y)
            {
                const uint64_t t = column[y];
                if ((t >> 48) != stamp)
                    continue; // nothing tracked here this frame (background)
                const size_t i = base + static_cast<size_t>(y) * fbWidth * 4;
                const uint8_t *r = &raw[i];
                const unsigned pixel = VBGO_TAG_PIXEL(t);
                if (pixel && (r[0] | r[1] | r[2]) == 0)
                    continue; // drawn in a shade the game switched off - stays background
                const unsigned chr = VBGO_TAG_CHAR(t), index = VBGO_TAG_INDEX(t), palette = VBGO_TAG_PALETTE(t);
                const uint8_t *rgb = nullptr;

                // 1. The map cell's own colors for this tile (and fills).
                if (haveCells && VBGO_TAG_HAS_CELL(t))
                {
                    const unsigned cell = VBGO_TAG_CELL(t);
                    const uint32_t cellKey = (cell << 13) | (palette << 11) | chr;
                    if (cellKey != lastCellKey)
                    {
                        lastCellKey = cellKey;
                        lastCell = nullptr;
                        for (uint32_t k = m_cellStart[cell], end = m_cellStart[cell + 1]; k < end; ++k)
                            if (cellTiles[k].palette == palette && cellTiles[k].hash == view.hashes[chr])
                            {
                                lastCell = &cellTiles[k];
                                break;
                            }
                    }
                    if (lastCell)
                    {
                        if (lastCell->keep >> index & 1)
                            continue; // left uncolored here on purpose
                        if (lastCell->mask >> index & 1)
                            rgb = lastCell->rgb[index];
                    }
                }
                if (!rgb)
                {
                    if (!pixel)
                        continue; // a fill nobody painted
                    // 2-4. The palette's, the layer's, or the tile's own colors.
                    const Slot &slot = m_slots[chr];
                    const TileColorPack::Tile *tile = slot.palette[palette];
                    if (slot.layered && tile == slot.base)
                        if (const TileColorPack::Tile *layer = m_pack->FindLayer(slot.hash, VBGO_TAG_WORLD(t)))
                            tile = layer;
                    if (!tile || !(tile->mask >> index & 1))
                        continue; // unpainted - keeps the Multicolor palette's color
                    rgb = tile->rgb[index];
                }
                uint8_t *dst = &frame[i];
                const int fade = m_fade[r[3] >> 2]; // 0-256
                if (fade == 256)
                {
                    dst[0] = rgb[2];
                    dst[1] = rgb[1];
                    dst[2] = rgb[0];
                }
                else
                {
                    dst[0] = static_cast<uint8_t>(bg[0] + (((rgb[2] - bg[0]) * fade + 128) >> 8));
                    dst[1] = static_cast<uint8_t>(bg[1] + (((rgb[1] - bg[1]) * fade + 128) >> 8));
                    dst[2] = static_cast<uint8_t>(bg[2] + (((rgb[0] - bg[2]) * fade + 128) >> 8));
                }
            }
        }
    }
}
