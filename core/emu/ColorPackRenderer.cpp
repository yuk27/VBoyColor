#include "emu/ColorPackRenderer.h"
#include "emu/vbgo_tiletrack.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

void ColorPackRenderer::SetPack(const TileColorPack *pack)
{
    m_pack = pack && !pack->Empty() ? pack : nullptr;
    m_cellStart.clear();
    m_fillCells.clear();
    m_layered.clear();
    m_markerBits.clear();
    m_contextRange.clear();
    for (Slot &slot : m_slots)
        slot = Slot{};
    m_resolvedFor = nullptr;
    for (auto &eye : m_markerGrid)
        for (auto &grid : eye)
            grid.clear();
    for (auto &eye : m_markerLayer)
        for (auto &grid : eye)
            grid.clear();
    m_layerBound = 0;
    if (!m_pack)
        return;
    const auto &groups = m_pack->ContextGroups();
    const auto &contexts = m_pack->ContextTiles(); // sorted by hash
    for (size_t g = 0; g < groups.size() && g < 64; ++g)
        for (const uint32_t marker : groups[g])
            m_markerBits[marker] |= 1ull << g;
    for (uint32_t i = 0; i < contexts.size();)
    {
        uint32_t j = i;
        while (j < contexts.size() && contexts[j].hash == contexts[i].hash)
            ++j;
        m_contextRange[contexts[i].hash] = {i, j - i};
        i = j;
    }
    if (!contexts.empty())
    {
        for (auto &eye : m_markerGrid)
            for (auto &grid : eye)
                grid.assign(kGridW * kGridH, 0);
        m_layerBound = m_pack->LayerBoundGroups();
        if (m_layerBound)
            for (auto &eye : m_markerLayer)
                for (auto &grid : eye)
                    grid.assign(kGridW * kGridH, 0xFF);
    }
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
        slot.ambiguous = m_pack->IsAmbiguous(slot.hash);
        slot.markerBits = 0;
        slot.contextCount = 0;
        if (!m_markerBits.empty())
        {
            const auto marker = m_markerBits.find(slot.hash);
            if (marker != m_markerBits.end())
                slot.markerBits = marker->second;
            const auto range = m_contextRange.find(slot.hash);
            if (range != m_contextRange.end())
                slot.contextFirst = range->second.first, slot.contextCount = range->second.second;
        }
    }
    m_resolvedFor = hashes;
}

