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
    // (eye matching starts over: last frame's matches belong to another game)
    m_eyeDisparityAt.clear();
    m_eyeCostAt.clear();
    m_eyeDisparity = MakeUnknownDisparities();
    std::fill(m_eyeRowDisparity.begin(), m_eyeRowDisparity.end(), static_cast<int16_t>(kUnknownDisparity));
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
    MatchEyes(frame, raw, fbWidth, eyeOffset);
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

namespace
{
// Shades packed 2 bits per pixel (pixel p of a row at bit 2 * (p + guard)),
// with guard pixels each side so a window at any disparity stays in the row.
constexpr int kGuardPixels = 96;
constexpr int kShadeRowWords = (VBGO_TT_WIDTH + 2 * kGuardPixels) * 2 / 64;

inline uint64_t ShadeBits(const uint64_t *row, int x, int n)
{
    const int bit = (x + kGuardPixels) * 2, w = bit >> 6, s = bit & 63;
    uint64_t v = row[w] >> s;
    if (s)
        v |= row[w + 1] << (64 - s);
    return v & ((1ull << (2 * n)) - 1);
}

inline int CountSet(uint64_t x)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(x);
#else
    x = x - ((x >> 1) & 0x5555555555555555ull);
    x = (x & 0x3333333333333333ull) + ((x >> 2) & 0x3333333333333333ull);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0Full;
    return static_cast<int>((x * 0x0101010101010101ull) >> 56);
#endif
}

// Pixels whose shades differ, of two runs of 2-bit shades.
inline int ShadesOff(uint64_t a, uint64_t b)
{
    const uint64_t x = a ^ b;
    return CountSet((x | (x >> 1)) & 0x5555555555555555ull);
}
} // namespace

