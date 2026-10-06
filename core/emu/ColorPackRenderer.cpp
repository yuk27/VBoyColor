#include "emu/ColorPackRenderer.h"
#include "emu/vbgo_tiletrack.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

namespace
{
int Sign9(uint16_t v)
{
    v &= 0x1FF;
    return (v & 0x100) ? static_cast<int>(v) - 0x200 : static_cast<int>(v);
}

inline int CountSet(uint64_t x)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(x);
#elif defined(_MSC_VER) && defined(_M_X64)
    return static_cast<int>(__popcnt64(x));
#else
    x = x - ((x >> 1) & 0x5555555555555555ull);
    x = (x & 0x3333333333333333ull) + ((x >> 2) & 0x3333333333333333ull);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0Full;
    return static_cast<int>((x * 0x0101010101010101ull) >> 56);
#endif
}

// A row of one shade's pixels as bits (pixel x at bit x), with a guard word
// before and two after, so a shift by up to 64 pixels either way stays inside.
constexpr int kRowWords = VBGO_TT_WIDTH / 64, kGuardedRow = kRowWords + 3;

// Word k of the row shifted so that bit x holds the row's bit x + d.
inline uint64_t Shifted(const uint64_t *row, int k, int d)
{
    const int bit = 64 * (k + 1) + d, w = bit >> 6, s = bit & 63;
    return s ? (row[w] >> s) | (row[w + 1] << (64 - s)) : row[w];
}

constexpr int kMaxDisparity = 64;

// The left-picture map (see m_leftPicture) has kPad guard pixels all
// round, so a look kPad pixels away needs no bounds check.
constexpr int kPad = 3, kMapW = VBGO_TT_WIDTH + 2 * kPad, kMapH = VBGO_TT_HEIGHT + 2 * kPad;
// (dx, dy) within kPad pixels, nearest first (rows before columns on ties:
// pictures are drawn in rows), with their offset in the map - see RegionColor.
struct NearSpot
{
    int8_t dx, dy;
    int32_t offset;
};
const std::vector<NearSpot> kNearest = [] {
    std::vector<NearSpot> v;
    for (int dy = -kPad; dy <= kPad; ++dy)
        for (int dx = -kPad; dx <= kPad; ++dx)
            if (dx * dx + dy * dy <= kPad * kPad)
                v.push_back({static_cast<int8_t>(dx), static_cast<int8_t>(dy), dy * kMapW + dx});
    std::stable_sort(v.begin(), v.end(), [](const NearSpot &a, const NearSpot &b) {
        const int da = a.dx * a.dx + a.dy * a.dy, db = b.dx * b.dx + b.dy * b.dy;
        return da != db ? da < db : std::abs(a.dy) < std::abs(b.dy);
    });
    return v;
}();
inline int MapAt(int x, int y) { return (y + kPad) * kMapW + x + kPad; }
} // namespace

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
    for (auto &grid : m_markerGrid)
        grid.clear();
    for (auto &grid : m_markerLayer)
        grid.clear();
    m_layerBound = 0;
    // (pairs start over: their estimates belong to another game)
    for (Pair &pair : m_pairs)
        pair = Pair{};
    m_pairCount = 0;
    // (automatic colors start over too: the layers seen, the brightness)
    m_autoWorlds = 0;
    m_autoWorldsUsed = ~0u;
    m_autoMaxLevel = 0;
    m_figureWorlds = 0;
    m_figuresUsed = ~0u;
    m_worldExtent.fill(0);
    SetFadeReference(m_pack ? m_pack->ReferenceLevel() : 63);
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
        for (auto &grid : m_markerGrid)
            grid.assign(kGridW * kGridH, 0);
        m_layerBound = m_pack->LayerBoundGroups();
        if (m_layerBound)
            for (auto &grid : m_markerLayer)
                grid.assign(kGridW * kGridH, 0xFF);
    }
    const std::vector<TileColorPack::CellTile> &cells = m_pack->CellTiles(); // sorted by cell
    m_cellStart.assign(65537, 0);
    m_cellHashes.clear();
    for (const TileColorPack::CellTile &cell : cells)
    {
        ++m_cellStart[cell.cell + 1u];
        m_cellHashes.insert(cell.hash);
    }
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

void ColorPackRenderer::SetFadeReference(unsigned level)
{
    // Same fade as the Multicolor palette (see ShadeColorizer): the brightest
    // shade's level from the core's tag byte, through the core's gamma -
    // relative to the brightness the paintings were made at (without a
    // pack: the brightest the game has shown), so a game that normally runs
    // below full brightness shows exactly the painted colors.
    const float reference = std::pow(std::max(1u, std::min(63u, level)) / 63.0f, 1.0f / 2.2f);
    for (int i = 0; i < 64; ++i)
        m_fade[i] = static_cast<int>(std::lround(256.0f * std::min(1.0f, std::pow(static_cast<float>(i) / 63.0f, 1.0f / 2.2f) / reference)));
}

void ColorPackRenderer::UpdateAutoColors()
{
    // Each background layer's ramp by its depth in the game's drawing order
    // (higher worlds are drawn first, farther back), over the layers drawn
    // since the game started - so a layer keeps its colors. (A pair's right
    // world takes its left partner's - see ClassifyWorlds.)
    // (Figures - see m_figureWorlds - are colored like sprites instead.)
    if (m_autoWorlds == m_autoWorldsUsed && m_figureWorlds == m_figuresUsed)
        return;
    m_autoWorldsUsed = m_autoWorlds;
    m_figuresUsed = m_figureWorlds;
    const uint32_t scenery = m_autoWorlds & ~m_figureWorlds;
    int nearest = 0, farthest = 31;
    if (scenery)
    {
        while (!(scenery >> nearest & 1))
            ++nearest;
        while (!(scenery >> farthest & 1))
            --farthest;
    }
    for (int w = 0; w < 32; ++w)
        m_autoLayer[w] = AutoColors::LayerRamp(farthest > nearest ? static_cast<float>(farthest - w) / (farthest - nearest) : 0.5f);
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
        slot.cellColored = !m_cellHashes.empty() && m_cellHashes.count(slot.hash) != 0;
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
}