uint64_t ColorPackRenderer::Near(unsigned eye, int x, int y, unsigned world) const
{
    // Markers drawn within reach - last frame, or already this frame (of a
    // layer-bound group, only ones drawn on this pixel's layer).
    const int cx = x >> 3, cy = y >> 3;
    uint64_t bits = 0;
    for (int gy = std::max(0, cy - kContextReach); gy <= std::min(kGridH - 1, cy + kContextReach); ++gy)
        for (int gx = std::max(0, cx - kContextReach); gx <= std::min(kGridW - 1, cx + kContextReach); ++gx)
        {
            const int i = gy * kGridW + gx;
            for (unsigned g = 0; g < 2; ++g)
            {
                const uint64_t here = m_markerGrid[eye][g][i];
                if (!m_layerBound)
                    bits |= here;
                else
                    bits |= (here & ~m_layerBound) | (m_markerLayer[eye][g][i] == world ? here & m_layerBound : 0);
            }
        }
    return bits;
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
    const TileColorPack::ContextTile *contexts = m_pack->ContextTiles().data();
    const bool haveContexts = !m_markerGrid[0][0].empty();

    for (unsigned eye = 0; eye < 2; ++eye)
    {
        vbgo_tt_eye_view view;
        if (!vbgo_tiletrack_eye_view(eye, &view))
            continue;
        ResolveSlots(view.hashes);
        uint64_t *markers = nullptr; // this frame's marker grid (the other one keeps the last frame's)
        uint8_t *markerLayers = nullptr;
        if (haveContexts)
        {
            m_gridCurrent[eye] ^= 1;
            markers = m_markerGrid[eye][m_gridCurrent[eye]].data();
            std::fill(markers, markers + kGridW * kGridH, 0);
            if (m_layerBound)
            {
                markerLayers = m_markerLayer[eye][m_gridCurrent[eye]].data();
                std::fill(markerLayers, markerLayers + kGridW * kGridH, 0xFF);
            }
        }
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
            int nearCell = -1; // the same for markers nearby (cell and layer)
            uint64_t nearBits = 0;
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
                if (pixel && m_slots[chr].ambiguous)
                    m_ambiguousPixels.emplace_back(static_cast<uint16_t>(x), static_cast<uint16_t>(y));
                const uint8_t *rgb = nullptr;
                if (markers && pixel && m_slots[chr].markerBits)
                {
                    // Sprites' groups count sprite markers, background figures'
                    // groups background ones (and remember the layer).
                    const bool sprite = VBGO_TAG_IS_OBJ(t) != 0;
                    const uint64_t bits = m_slots[chr].markerBits & (sprite ? ~m_layerBound : m_layerBound);
                    if (bits)
                    {
                        const unsigned cell = (y >> 3) * kGridW + (x >> 3);
                        markers[cell] |= bits;
                        if (!sprite && markerLayers)
                            markerLayers[cell] = static_cast<uint8_t>(VBGO_TAG_WORLD(t));
                    }
                }

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
                    const Slot &slot = m_slots[chr];
                    const TileColorPack::Tile *tile = nullptr;
                    // Context: a shared tile in an object whose marker is nearby
                    // (a sprite's colors only on sprites, a background
                    // figure's only on its layer).
                    if (slot.contextCount)
                    {
                        const unsigned world = VBGO_TAG_WORLD(t), sprite = VBGO_TAG_IS_OBJ(t);
                        const int cell = static_cast<int>(((y >> 3) * kGridW + (x >> 3)) | (world << 16) | (sprite << 21));
                        if (cell != nearCell)
                        {
                            nearCell = cell;
                            nearBits = Near(eye, static_cast<int>(x), static_cast<int>(y), world) & (sprite ? ~m_layerBound : m_layerBound);
                        }
                        for (uint32_t k = 0; nearBits && k < slot.contextCount; ++k)
                        {
                            const TileColorPack::ContextTile &c = contexts[slot.contextFirst + k];
                            if (nearBits >> c.group & 1)
                            {
                                tile = &c.tile;
                                break;
                            }
                        }
                    }
                    // 2-4. The palette's, the layer's, or the tile's own colors.
                    if (!tile)
                    {
                        tile = slot.palette[palette];
                        if (slot.layered && tile == slot.base)
                            if (const TileColorPack::Tile *layer = m_pack->FindLayer(slot.hash, VBGO_TAG_WORLD(t)))
                                tile = layer;
                    }
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
        if (!m_ambiguousPixels.empty())
            PaintAmbiguous(frame, fbWidth, eyeOffset[eye], view);
    }
    MatchEyes(frame, fbWidth, eyeOffset);
}