void ColorPackRenderer::MatchEyes(uint8_t *frame, const uint8_t *raw, uint32_t fbWidth, const uint32_t eyeOffset[2])
{
    // Both eyes see one scene, so they must show it in the same colors - but
    // the pack is painted from left-eye captures, and games often draw a
    // layer twice, once per eye on a world of its own (Mario Clash), with
    // map cells and even tiles of its own (pre-shifted copies), shift a
    // layer by another amount on every row (Wario Land's mountains), or draw
    // each eye's picture separately, depth and all (Galactic Pinball's title
    // and tables): looked up on its own, the right eye would get other
    // places' colors, or none. So every right-eye pixel takes the color of
    // the left-eye pixel showing the same thing:
    //  1. the same tile pixel (by contents, sprite or background alike) at
    //     the disparity its layer had on this row last frame, or overall;
    //     else the instance on the row nearest to that (within
    //     kMaxDisparity) if what's around it looks the same too;
    //  2. otherwise the left pixel whose surroundings look the same: the
    //     kWindowWidth x kWindowHeight pixels around it, in the shades the
    //     eyes actually show - the best of the disparities this pixel had
    //     last frame, its neighbour's, the pixel above's and its layer's;
    //     if none is close, the best within kMaxDisparity (at most
    //     kSearchBudget searches a frame - the rest catch up in the next
    //     frames - and none again while last frame's match is as good);
    //  3. a tile pixel the left eye shows somewhere else, without a close
    //     match, keeps its own colors - the pack's colors for it are the
    //     left eye's too.
    // Anything without a counterpart (cut off at the screen's edge, seen by
    // one eye only) keeps its own colors.
    vbgo_tt_eye_view view[2];
    if (!vbgo_tiletrack_eye_view(0, &view[0]) || !vbgo_tiletrack_eye_view(1, &view[1]))
        return;
    // Both eyes' tags, row by row, packed to what matching needs: bit 31
    // drawn this frame, bits 0-17 what it shows (tile, pixel, sprite),
    // 18-19 the pixel's value, 20-24 the layer.
    // A tile is named by its contents, not by the character slot it sits
    // in: games keep copies of a tile in several slots and draw each eye
    // from its own (Mario Clash's score digits). The name is the first left-
    // eye slot holding that tile, or - flagged kRightOnly - the first right-
    // eye one if the left eye has no such tile.
    constexpr uint32_t kDrawn = 1u << 31, kRightOnly = 1u << 30, kWhat = (1u << 18) - 1;
    std::array<uint16_t, 2048> nameOf[2];
    {
        if (m_tileNames.empty())
            m_tileNames.resize(kTileNameSlots);
        if (++m_tileNameGeneration == 0) // (wrapped: start over)
        {
            std::fill(m_tileNames.begin(), m_tileNames.end(), TileName{});
            m_tileNameGeneration = 1;
        }
        for (unsigned eye = 0; eye < 2; ++eye)
            for (unsigned c = 0; c < 2048; ++c)
            {
                const uint32_t hash = view[eye].hashes[c];
                uint32_t i = (hash * 2654435761u) >> (32 - kTileNameBits);
                while (m_tileNames[i].generation == m_tileNameGeneration && m_tileNames[i].hash != hash)
                    i = (i + 1) & (kTileNameSlots - 1);
                TileName &n = m_tileNames[i];
                if (n.generation != m_tileNameGeneration)
                    n = {hash, static_cast<uint16_t>(eye ? 0x8000 | c : c), m_tileNameGeneration};
                nameOf[eye][c] = n.name;
            }
    }
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
                        const unsigned slot = nameOf[eye][VBGO_TAG_CHAR(t)];
                        out[y * VBGO_TT_WIDTH + x] =
                            column && (t >> 48) == stamp
                                ? kDrawn | (slot & 0x7FF) | ((slot & 0x8000) ? kRightOnly : 0) | (VBGO_TAG_INDEX(t) << 11) |
                                      (VBGO_TAG_IS_OBJ(t) << 17) | (VBGO_TAG_PIXEL(t) << 18) | (VBGO_TAG_WORLD(t) << 20)
                                : 0;
                    }
                }
    }
    // The shades both eyes show (the core's tag byte, low 2 bits), packed.
    m_eyeShades.assign(2 * VBGO_TT_HEIGHT * kShadeRowWords, 0);
    for (unsigned eye = 0; eye < 2; ++eye)
        for (int y = 0; y < VBGO_TT_HEIGHT; ++y)
        {
            uint64_t *out = &m_eyeShades[(eye * VBGO_TT_HEIGHT + y) * kShadeRowWords];
            const uint8_t *in = &raw[(static_cast<size_t>(y) * fbWidth + eyeOffset[eye]) * 4 + 3];
            for (int x = 0; x < VBGO_TT_WIDTH; ++x)
            {
                const int bit = (x + kGuardPixels) * 2;
                out[bit >> 6] |= static_cast<uint64_t>(in[x * 4] & 3) << (bit & 63);
            }
        }
    // Every tile pixel the left eye shows anywhere (bit per slot, pixel, sprite).
    std::fill(m_leftShows.begin(), m_leftShows.end(), 0);
    for (size_t i = 0; i < VBGO_TT_EYE_PIXELS; ++i)
        if (const uint32_t l = m_eyeTags[i])
            m_leftShows[(l & kWhat) >> 6] |= 1ull << (l & 63);
    std::array<std::array<uint32_t, 2 * kMaxDisparity + 1>, 32> votes{}, lookalikeVotes{};
    int searchBudget = kSearchBudget;
    ++m_eyeFrame;
    if (m_eyeCostAt.empty())
        m_eyeCostAt.assign(VBGO_TT_EYE_PIXELS, kNotSearched);
    for (int y = 0; y < VBGO_TT_HEIGHT; ++y)
    {
        const uint32_t *left = &m_eyeTags[y * VBGO_TT_WIDTH];
        const uint32_t *right = &m_eyeTags[VBGO_TT_EYE_PIXELS + y * VBGO_TT_WIDTH];
        int16_t *disparityAt = &m_eyeDisparityAt[y * VBGO_TT_WIDTH];
        uint8_t *row = &frame[static_cast<size_t>(y) * fbWidth * 4];
        auto same = [&](int xl, uint32_t r) {
            return xl >= 0 && xl < VBGO_TT_WIDTH && ((left[xl] ^ r) & (kDrawn | kRightOnly | kWhat)) == 0;
        };
        std::array<int16_t, 32> rowCandidate;
        std::array<int32_t, 32> rowCount{};
        auto take = [&](int x, int from, bool identical) {
            const unsigned world = (right[x] >> 20) & 31;
            const int d = from - x;
            ++(identical ? votes : lookalikeVotes)[world][d + kMaxDisparity];
            disparityAt[x] = static_cast<int16_t>(d);
            if (!rowCount[world])
                rowCandidate[world] = static_cast<int16_t>(d), rowCount[world] = 1;
            else
                rowCount[world] += rowCandidate[world] == d ? 1 : -1;
            std::memcpy(&row[(eyeOffset[1] + x) * 4], &row[(eyeOffset[0] + from) * 4], 3);
        };
        // Each layer's disparity on this row last frame (a layer can shift
        // every row by its own amount - Wario Land's mountains), for the rows
        // to come this frame's (whatever most of its pixels here had).
        int16_t *rowDisparity = &m_eyeRowDisparity[y * 32];
        struct CommitRow // (at the end of the row, however it ends)
        {
            std::array<int16_t, 32> &candidate;
            std::array<int32_t, 32> &count;
            int16_t *out;
            ~CommitRow()
            {
                for (unsigned w = 0; w < 32; ++w)
                    out[w] = count[w] > 0 ? candidate[w] : static_cast<int16_t>(kUnknownDisparity);
            }
        } commitRow{rowCandidate, rowCount, rowDisparity};
        const std::array<int16_t, 32> lastRow = [&] {
            std::array<int16_t, 32> a;
            std::copy(rowDisparity, rowDisparity + 32, a.begin());
            return a;
        }();
        // Mostly the same tile pixel at its layer's disparity; the rest after.
        int missed[VBGO_TT_WIDTH], misses = 0;
        for (int x = 0; x < VBGO_TT_WIDTH; ++x)
        {
            const uint32_t r = right[x];
            if (!r)
                continue;
            const unsigned world = (r >> 20) & 31;
            const int onRow = lastRow[world], predicted = m_eyeDisparity[world];
            if (onRow != kUnknownDisparity && same(x + onRow, r))
                take(x, x + onRow, true);
            else if (predicted != kUnknownDisparity && predicted != onRow && same(x + predicted, r))
                take(x, x + predicted, true);
            else
                missed[misses++] = x;
        }
        if (!misses)
            continue;
        // The windows (kWindowWidth x kWindowHeight pixels around x in the
        // right eye, around x + d in the left): how many pixels' shades
        // differ, counting up to limit.
        const int top = std::max(0, y - kWindowHeight / 2), bottom = std::min(VBGO_TT_HEIGHT - 1, y + kWindowHeight / 2);
        const uint64_t *leftShades = &m_eyeShades[0], *rightShades = &m_eyeShades[VBGO_TT_HEIGHT * kShadeRowWords];
        auto windowOff = [&](int x, int d, int limit) {
            int off = 0;
            for (int yy = top; yy <= bottom && off < limit; ++yy)
                off += ShadesOff(ShadeBits(&rightShades[yy * kShadeRowWords], x - kWindowWidth / 2, kWindowWidth),
                                 ShadeBits(&leftShades[yy * kShadeRowWords], x + d - kWindowWidth / 2, kWindowWidth));
            return off;
        };
        const uint64_t *rightRow = &rightShades[y * kShadeRowWords], *leftRow = &leftShades[y * kShadeRowWords];
        // Which tile pixels the left eye has on this row (a 4096-bit filter).
        std::array<uint64_t, 64> onThisRow{};
        auto filterBit = [](uint32_t what) { return (what * 2654435761u) >> 20; };
        for (int x = 0; x < VBGO_TT_WIDTH; ++x)
            if (left[x])
            {
                const uint32_t b = filterBit(left[x] & kWhat);
                onThisRow[b >> 6] |= 1ull << (b & 63);
            }
        // Disparities nearest to around first, within kMaxDisparity and the
        // screen; f returns true to stop.
        auto nearestFirst = [](int x, int around, auto &&f) {
            const int lowest = std::max(-kMaxDisparity, -x), highest = std::min(kMaxDisparity, VBGO_TT_WIDTH - 1 - x);
            around = std::min(std::max(around, lowest), highest);
            if (f(around))
                return;
            for (int dist = 1;; ++dist)
            {
                const int lo = around - dist, hi = around + dist;
                if (lo < lowest && hi > highest)
                    return;
                if (lo >= lowest && f(lo))
                    return;
                if (hi <= highest && f(hi))
                    return;
            }
        };
        int runDisparity = kUnknownDisparity, runEnd = -2;
        unsigned runWorld = 32;
        for (int m = 0; m < misses; ++m)
        {
            const int x = missed[m];
            const uint32_t r = right[x];
            const unsigned world = (r >> 20) & 31;
            const uint64_t shade = ShadeBits(rightRow, x, 1);
            const int candidates[5] = {disparityAt[x], runWorld == world && runEnd + 2 >= x ? runDisparity : kUnknownDisparity,
                                       y ? m_eyeDisparityAt[(y - 1) * VBGO_TT_WIDTH + x] : kUnknownDisparity,
                                       lastRow[world], m_eyeDisparity[world]};
            int around = 0;
            for (const int c : candidates)
                if (c != kUnknownDisparity)
                {
                    around = c;
                    break;
                }
            // 1. The same tile pixel elsewhere on the row: the instance
            // nearest to where its layer would put it - if what's around it
            // looks the same too (a tile used all over, in other places;
            // not asked of sprites, whose surroundings are at other depths).
            if (!(r & kRightOnly) && (onThisRow[filterBit(r & kWhat) >> 6] >> (filterBit(r & kWhat) & 63) & 1))
            {
                int found = 0;
                nearestFirst(x, around, [&](int c) {
                    if (!same(x + c, r))
                        return false;
                    if (!shade || (r & 1u << 17) || windowOff(x, c, kFairMatch + 1) <= kFairMatch)
                    {
                        take(x, x + c, true);
                        runDisparity = c, runWorld = world, runEnd = x;
                        found = -1;
                        return true;
                    }
                    return ++found == 2;
                });
                if (found < 0)
                    continue;
            }
            const bool shownElsewhere = !(r & kRightOnly) && (m_leftShows[(r & kWhat) >> 6] >> (r & 63) & 1);
            if (!shade)
            {
                disparityAt[x] = kUnknownDisparity;
                continue; // the black shade: black either way
            }
            // 2. By what both eyes show around it: the disparity it had last
            // frame, its neighbour's or the pixel above's, its layer's on
            // the row or overall - the best of them; if none is close, the
            // best within kMaxDisparity (at most kSearchBudget pixels a frame
            // search - the rest take the best of those, and get searched in
            // the frames after).
            int d = kUnknownDisparity, best = kWindowWidth * kWindowHeight + 1;
            uint8_t &lastCost = m_eyeCostAt[y * VBGO_TT_WIDTH + x];
            bool settled = false; // as good as last frame's match: no need to look further
            for (int i = 0; i < 5 && best > kCloseMatch && !settled; ++i)
            {
                const int c = candidates[i];
                if (c == kUnknownDisparity || std::abs(c) > kMaxDisparity || x + c < 0 || x + c >= VBGO_TT_WIDTH ||
                    ShadeBits(leftRow, x + c, 1) != shade)
                    continue;
                bool tried = false;
                for (int j = 0; j < i; ++j)
                    tried |= candidates[j] == c;
                if (tried)
                    continue;
                const int off = windowOff(x, c, best);
                if (off < best)
                    best = off, d = c;
                settled = i == 0 && lastCost <= kWindowWidth * kWindowHeight && best <= lastCost;
            }
            const bool search = best > kFairMatch && !settled && searchBudget > 0 &&
                                (lastCost == kNotSearched || ((x + y + static_cast<int>(m_eyeFrame)) & (kResearchEvery - 1)) == 0);
            if (search)
            {
                --searchBudget;
                int limit = best;
                nearestFirst(x, d != kUnknownDisparity ? d : around, [&](int c) {
                    if (ShadeBits(leftRow, x + c, 1) != shade)
                        return false;
                    const int off = windowOff(x, c, limit);
                    if (off < limit)
                        limit = off, d = c;
                    return limit == 0;
                });
                best = limit;
            }
            // A tile pixel the left eye shows elsewhere keeps its own colors
            // unless something looks much the same.
            if (shownElsewhere && best > kFairMatch)
                d = kUnknownDisparity;
            // Remember how good it was - unless it's a poor match nobody
            // searched past yet (searched as soon as the budget allows).
            const bool trusted = search || settled || best <= kFairMatch || lastCost == kNoMatch;
            lastCost = !trusted ? kNotSearched : d == kUnknownDisparity ? kNoMatch : static_cast<uint8_t>(best);
            if (d == kUnknownDisparity)
            {
                // 3. Nothing like it: its own colors.
                disparityAt[x] = kUnknownDisparity;
                continue;
            }
            runDisparity = d, runWorld = world, runEnd = x;
            take(x, x + d, false);
        }
    }
    // Each layer's disparity for the next frame: where most of its pixels
    // showed the same tile pixel, else where most matched by what's around
    // them (kept if it showed nothing matching this time).
    for (unsigned w = 0; w < 32; ++w)
        for (const auto *v : {&votes[w], &lookalikeVotes[w]})
        {
            uint32_t best = 0;
            for (int d = 0; d <= 2 * kMaxDisparity; ++d)
                if ((*v)[d] > best)
                    best = (*v)[d], m_eyeDisparity[w] = static_cast<int16_t>(d - kMaxDisparity);
            if (best)
                break;
        }
}