void ColorPackRenderer::ClassifyWorlds(const uint16_t *worlds)
{
    // Which eyes draw each world (LON/RON), its type, and the per-eye pairs:
    // a left-only world next to a right-only one of the same type, in the
    // order the VIP draws them (Mario Clash's stage, Galactic Pinball's
    // tables). A pair's right world takes its left partner's colors.
    std::array<World, 32> next{};
    for (unsigned w = 0; w < 32; ++w)
        next[w].colors = static_cast<uint8_t>(w);
    m_rightShift.fill(0);
    m_pairOfLeft.fill(-1);
    m_pairOfRight.fill(-1);
    m_spritePairOf.fill(-1);
    std::array<Pair, kMaxPairs> pairs{};
    int pairCount = 0;
    if (worlds)
    {
        int listed[32], n = 0;
        for (int w = 31; w >= 0; --w)
        {
            const uint16_t *a = &worlds[w * 16];
            if (VBGO_WORLD_END(a))
                break;
            World &world = next[w];
            world.eyes = static_cast<uint8_t>(VBGO_WORLD_LON(a) | (VBGO_WORLD_RON(a) << 1));
            world.type = static_cast<uint8_t>(VBGO_WORLD_TYPE(a));
            if (!world.eyes)
                continue;
            listed[n++] = w;
            // Both eyes: the right eye shows a map pixel 2 x (GP - MP) to the
            // right of the left eye's (affine layers: GP only).
            if (world.eyes == 3 && world.type != 3)
                m_rightShift[w] = static_cast<int16_t>(2 * (Sign9(a[2]) - (world.type == 2 ? 0 : Sign9(a[5]))));
        }
        for (int k = 0; k + 1 < n;)
        {
            const int a = listed[k], b = listed[k + 1];
            if (next[a].type == next[b].type && next[a].type != 3 && (next[a].eyes | next[b].eyes) == 3 &&
                next[a].eyes != 3 && next[b].eyes != 3 && pairCount < kMaxPairs)
            {
                const int left = next[a].eyes == 1 ? a : b, right = left == a ? b : a;
                next[left].partner = static_cast<int8_t>(right);
                next[right].partner = static_cast<int8_t>(left);
                next[right].colors = static_cast<uint8_t>(left);
                // (the same pair as last frame keeps its estimates)
                Pair &pair = pairs[pairCount];
                for (int old = 0; old < m_pairCount; ++old)
                    if (!m_pairs[old].sprites && m_pairs[old].left == left && m_pairs[old].right == right)
                    {
                        pair = std::move(m_pairs[old]);
                        m_pairs[old].left = m_pairs[old].right = 0xFF;
                        break;
                    }
                pair.left = static_cast<uint8_t>(left);
                pair.right = static_cast<uint8_t>(right);
                m_pairOfLeft[left] = static_cast<int8_t>(pairCount);
                m_pairOfRight[right] = static_cast<int8_t>(pairCount);
                ++pairCount;
                k += 2;
            }
            else
                ++k;
        }
        // Sprites only one eye shows (JLON / JRON): Galactic Pinball draws
        // much of a table as a left-eye and a right-eye set of sprites, with
        // the depth drawn into their positions (and often their tiles) - a
        // pair too, per sprite world.
        for (int k = 0; k < n && pairCount < kMaxPairs; ++k)
        {
            const int w = listed[k];
            if (next[w].type != 3)
                continue;
            Pair &pair = pairs[pairCount];
            for (int old = 0; old < m_pairCount; ++old)
                if (m_pairs[old].sprites && m_pairs[old].left == w)
                {
                    pair = std::move(m_pairs[old]);
                    m_pairs[old].left = m_pairs[old].right = 0xFF;
                    break;
                }
            pair.left = pair.right = static_cast<uint8_t>(w);
            pair.sprites = true;
            m_spritePairOf[w] = static_cast<int8_t>(pairCount);
            ++pairCount;
        }
    }
    else
        for (unsigned w = 0; w < 32; ++w)
            next[w].eyes = 3; // (a core without the world snapshot: every world counts as both eyes')
    m_worlds = next;
    m_pairs = std::move(pairs);
    m_pairCount = pairCount;
}

int ColorPackRenderer::EyeOnly(uint64_t tag) const
{
    if (!m_oam || !VBGO_TAG_IS_OBJ(tag))
        return 0;
    const unsigned flags = m_oam[VBGO_TAG_OBJ_NO(tag) * 4 + 1] & 0xC000;
    return flags == 0x8000 ? 1 : flags == 0x4000 ? 2 : 0;
}

bool ColorPackRenderer::InPicture(const Pair &pair, uint64_t tag, unsigned eye) const
{
    if (!pair.sprites)
        return !VBGO_TAG_IS_OBJ(tag) && VBGO_TAG_WORLD(tag) == (eye ? pair.right : pair.left);
    return VBGO_TAG_IS_OBJ(tag) && VBGO_TAG_WORLD(tag) == pair.left && EyeOnly(tag) == (eye ? 2 : 1);
}

int ColorPackRenderer::PairDisparity(unsigned rightWorld, unsigned band) const
{
    if (rightWorld >= 32 || band >= static_cast<unsigned>(kBands) || m_pairOfRight[rightWorld] < 0)
        return kNoDisparity;
    const Pair &pair = m_pairs[m_pairOfRight[rightWorld]];
    return pair.estimated ? pair.disparity[band] : kNoDisparity;
}

int ColorPackRenderer::LeftX(const uint64_t tag, int x, unsigned eye, unsigned y) const
{
    if (!eye)
        return x;
    if (VBGO_TAG_IS_OBJ(tag))
    {
        // A right-eye-only sprite: where its sprite pair's disparity puts it.
        const int p = m_spritePairOf[VBGO_TAG_WORLD(tag)];
        if (p >= 0 && EyeOnly(tag) == 2 && m_pairs[p].estimated && m_pairs[p].disparity[y >> 3] != kNoDisparity)
            return x + m_pairs[p].disparity[y >> 3];
        // right x = JX + JP, left x = JX - JP
        return m_oam ? x - 2 * vbgo_obj_parallax(m_oam, VBGO_TAG_OBJ_NO(tag)) : x;
    }
    const unsigned world = VBGO_TAG_WORLD(tag);
    if (m_pairOfRight[world] >= 0)
    {
        const Pair &pair = m_pairs[m_pairOfRight[world]];
        const int d = pair.estimated ? pair.disparity[y >> 3] : kNoDisparity;
        return d == kNoDisparity ? x : x + d;
    }
    return x - m_rightShift[world];
}