void ColorPackRenderer::PaintAmbiguous(uint8_t *frame, uint32_t fbWidth, uint32_t eyeOffset, const vbgo_tt_eye_view &view)
{
    // A tile a character paints two ways in one frame (a plain filled tile:
    // the shirt's white, the cap's green) takes, pixel by pixel, the color
    // of the nearest pixel of the same shade on its layer that isn't such a
    // tile - the shirt's or the cap's around it (within 8 pixels, straight
    // up, down, left or right). Without one it keeps what the pack gave it.
    auto tagAt = [&](int x, int y, uint64_t &t) {
        if (x < 0 || x >= VBGO_TT_WIDTH || y < 0 || y >= VBGO_TT_HEIGHT || !view.columns[x])
            return false;
        t = view.columns[x][y];
        return (t >> 48) == view.stamp;
    };
    static constexpr int kDirections[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
    for (const auto &p : m_ambiguousPixels)
    {
        const int x = p.first, y = p.second;
        uint64_t t = 0, n = 0;
        if (!tagAt(x, y, t))
            continue;
        int from = -1;
        for (int d = 1; d <= 8 && from < 0; ++d)
            for (const auto &dir : kDirections)
            {
                const int nx = x + dir[0] * d, ny = y + dir[1] * d;
                if (tagAt(nx, ny, n) && VBGO_TAG_PIXEL(n) == VBGO_TAG_PIXEL(t) && VBGO_TAG_WORLD(n) == VBGO_TAG_WORLD(t) &&
                    VBGO_TAG_IS_OBJ(n) == VBGO_TAG_IS_OBJ(t) && !m_slots[VBGO_TAG_CHAR(n)].ambiguous)
                {
                    from = ny * static_cast<int>(fbWidth) + static_cast<int>(eyeOffset) + nx;
                    break;
                }
            }
        if (from >= 0)
            std::memcpy(&frame[(static_cast<size_t>(y) * fbWidth + eyeOffset + x) * 4], &frame[static_cast<size_t>(from) * 4], 3);
    }
    m_ambiguousPixels.clear();
}

void ColorPackRenderer::MatchEyes(uint8_t *frame, uint32_t fbWidth, const uint32_t eyeOffset[2])
{
    // Both eyes see one scene, so they must show it in the same colors - but
    // the pack is painted from left-eye captures, and games often draw a
    // layer twice, once per eye on a world of its own (Wario Land: 23 left,
    // 24 right), with map cells and even tiles of its own (pre-shifted
    // copies): looked up on its own, the right eye would get other layers'
    // colors, or none. So every right-eye pixel takes the color of the
    // left-eye pixel showing the same thing:
    //  1. the same tile pixel on that row (sprite or background alike), at
    //     the disparity its layer had last time, or else wherever the left
    //     eye shows it on the row (within kMaxDisparity);
    //  2. for a tile pixel the left eye doesn't show - the right eye's own
    //     copy of a texture, or a picture drawn once per eye with tiles of
    //     its own (Galactic Pinball's title and tables, with depth inside
    //     them) - the left pixel the row's pixel values say it is: the
    //     disparity this pixel had last frame or the one its neighbour on
    //     the row just had, if the left pixel there has the same value;
    //     else the best match of the pixels around it within kSearch of its
    //     layer's disparity.
    // Anything without a counterpart (cut off at the screen's edge) keeps
    // its own colors.
    vbgo_tt_eye_view view[2];
    if (!vbgo_tiletrack_eye_view(0, &view[0]) || !vbgo_tiletrack_eye_view(1, &view[1]))
        return;
    // Both eyes' tags, row by row, packed to what matching needs: bit 31
    // drawn this frame, bits 0-17 what it shows (character slot, pixel,
    // sprite), 18-19 the pixel's value, 20-24 the layer.
    constexpr uint32_t kDrawn = 1u << 31, kWhat = (1u << 18) - 1;
    m_eyeTags.resize(2 * VBGO_TT_EYE_PIXELS);
    if (m_eyeDisparityAt.empty())
        m_eyeDisparityAt.assign(VBGO_TT_EYE_PIXELS, kUnknownDisparity);
    for (unsigned eye = 0; eye < 2; ++eye)
    {
        uint32_t *out = &m_eyeTags[eye * VBGO_TT_EYE_PIXELS];
        const uint64_t stamp = view[eye].stamp;
        for (int x0 = 0; x0 < VBGO_TT_WIDTH; x0 += 8) // in 8x8 blocks, for the cache
            for (int y0 = 0; y0 < VBGO_TT_HEIGHT; y0 += 8)
                for (int x = x0; x < x0 + 8; ++x)
                {
                    const uint64_t *column = view[eye].columns[x];
                    for (int y = y0; y < y0 + 8; ++y)
                    {
                        const uint64_t t = column ? column[y] : 0;
                        out[y * VBGO_TT_WIDTH + x] =
                            column && (t >> 48) == stamp
                                ? kDrawn | VBGO_TAG_CHAR(t) | (VBGO_TAG_INDEX(t) << 11) | (VBGO_TAG_IS_OBJ(t) << 17) |
                                      (VBGO_TAG_PIXEL(t) << 18) | (VBGO_TAG_WORLD(t) << 20)
                                : 0;
                    }
                }
    }
    // (Both eyes come from one drawing pass, so a character slot is the same
    // tile in both - checked, in case a game changes one in between.)
    const bool sameSlots = view[0].hashes == view[1].hashes;
    // Where on the current row the left eye shows each tile pixel: a small
    // open-addressing table, valid for the row whose number it carries.
    struct Seen
    {
        uint32_t what;
        int16_t x;
        uint16_t row;
    };
    std::array<Seen, 1024> seenAt;
    for (Seen &e : seenAt)
        e.row = 0xFFFF;
    auto slotOf = [](uint32_t what) { return (what * 2654435761u) >> 22; };
    std::array<std::array<uint32_t, 2 * kMaxDisparity + 1>, 32> votes{};
    for (int y = 0; y < VBGO_TT_HEIGHT; ++y)
    {
        const uint32_t *left = &m_eyeTags[y * VBGO_TT_WIDTH];
        const uint32_t *right = &m_eyeTags[VBGO_TT_EYE_PIXELS + y * VBGO_TT_WIDTH];
        uint8_t *row = &frame[static_cast<size_t>(y) * fbWidth * 4];
        auto same = [&](int xl, uint32_t r) {
            return xl >= 0 && xl < VBGO_TT_WIDTH && ((left[xl] ^ r) & (kDrawn | kWhat)) == 0 &&
                   (sameSlots || view[0].hashes[left[xl] & 0x7FF] == view[1].hashes[r & 0x7FF]);
        };
        auto take = [&](int x, int from, bool identical) {
            if (identical)
            {
                const unsigned world = (right[x] >> 20) & 31;
                ++votes[world][from - x + kMaxDisparity];
            }
            std::memcpy(&row[(eyeOffset[1] + x) * 4], &row[(eyeOffset[0] + from) * 4], 3);
        };
        // Mostly the same tile pixel at its layer's disparity; the rest after.
        int missed[VBGO_TT_WIDTH], misses = 0;
        for (int x = 0; x < VBGO_TT_WIDTH; ++x)
        {
            const uint32_t r = right[x];
            if (!r)
                continue;
            const int predicted = m_eyeDisparity[(r >> 20) & 31];
            if (predicted != kUnknownDisparity && same(x + predicted, r))
                take(x, x + predicted, true);
            else
                missed[misses++] = x;
        }
        if (!misses)
            continue;
        for (int x = 0; x < VBGO_TT_WIDTH; ++x)
            if (left[x])
            {
                const uint32_t what = left[x] & kWhat;
                uint32_t i = slotOf(what);
                while (seenAt[i].row == y && seenAt[i].what != what)
                    i = (i + 1) & 1023;
                seenAt[i] = {what, static_cast<int16_t>(x), static_cast<uint16_t>(y)};
            }
        int runDisparity = kUnknownDisparity, runEnd = -2;
        unsigned runWorld = 32;
        for (int m = 0; m < misses; ++m)
        {
            const int x = missed[m];
            const uint32_t r = right[x];
            const unsigned world = (r >> 20) & 31;
            const int predicted = m_eyeDisparity[world];
            int seen = -1;
            for (uint32_t i = slotOf(r & kWhat); seenAt[i].row == y; i = (i + 1) & 1023)
                if (seenAt[i].what == (r & kWhat))
                {
                    seen = seenAt[i].x;
                    break;
                }
            if (seen >= 0 && std::abs(seen - x) <= kMaxDisparity && same(seen, r))
            {
                take(x, seen, true);
                continue;
            }
            // 2. By the pixels' values.
            int16_t &cached = m_eyeDisparityAt[y * VBGO_TT_WIDTH + x];
            auto off = [&](int d, int o) { // the pixel o from x and its counterpart at disparity d differ
                const int xr = x + o, xl = x + d + o;
                const uint32_t a = xr >= 0 && xr < VBGO_TT_WIDTH ? right[xr] : 0;
                const uint32_t b = xl >= 0 && xl < VBGO_TT_WIDTH ? left[xl] : 0;
                return ((a ^ b) & (kDrawn | 3u << 18 | 1u << 17)) != 0;
            };
            auto fits = [&](int d) { return !off(d, 0) && !off(d, -1) && !off(d, 1); };
            int d = kUnknownDisparity;
            if (cached != kUnknownDisparity && fits(cached))
                d = cached;
            else if (runDisparity != kUnknownDisparity && runWorld == world && runEnd + 2 >= x && fits(runDisparity))
                d = runDisparity;
            else
            {
                // The left pixel whose neighbours (2 each side) match best.
                const int around = predicted != kUnknownDisparity ? predicted : runWorld == world && runDisparity != kUnknownDisparity ? runDisparity : 0;
                int best = 3; // at most 2 of the 5 off
                for (int k = 0; k <= 2 * kSearch; ++k)
                {
                    const int c = around + ((k & 1) ? -(k + 1) / 2 : k / 2);
                    if (off(c, 0))
                        continue;
                    int cost = 0;
                    for (int o = -2; o <= 2 && cost < best; ++o)
                        cost += off(c, o);
                    if (cost < best)
                        best = cost, d = c;
                    if (best == 0)
                        break;
                }
            }
            cached = static_cast<int16_t>(d);
            if (d == kUnknownDisparity)
                continue;
            runDisparity = d, runWorld = world, runEnd = x;
            take(x, x + d, false);
        }
    }
    // Each layer's disparity for the next frame: where most of its pixels
    // matched (kept if it showed nothing matching this time).
    for (unsigned w = 0; w < 32; ++w)
    {
        uint32_t best = 0;
        for (int d = 0; d <= 2 * kMaxDisparity; ++d)
            if (votes[w][d] > best)
                best = votes[w][d], m_eyeDisparity[w] = static_cast<int16_t>(d - kMaxDisparity);
    }
}