uint64_t ColorPackRenderer::Near(int x, int y, unsigned world) const
{
    // Markers drawn within reach over the last two frames (of a layer-bound
    // group, only ones drawn on this pixel's layer) - in left-eye terms.
    // (Sprites' groups: m_nearSprites.)
    const int cx = x >> 3, cy = y >> 3;
    uint64_t bits = 0;
    for (unsigned k = 1; k <= 2; ++k)
    {
        const unsigned g = (m_gridCurrent + k) % 3;
        const uint64_t *grid = m_markerGrid[g].data();
        const uint8_t *layers = m_layerBound ? m_markerLayer[g].data() : nullptr;
        for (int gy = std::max(0, cy - kContextReach); gy <= std::min(kGridH - 1, cy + kContextReach); ++gy)
            for (int gx = std::max(0, cx - kContextReach); gx <= std::min(kGridW - 1, cx + kContextReach); ++gx)
            {
                const int i = gy * kGridW + gx;
                const uint64_t here = grid[i];
                if (!layers)
                    bits |= here;
                else
                    bits |= (here & ~m_layerBound) | (layers[i] == world ? here & m_layerBound : 0);
            }
    }
    return bits;
}

bool ColorPackRenderer::SharedWithLeftPicture(unsigned chr, uint32_t hash)
{
    const uint32_t yes = (m_frame << 1) | 1, no = m_frame << 1;
    uint32_t &known = m_slotSharedAt[chr];
    if (known == yes || known == no)
        return known == yes;
    bool found = false;
    if (hash == 0)
        found = m_leftPairHashZero;
    else if (!m_leftPairHashes.empty())
    {
        const uint32_t mask = static_cast<uint32_t>(m_leftPairHashes.size() - 1);
        for (uint32_t i = (hash * 2654435761u) & mask; m_leftPairHashes[i]; i = (i + 1) & mask)
            if (m_leftPairHashes[i] == hash)
            {
                found = true;
                break;
            }
    }
    known = found ? yes : no;
    return found;
}

const TileColorPack::CellTile *ColorPackRenderer::FindCell(unsigned cell, unsigned palette, uint32_t hash) const
{
    const TileColorPack::CellTile *cells = m_pack->CellTiles().data();
    for (uint32_t k = m_cellStart[cell], end = m_cellStart[cell + 1]; k < end; ++k)
        if (cells[k].palette == palette && cells[k].hash == hash)
            return &cells[k];
    return nullptr;
}

const TileColorPack::CellTile *ColorPackRenderer::MappedCell(int pair, uint64_t tag, int x, int y, uint32_t hash)
{
    // A tile both pictures of a pair use, that the pack colors per map cell
    // (the left picture's cells - painted from the left eye): the left
    // picture's cell showing the same tile pixel on this row, nearest to
    // where the band's disparity puts it - once per right-picture cell and
    // frame, so a cell's pixels all go by the same one.
    const unsigned palette = VBGO_TAG_PALETTE(tag), index = VBGO_TAG_INDEX(tag);
    const uint32_t key = (VBGO_TAG_CELL(tag) << 13) | (palette << 11) | VBGO_TAG_CHAR(tag);
    const uint32_t mask = static_cast<uint32_t>(m_mapCache.size() - 1);
    uint32_t i = (key * 2654435761u) >> 20 & mask;
    while (m_mapCache[i].frame == m_frame && m_mapCache[i].key != key)
        i = (i + 1) & mask;
    MapEntry &entry = m_mapCache[i];
    if (entry.frame == m_frame)
        return entry.cell;
    const Pair &p = m_pairs[pair];
    const int d = p.estimated ? p.disparity[y >> 3] : kNoDisparity;
    const int target = x + (d == kNoDisparity ? 0 : d);
    const LeftPixel *best = nullptr;
    int bestDistance = kMaxDisparity + 1;
    for (const LeftPixel &l : m_pairRows[y])
        if (l.hash == hash && l.index == index && l.palette == palette && l.world == p.left && std::abs(l.x - target) < bestDistance)
            best = &l, bestDistance = std::abs(l.x - target);
    entry = {key, m_frame, best ? FindCell(best->cell, palette, hash) : nullptr};
    return entry.cell;
}

bool ColorPackRenderer::RegionColor(int p, int x, int y, unsigned shade, const uint8_t *frame, uint32_t fbWidth,
                                    uint32_t leftOffset, uint8_t *out)
{
    // Where the pixel's 8x8 block lines up with the left picture (the
    // block's disparity - see EstimatePair, worked out when the picture
    // changes, not every frame): the color of the nearest left-picture pixel
    // of the same shade - the very pixel it shows there if the block is the
    // left drawing shifted; on a picture drawn separately for each eye, the
    // same spot of the same object, whatever the two dithers do. Failing
    // that, by region: the most common color the left picture's pixels of
    // that shade show in that 8x8 window, or a wider one around it.
    Pair &pair = m_pairs[p];
    if (!pair.estimated)
        return false;
    const int bx = x >> 3, by = y >> 3;
    const int d = pair.blockDisparity[by * kBlocksX + bx];
    if (d == kNoDisparity)
        return false;
    // The nearest left-picture pixel of the same shade to where the block's
    // disparity puts it (the very pixel, if the block is the left drawing
    // shifted), within kNearReach.
    const int lx = x + d;
    const uint8_t want = static_cast<uint8_t>((p + 1) | (shade << 5));
    if (lx >= -kPad && lx < VBGO_TT_WIDTH + kPad)
    {
        const uint8_t *at = &m_leftPicture[MapAt(lx, y)];
        for (const NearSpot &o : kNearest)
            if (at[o.offset] == want)
            {
                std::memcpy(out, &frame[(static_cast<size_t>(y + o.dy) * fbWidth + leftOffset + lx + o.dx) * 4], 3);
                return true;
            }
    }
    Block &block = pair.blocks[by * kBlocksX + bx];
    if (block.frame != m_frame)
    {
        block.frame = m_frame;
        block.bgr[0] = block.bgr[1] = block.bgr[2] = 0;
        static constexpr int kWindows[3][4] = {{0, 8, 0, 8}, {-4, 12, 0, 8}, {-8, 16, -4, 12}}; // columns, rows (from the block)
        for (const auto &win : kWindows)
        {
            uint32_t candidate[3] = {}, count[3] = {};
            for (int cx = std::max(0, bx * 8 + d + win[0]); cx < std::min(VBGO_TT_WIDTH, bx * 8 + d + win[1]); ++cx)
                for (int cy = std::max(0, by * 8 + win[2]); cy < std::min(VBGO_TT_HEIGHT, by * 8 + win[3]); ++cy)
                {
                    const uint8_t at = m_leftPicture[MapAt(cx, cy)];
                    if ((at & 31) != p + 1)
                        continue;
                    const unsigned s = at >> 5;
                    const size_t i = (static_cast<size_t>(cy) * fbWidth + leftOffset + cx) * 4;
                    const uint32_t c = 1u << 24 | frame[i] | (frame[i + 1] << 8) | (frame[i + 2] << 16);
                    if (!count[s - 1])
                        candidate[s - 1] = c, count[s - 1] = 1;
                    else
                        count[s - 1] += candidate[s - 1] == c ? 1 : -1;
                }
            bool all = true;
            for (int k = 0; k < 3; ++k)
            {
                if (!block.bgr[k])
                    block.bgr[k] = candidate[k];
                all = all && block.bgr[k];
            }
            if (all)
                break;
        }
    }
    const uint32_t c = block.bgr[shade - 1];
    if (!c)
        return false;
    out[0] = static_cast<uint8_t>(c);
    out[1] = static_cast<uint8_t>(c >> 8);
    out[2] = static_cast<uint8_t>(c >> 16);
    return true;
}

void ColorPackRenderer::Paint(uint8_t *frame, const uint8_t *raw, uint32_t fbWidth, const uint32_t eyeOffset[2],
                              const ShadeRgb &background)
{
    const TileColorPack *pack = m_packShown ? m_pack : nullptr;
    if (!pack && !m_auto)
        return;
    vbgo_tt_eye_view view[2];
    const bool have[2] = {vbgo_tiletrack_eye_view(0, &view[0]), vbgo_tiletrack_eye_view(1, &view[1])};
    if (!have[0] && !have[1])
        return;
    ++m_frame;
    const vbgo_tt_eye_view &any = have[0] ? view[0] : view[1];
    ClassifyWorlds(any.worlds);
    m_oam = any.oam;
    if (m_auto)
        UpdateAutoColors();
    const int bg[3] = {static_cast<int>(background.b * 255.0f + 0.5f), static_cast<int>(background.g * 255.0f + 0.5f),
                       static_cast<int>(background.r * 255.0f + 0.5f)};
    const bool haveCells = pack && !m_cellStart.empty();
    const TileColorPack::ContextTile *contexts = pack ? pack->ContextTiles().data() : nullptr;
    const bool haveContexts = pack && !m_markerGrid[0].empty();
    uint64_t *markers = nullptr; // this frame's marker grid (Near reads the two before it)
    uint8_t *markerLayers = nullptr;
    if (haveContexts)
    {
        m_gridCurrent = (m_gridCurrent + 1) % 3;
        markers = m_markerGrid[m_gridCurrent].data();
        std::fill(markers, markers + kGridW * kGridH, 0);
        if (m_layerBound)
        {
            markerLayers = m_markerLayer[m_gridCurrent].data();
            std::fill(markerLayers, markerLayers + kGridW * kGridH, 0xFF);
        }
        // Sprites' groups: the last two frames' markers within reach of each
        // cell, once (rows, then columns), so a lookup is one read.
        std::vector<uint64_t> rows(kGridW * kGridH, 0);
        const uint64_t *a = m_markerGrid[(m_gridCurrent + 1) % 3].data(), *b = m_markerGrid[(m_gridCurrent + 2) % 3].data();
        for (int gy = 0; gy < kGridH; ++gy)
            for (int gx = 0; gx < kGridW; ++gx)
            {
                uint64_t bits = 0;
                for (int x = std::max(0, gx - kContextReach); x <= std::min(kGridW - 1, gx + kContextReach); ++x)
                    bits |= a[gy * kGridW + x] | b[gy * kGridW + x];
                rows[gy * kGridW + gx] = bits & ~m_layerBound;
            }
        m_nearSprites.assign(kGridW * kGridH, 0);
        for (int gy = 0; gy < kGridH; ++gy)
            for (int gx = 0; gx < kGridW; ++gx)
                for (int y = std::max(0, gy - kContextReach); y <= std::min(kGridH - 1, gy + kContextReach); ++y)
                    m_nearSprites[gy * kGridW + gx] |= rows[y * kGridW + gx];
    }
    // Pairs: the left pictures' colors per block and shade, the tiles they
    // use, their pixels of tiles colored per map cell.
    const bool pairs = pack && m_pairCount > 0;
    if (pairs)
    {
        for (int p = 0; p < m_pairCount; ++p)
        {
            if (m_pairs[p].blocks.empty())
                m_pairs[p].blocks.resize(kBlocksX * kBands);
        }
        m_leftPairHashes.assign(4096, 0);
        m_leftPairHashZero = false;
        m_leftPicture.assign(kMapW * kMapH, 0);
        for (auto &row : m_pairRows)
            row.clear();
        if (m_mapCache.empty())
            m_mapCache.resize(4096);
    }
    std::array<uint8_t, 2048> leftPairSlot{}; // slots the left pictures drew this frame
    uint32_t worldsDrawn = 0; // background layers drawn this frame (by the world whose colors they take)
    unsigned brightest = 0;
    std::array<int16_t, 32> left, right, top, bottom; // (each layer's extent this frame, both eyes)
    left.fill(VBGO_TT_WIDTH), right.fill(-1), top.fill(VBGO_TT_HEIGHT), bottom.fill(-1);

    auto write = [&](uint8_t *dst, const uint8_t *rgb, unsigned level) {
        const int fade = m_fade[level]; // 0-256
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
    };
    // The tile's colors with context (the slow path - see Run::slow).
    auto tileColors = [&](uint64_t t, unsigned eye, int x, int y, int &nearCell, uint64_t &nearBits) -> const uint8_t * {
        const unsigned chr = VBGO_TAG_CHAR(t), index = VBGO_TAG_INDEX(t), palette = VBGO_TAG_PALETTE(t);
        const unsigned world = m_worlds[VBGO_TAG_WORLD(t)].colors;
        const Slot &slot = m_slots[chr];
        const TileColorPack::Tile *tile = nullptr;
        // Context: a shared tile in an object whose marker is nearby (a
        // sprite's colors only on sprites, a background figure's only on
        // its layer) - nearby where the left eye shows this pixel.
        if (slot.contextCount && haveContexts)
        {
            const unsigned sprite = VBGO_TAG_IS_OBJ(t);
            const int lx = std::min(VBGO_TT_WIDTH - 1, std::max(0, LeftX(t, x, eye, static_cast<unsigned>(y))));
            const int cell = static_cast<int>(((y >> 3) * kGridW + (lx >> 3)) | (world << 16) | (sprite << 21));
            if (cell != nearCell)
            {
                nearCell = cell;
                nearBits = sprite ? m_nearSprites[(y >> 3) * kGridW + (lx >> 3)] : m_layerBound ? Near(lx, y, world) & m_layerBound : 0;
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
        if (!tile)
        {
            tile = slot.palette[palette];
            if (slot.layered && tile == slot.base)
                if (const TileColorPack::Tile *layer = m_pack->FindLayer(slot.hash, world))
                    tile = layer;
        }
        return tile && (tile->mask >> index & 1) ? tile->rgb[index] : nullptr;
    };
    // What a run of pixels (down a column, from one tile in one map cell, or
    // one sprite's tile) shares - looked up once for the run.
    struct Run
    {
        const TileColorPack::CellTile *cell = nullptr; // its map cell's colors (or the left partner's)
        const TileColorPack::Tile *tile = nullptr;     // the tile's colors (palette, layer or its own)
        const AutoColors::Ramp *ramp = nullptr;        // automatic colors, if on
        int ownPair = -1;                              // a pair's right picture's own tile: region colors
        uint8_t leftPicture = 0;                       // a pair's left picture: that pair + 1
        bool record = false;                           // (a left picture's tile colored per map cell: remember where)
        bool slow = false;                             // context, ambiguous or a marker: pixel by pixel
    };
    constexpr uint64_t kRunMask = 0x7FFull | (3ull << 17) | (1ull << 19) | (31ull << 22) | (1ull << 27) | (0xFFFFull << 28);

    for (unsigned eye = 0; eye < 2; ++eye)
    {
        if (!have[eye])
            continue;
        const vbgo_tt_eye_view &v = view[eye];
        if (pack)
            ResolveSlots(v.hashes);
        if (eye == 1 && pairs)
        {
            // The tiles the left pictures drew (by contents: games keep a
            // tile in a slot per eye - Mario Clash's digits).
            const uint32_t mask = static_cast<uint32_t>(m_leftPairHashes.size() - 1);
            for (unsigned c = 0; c < 2048; ++c)
                if (leftPairSlot[c])
                {
                    const uint32_t hash = view[0].hashes[c];
                    if (!hash)
                    {
                        m_leftPairHashZero = true;
                        continue;
                    }
                    uint32_t i = (hash * 2654435761u) & mask;
                    while (m_leftPairHashes[i] && m_leftPairHashes[i] != hash)
                        i = (i + 1) & mask;
                    m_leftPairHashes[i] = hash;
                }
        }
        const uint64_t stamp = v.stamp;
        // In strips of kStrip columns, row by row: the frame is stored by
        // rows, so a row of a strip is one cache line of it (the tags are by
        // columns - each column of the strip reads on down its own).
        constexpr uint32_t kStrip = 16;
        for (uint32_t x0 = 0; x0 < VBGO_TT_WIDTH; x0 += kStrip)
        {
            uint64_t runKeys[kStrip];
            Run runs[kStrip];
            int nearCells[kStrip]; // (context: the last pixel's markers nearby, by cell and layer)
            uint64_t nearBitsOf[kStrip];
            const uint64_t *columns[kStrip];
            uint32_t drawn = 0; // (columns with anything in them)
            for (uint32_t k = 0; k < kStrip; ++k)
            {
                runKeys[k] = ~0ull, nearCells[k] = -1, nearBitsOf[k] = 0;
                columns[k] = v.columns[x0 + k];
                drawn += columns[k] != nullptr;
            }
            if (!drawn)
                continue;
            const size_t stripBase = (static_cast<size_t>(eyeOffset[eye]) + x0) * 4, rowBytes = static_cast<size_t>(fbWidth) * 4;
            for (uint32_t y = 0; y < VBGO_TT_HEIGHT; ++y)
                for (uint32_t k = 0; k < kStrip; ++k)
                {
                    if (!columns[k])
                        continue;
                    const uint64_t t = columns[k][y];
                    if ((t >> 48) != stamp)
                        continue; // nothing tracked here this frame (background)
                    const uint32_t x = x0 + k;
                    uint64_t &runKey = runKeys[k];
                    Run &run = runs[k];
                    int &nearCell = nearCells[k];
                    uint64_t &nearBits = nearBitsOf[k];
                    const size_t i = stripBase + k * 4 + y * rowBytes;
                    const uint8_t *r = &raw[i];
                    const unsigned pixel = VBGO_TAG_PIXEL(t);
                    if (pixel && (r[0] | r[1] | r[2]) == 0)
                        continue; // drawn in a shade the game switched off - stays background
                    if ((t & kRunMask) != runKey)
                    {
                        // A new run: everything but the pixel inside the tile.
                        runKey = t & kRunMask;
                        run = Run{};
                        const unsigned chr = VBGO_TAG_CHAR(t), palette = VBGO_TAG_PALETTE(t), world = VBGO_TAG_WORLD(t);
                        const bool sprite = VBGO_TAG_IS_OBJ(t) != 0;
                        const unsigned colors = m_worlds[world].colors;
                        if (!sprite)
                        {
                            worldsDrawn |= 1u << colors;
                            left[colors] = std::min<int16_t>(left[colors], static_cast<int16_t>(x));
                            right[colors] = std::max<int16_t>(right[colors], static_cast<int16_t>(x));
                            top[colors] = std::min<int16_t>(top[colors], static_cast<int16_t>(y));
                            bottom[colors] = std::max<int16_t>(bottom[colors], static_cast<int16_t>(y));
                        }
                        brightest = std::max<unsigned>(brightest, r[3] >> 2);
                        if (m_auto)
                            run.ramp = sprite || (m_figureWorlds >> colors & 1) ? &AutoColors::kSpriteRamps[palette] : &m_autoLayer[colors];
                        if (pack)
                        {
                            const Slot &slot = m_slots[chr];
                            run.slow = slot.ambiguous || (markers && slot.markerBits) || (slot.contextCount && haveContexts);
                            if (haveCells && VBGO_TAG_HAS_CELL(t))
                                run.cell = FindCell(VBGO_TAG_CELL(t), palette, slot.hash);
                            if (!run.slow)
                            {
                                run.tile = slot.palette[palette];
                                if (slot.layered && run.tile == slot.base)
                                    if (const TileColorPack::Tile *layer = m_pack->FindLayer(slot.hash, colors))
                                        run.tile = layer;
                            }
                            if (pairs && sprite && m_spritePairOf[world] >= 0)
                            {
                                const int only = EyeOnly(t);
                                if (!eye && only == 1)
                                    leftPairSlot[chr] = 1, run.leftPicture = static_cast<uint8_t>(m_spritePairOf[world] + 1);
                                else if (eye && only == 2 && !SharedWithLeftPicture(chr, slot.hash))
                                {
                                    run.ownPair = m_spritePairOf[world];
                                    EstimatePair(run.ownPair, view, raw, fbWidth, eyeOffset);
                                }
                            }
                            if (pairs && !sprite)
                            {
                                if (!eye && m_pairOfLeft[world] >= 0)
                                {
                                    leftPairSlot[chr] = 1;
                                    run.leftPicture = static_cast<uint8_t>(m_pairOfLeft[world] + 1);
                                    run.record = slot.cellColored && VBGO_TAG_HAS_CELL(t);
                                }
                                else if (eye && m_pairOfRight[world] >= 0)
                                {
                                    const int p = m_pairOfRight[world];
                                    if (!SharedWithLeftPicture(chr, slot.hash))
                                    {
                                        run.ownPair = p;
                                        EstimatePair(p, view, raw, fbWidth, eyeOffset);
                                    }
                                    else if (!run.cell && slot.cellColored && VBGO_TAG_HAS_CELL(t))
                                    {
                                        EstimatePair(p, view, raw, fbWidth, eyeOffset);
                                        run.cell = MappedCell(p, t, static_cast<int>(x), static_cast<int>(y), slot.hash);
                                    }
                                }
                            }
                        }
                    }
                    const unsigned shade = r[3] & 3, index = VBGO_TAG_INDEX(t);
                    const uint8_t *rgb = nullptr;
                    if (run.leftPicture && pixel && shade)
                        m_leftPicture[MapAt(x, y)] = static_cast<uint8_t>(run.leftPicture | (shade << 5));
                    if (run.slow && pixel)
                    {
                        const unsigned chr = VBGO_TAG_CHAR(t);
                        if (m_slots[chr].ambiguous)
                            m_ambiguousPixels.emplace_back(static_cast<uint16_t>(x), static_cast<uint16_t>(y));
                        if (markers && m_slots[chr].markerBits)
                        {
                            // Sprites' groups count sprite markers, background
                            // figures' groups background ones (and remember the
                            // layer) - at the marker's left-eye spot.
                            const bool sprite = VBGO_TAG_IS_OBJ(t) != 0;
                            const uint64_t bits = m_slots[chr].markerBits & (sprite ? ~m_layerBound : m_layerBound);
                            const int lx = LeftX(t, static_cast<int>(x), eye, y);
                            if (bits && lx >= 0 && lx < VBGO_TT_WIDTH)
                            {
                                const unsigned cell = (y >> 3) * kGridW + (lx >> 3);
                                markers[cell] |= bits;
                                if (!sprite && markerLayers)
                                    markerLayers[cell] = static_cast<uint8_t>(m_worlds[VBGO_TAG_WORLD(t)].colors);
                            }
                        }
                    }
                    // 1. The map cell's own colors for this tile (and fills).
                    if (run.cell)
                    {
                        if (run.cell->keep >> index & 1)
                            goto automatic; // left uncolored here on purpose: the mode's own colors
                        if (run.cell->mask >> index & 1)
                            rgb = run.cell->rgb[index];
                    }
                    if (!rgb)
                    {
                        if (!pixel)
                            continue; // a fill nobody painted
                        // A pair's right picture, a tile of its own: the left
                        // picture's color for its shade there, as shown.
                        if (run.ownPair >= 0 && shade &&
                            RegionColor(run.ownPair, static_cast<int>(x), static_cast<int>(y), shade, frame, fbWidth, eyeOffset[0], &frame[i]))
                            continue;
                        // 2-4. Context, the palette's, the layer's, or the tile's own colors.
                        if (run.slow)
                            rgb = tileColors(t, eye, static_cast<int>(x), static_cast<int>(y), nearCell, nearBits);
                        else if (run.tile && (run.tile->mask >> index & 1))
                            rgb = run.tile->rgb[index];
                    }
                automatic:
                    if (!rgb)
                    {
                        // Unpainted: keeps the Multicolor palette's color - or,
                        // in Auto mode, its layer's or sprite palette's (see
                        // AutoColors.h); the black shade stays background.
                        if (!run.ramp || !pixel || !shade)
                            continue;
                        rgb = (*run.ramp)[shade - 1].data();
                    }
                    if (run.record && pixel)
                        m_pairRows[y].push_back({static_cast<int16_t>(x), static_cast<uint8_t>(index), static_cast<uint8_t>(VBGO_TAG_PALETTE(t)),
                                                 static_cast<uint8_t>(VBGO_TAG_WORLD(t)), static_cast<uint16_t>(VBGO_TAG_CELL(t)),
                                                 m_slots[VBGO_TAG_CHAR(t)].hash});
                    write(&frame[i], rgb, r[3] >> 2);
                }
        }
        if (!m_ambiguousPixels.empty())
            PaintAmbiguous(frame, fbWidth, eyeOffset[eye], v);
    }
    // Automatic colors: the layers this game draws, its brightness (without
    // a pack, they fade relative to the brightest it has shown).
    m_autoWorlds |= worldsDrawn;
    // Figures: layers that stay small (Mario's Tennis draws its players on
    // layers of their own, Teleroboxer its opponent's parts) - the size of
    // the largest the layer was lately (shrinking a pixel a frame).
    for (unsigned w = 0; w < 32; ++w)
        if (right[w] >= 0)
        {
            const int size = std::max(right[w] - left[w], bottom[w] - top[w] + 7) + 1; // (runs: their first rows)
            m_worldExtent[w] = static_cast<int16_t>(std::max(size, m_worldExtent[w] - 1));
            if (m_worldExtent[w] <= kFigureSize)
                m_figureWorlds |= 1u << w;
            else
                m_figureWorlds &= ~(1u << w);
        }
    if (!m_pack && brightest > m_autoMaxLevel)
    {
        m_autoMaxLevel = brightest;
        SetFadeReference(brightest);
    }
}

void ColorPackRenderer::EstimatePair(int p, const vbgo_tt_eye_view view[2], const uint8_t *raw, uint32_t fbWidth,
                                     const uint32_t eyeOffset[2])
{
    // Per 8-row band: the shift that lines up the most right-picture pixels
    // with left-picture pixels of the same shade (within kMaxDisparity).
    // Kept while the pair's registers stay put; while both worlds scroll
    // together, refined around the last estimate every few frames; anything
    // else searches again.
    Pair &pair = m_pairs[p];
    if (pair.checkedAt == m_frame)
        return;
    pair.checkedAt = m_frame;
    const uint16_t *worlds = view[0].worlds ? view[0].worlds : view[1].worlds;
    if (!worlds)
        return;
    std::array<uint16_t, 32> attributes;
    std::copy(&worlds[pair.left * 16], &worlds[pair.left * 16] + 16, attributes.begin());
    std::copy(&worlds[pair.right * 16], &worlds[pair.right * 16] + 16, attributes.begin() + 16);
    const uint32_t age = m_frame - pair.estimatedAt;
    if (pair.estimated && attributes == pair.attributes && age < 64)
        return;
    bool scrolled = pair.estimated;
    for (int f = 0; f < 16 && scrolled; ++f)
        scrolled = f >= 1 && f <= 6 ? static_cast<uint16_t>(attributes[16 + f] - attributes[f]) ==
                                          static_cast<uint16_t>(pair.attributes[16 + f] - pair.attributes[f])
                                    : attributes[f] == pair.attributes[f] && attributes[16 + f] == pair.attributes[16 + f];
    if (scrolled && age < 4)
        return;
    pair.attributes = attributes;
    // Both pictures' pixels, by row: per eye, per row, per shade 1-3, a guarded bit row.
    const size_t eyeRows = static_cast<size_t>(VBGO_TT_HEIGHT) * 3 * kGuardedRow;
    m_estimateBits.assign(2 * eyeRows, 0);
    uint64_t *bits = m_estimateBits.data();
    for (unsigned eye = 0; eye < 2; ++eye)
    {
        for (int x = 0; x < VBGO_TT_WIDTH; ++x)
        {
            const uint64_t *column = view[eye].columns[x];
            if (!column)
                continue;
            for (int y = 0; y < VBGO_TT_HEIGHT; ++y)
            {
                const uint64_t t = column[y];
                if ((t >> 48) != view[eye].stamp || !VBGO_TAG_PIXEL(t) || !InPicture(pair, t, eye))
                    continue;
                const unsigned shade = raw[(static_cast<size_t>(y) * fbWidth + eyeOffset[eye] + x) * 4 + 3] & 3;
                if (shade)
                    bits[eye * eyeRows + (static_cast<size_t>(y) * 3 + shade - 1) * kGuardedRow + 1 + (x >> 6)] |= 1ull << (x & 63);
            }
        }
    }
    const uint64_t *leftBits = bits, *rightBits = bits + eyeRows;
    std::array<int16_t, kBands> found;
    found.fill(kNoDisparity);
    for (int band = 0; band < kBands; ++band)
    {
        int lit = 0;
        for (int y = band * 8; y < band * 8 + 8; ++y)
            for (int s = 0; s < 3; ++s)
                for (int w = 0; w < kRowWords; ++w)
                    lit += CountSet(rightBits[(y * 3 + s) * kGuardedRow + 1 + w]);
        if (lit < 16)
            continue;
        const int previous = pair.estimated ? pair.disparity[band] : kNoDisparity;
        const int around = previous != kNoDisparity ? previous : 0;
        // How many pixels line up at d (rows of the band, every step-th).
        auto same = [&](int d, int step) {
            int n = 0;
            for (int y = band * 8; y < band * 8 + 8; y += step)
                for (int s = 0; s < 3; ++s)
                {
                    const uint64_t *r = &rightBits[(y * 3 + s) * kGuardedRow];
                    const uint64_t *l = &leftBits[(y * 3 + s) * kGuardedRow];
                    for (int w = 0; w < kRowWords; ++w)
                        n += CountSet(r[1 + w] & Shifted(l, w, d));
                }
            return n;
        };
        // (ties: the one nearer the last estimate, else nearer 0)
        auto search = [&](int from, int to, int stride, int step, int &bestD) {
            int best = -1;
            for (int d = std::max(-kMaxDisparity, from); d <= std::min(kMaxDisparity, to); d += stride)
            {
                const int n = same(d, step);
                if (n > best || (n == best && std::abs(d - around) < std::abs(bestD - around)))
                    best = n, bestD = d;
            }
            return best;
        };
        int bestD = around, best;
        if (scrolled && previous != kNoDisparity)
            best = search(previous - 4, previous + 4, 1, 1, bestD); // (scrolled: near the last one)
        else
        {
            // Coarse (every other shift, every other row), then fine around it.
            search(-kMaxDisparity, kMaxDisparity, 2, 2, bestD);
            const int coarse = bestD;
            best = search(coarse - 2, coarse + 2, 1, 1, bestD);
        }
        if (best > 0)
            found[band] = static_cast<int16_t>(bestD);
    }
    // Bands without enough to go on: the nearest band's.
    for (int band = 0; band < kBands; ++band)
    {
        if (found[band] != kNoDisparity)
        {
            pair.disparity[band] = found[band];
            continue;
        }
        int16_t d = kNoDisparity;
        for (int reach = 1; reach < kBands && d == kNoDisparity; ++reach)
            for (const int b : {band - reach, band + reach})
                if (b >= 0 && b < kBands && found[b] != kNoDisparity)
                {
                    d = found[b];
                    break;
                }
        pair.disparity[band] = d;
    }
    // Per 8x8 block of the right picture: the shift within reach of its
    // band's that lines up the most of its pixels (a 16-pixel-wide window
    // around it, its rows) - pictures have depth within a band (Wario
    // Land's title: Wario's head before his plane's nose) - then each block
    // the median of itself and its neighbours (no lone outliers).
    std::vector<int16_t> blocks(kBlocksX * kBands, static_cast<int16_t>(kNoDisparity));
    std::vector<uint8_t> exact(kBlocksX * kBands, 0);
    const bool keepBlocks = scrolled && pair.blockDisparity.size() == blocks.size();
    for (int by = 0; by < kBands; ++by)
    {
        const int band = pair.disparity[by];
        if (band == kNoDisparity)
            continue;
        for (int bx = 0; bx < kBlocksX; ++bx)
        {
            const int w = bx >> 3, shift = (bx & 7) * 8;
            int lit = 0;
            for (int y = by * 8; y < by * 8 + 8; ++y)
                for (int s = 0; s < 3; ++s)
                    lit += CountSet((rightBits[(y * 3 + s) * kGuardedRow + 1 + w] >> shift) & 0xFF);
            if (lit < 6)
                continue;
            const int previous = keepBlocks ? pair.blockDisparity[by * kBlocksX + bx] : kNoDisparity;
            const int center = previous != kNoDisparity ? previous : band, reach = previous != kNoDisparity ? 3 : 10;
            const int x0 = std::max(0, bx * 8 - 4), x1 = std::min(VBGO_TT_WIDTH, bx * 8 + 12);
            auto maskOf = [&](int k) {
                const int lo = std::max(x0 - 64 * k, 0), hi = std::min(x1 - 64 * k, 64);
                return (hi >= 64 ? ~0ull : (1ull << hi) - 1) & ~((1ull << lo) - 1);
            };
            int windowLit = 0;
            for (int y = by * 8; y < by * 8 + 8; ++y)
                for (int s = 0; s < 3; ++s)
                    for (int k = x0 >> 6; k <= (x1 - 1) >> 6; ++k)
                        windowLit += CountSet(rightBits[(y * 3 + s) * kGuardedRow + 1 + k] & maskOf(k));
            int best = -1, bestD = center;
            for (int d = std::max(-kMaxDisparity, center - reach); d <= std::min(kMaxDisparity, center + reach); ++d)
            {
                int n = 0;
                for (int y = by * 8; y < by * 8 + 8; ++y)
                    for (int s = 0; s < 3; ++s)
                    {
                        const uint64_t *r = &rightBits[(y * 3 + s) * kGuardedRow];
                        const uint64_t *l = &leftBits[(y * 3 + s) * kGuardedRow];
                        for (int k = x0 >> 6; k <= (x1 - 1) >> 6; ++k)
                            n += CountSet(r[1 + k] & maskOf(k) & Shifted(l, k, d));
                    }
                if (n > best || (n == best && std::abs(d - band) < std::abs(bestD - band)))
                    best = n, bestD = d;
            }
            if (best > 0)
                blocks[by * kBlocksX + bx] = static_cast<int16_t>(bestD);
            // (nearly every pixel lines up: the same drawing, shifted - its pixels correspond exactly)
            exact[by * kBlocksX + bx] = best * 10 >= windowLit * 9;
        }
    }
    pair.blockDisparity.assign(blocks.size(), static_cast<int16_t>(kNoDisparity));
    for (int by = 0; by < kBands; ++by)
        for (int bx = 0; bx < kBlocksX; ++bx)
        {
            int values[9], n = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const int yy = by + dy, xx = bx + dx;
                    if (yy >= 0 && yy < kBands && xx >= 0 && xx < kBlocksX && blocks[yy * kBlocksX + xx] != kNoDisparity)
                        values[n++] = blocks[yy * kBlocksX + xx];
                }
            if (exact[by * kBlocksX + bx])
                pair.blockDisparity[by * kBlocksX + bx] = blocks[by * kBlocksX + bx]; // (as found)
            else if (blocks[by * kBlocksX + bx] != kNoDisparity && n)
            {
                std::nth_element(values, values + n / 2, values + n);
                pair.blockDisparity[by * kBlocksX + bx] = static_cast<int16_t>(values[n / 2]);
            }
            else
                pair.blockDisparity[by * kBlocksX + bx] = pair.disparity[by]; // (few pixels: its band's)
        }
    pair.estimated = true;
    pair.estimatedAt = m_frame;
}

void ColorPackRenderer::PaintAmbiguous(uint8_t *frame, uint32_t fbWidth, uint32_t eyeOffset, const vbgo_tt_eye_view &view)
{
    // A tile a character paints two ways in one frame (a plain filled tile:
    // the shirt's white, the cap's green) takes, pixel by pixel, the color
    // of the nearest pixel of the same shade on its layer that isn't such a
    // tile - the shirt's or the cap's around it (within 8 pixels, straight
    // up, down, left or right). Without one it keeps what the pack gave it.
    // Only what both eyes show the same way around it counts: on a layer,
    // its own layer; among sprites, sprites at the same depth (parallax) -
    // their pixels sit the same way around it in both eyes.
    auto tagAt = [&](int x, int y, uint64_t &t) {
        if (x < 0 || x >= VBGO_TT_WIDTH || y < 0 || y >= VBGO_TT_HEIGHT || !view.columns[x])
            return false;
        t = view.columns[x][y];
        return (t >> 48) == view.stamp;
    };
    auto depth = [&](uint64_t t) { return m_oam && VBGO_TAG_IS_OBJ(t) ? vbgo_obj_parallax(m_oam, VBGO_TAG_OBJ_NO(t)) : 0; };
    static constexpr int kDirections[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
    for (const auto &p : m_ambiguousPixels)
    {
        const int x = p.first, y = p.second;
        uint64_t t = 0, n = 0;
        if (!tagAt(x, y, t))
            continue;
        const int jp = depth(t);
        int from = -1;
        for (int d = 1; d <= 8 && from < 0; ++d)
            for (const auto &dir : kDirections)
            {
                const int nx = x + dir[0] * d, ny = y + dir[1] * d;
                if (tagAt(nx, ny, n) && VBGO_TAG_PIXEL(n) == VBGO_TAG_PIXEL(t) && VBGO_TAG_WORLD(n) == VBGO_TAG_WORLD(t) &&
                    VBGO_TAG_IS_OBJ(n) == VBGO_TAG_IS_OBJ(t) && !m_slots[VBGO_TAG_CHAR(n)].ambiguous && depth(n) == jp)
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
