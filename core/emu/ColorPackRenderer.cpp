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

// RegionColor looks for a left-picture pixel within kNear pixels of a spot
// up to kNear pixels off the frame: the left-picture map (see m_leftPicture)
// has kPad guard pixels all round, so that needs no bounds check.
constexpr int kNear = 3, kPad = 2 * kNear, kMapW = VBGO_TT_WIDTH + 2 * kPad, kMapH = VBGO_TT_HEIGHT + 2 * kPad;
// (dx, dy) within kNear pixels, nearest first (rows before columns on ties:
// pictures are drawn in rows), with their offset in the map - see RegionColor.
struct NearSpot
{
    int8_t dx, dy;
    int32_t offset;
};
const std::vector<NearSpot> kNearest = [] {
    std::vector<NearSpot> v;
    for (int dy = -kNear; dy <= kNear; ++dy)
        for (int dx = -kNear; dx <= kNear; ++dx)
            if (dx * dx + dy * dy <= kNear * kNear)
                v.push_back({static_cast<int8_t>(dx), static_cast<int8_t>(dy), dy * kMapW + dx});
    std::stable_sort(v.begin(), v.end(), [](const NearSpot &a, const NearSpot &b) {
        const int da = a.dx * a.dx + a.dy * a.dy, db = b.dx * b.dx + b.dy * b.dy;
        return da != db ? da < db : std::abs(a.dy) < std::abs(b.dy);
    });
    return v;
}();
inline int MapAt(int x, int y) { return (y + kPad) * kMapW + x + kPad; }

const TileColorPack::Tile kNoTile{}; // (no tile colors: nothing painted)
} // namespace

void ColorPackRenderer::SetPack(const TileColorPack *pack)
{
    m_depth.Reset();
    m_pack = pack && !pack->Empty() ? pack : nullptr;
    m_cellStart.clear();
    m_fillCells.clear();
    m_layered.clear();
    m_markerBits.clear();
    m_contextRange.clear();
    for (Slot &slot : m_slots)
        slot = Slot{};
    m_slotLayer.fill(SlotLayer{});
    for (auto &grid : m_markerGrid)
        grid.clear();
    for (auto &grid : m_markerLayer)
        grid.clear();
    m_layerBound = ContextGroupBits{};
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
    for (size_t g = 0; g < groups.size() && g < TileColorPack::kMaxContextGroups; ++g)
        for (const uint32_t marker : groups[g])
            m_markerBits[marker].Set(g);
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
            grid.assign(kGridW * kGridH, ContextGroupBits{});
        m_layerBound = m_pack->LayerBoundGroups();
        if (m_layerBound.Any())
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
        m_slotLayer[c].world = 0xFF;
        slot.ambiguous = m_pack->IsAmbiguous(slot.hash);
        slot.cellColored = !m_cellHashes.empty() && m_cellHashes.count(slot.hash) != 0;
        slot.rightPainted = m_pack->RightEyePainted(slot.hash);
        slot.markerBits = ContextGroupBits{};
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
    // left-only worlds next to right-only ones of the same type, in the
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
        // A run of left-only worlds next to a run of right-only ones (same
        // type) pair up in order, one by one: Mario Clash alternates them,
        // Galactic Pinball's Cosmic table draws three left worlds, then
        // their three right ones. A left run's extra worlds join its last
        // pair - the right world draws what they do together (the Alien
        // table: three left worlds, one right); a right run's extra worlds
        // may pair with what follows.
        auto addPair = [&](int left, int right, uint32_t leftWorlds) {
            next[right].partner = static_cast<int8_t>(left);
            next[right].colors = static_cast<uint8_t>(left);
            for (int w = 0; w < 32; ++w)
                if (leftWorlds >> w & 1)
                {
                    next[w].partner = static_cast<int8_t>(right);
                    m_pairOfLeft[w] = static_cast<int8_t>(pairCount);
                }
            // (the same pair as last frame keeps its estimates)
            Pair &pair = pairs[pairCount];
            for (int old = 0; old < m_pairCount; ++old)
                if (!m_pairs[old].sprites && m_pairs[old].left == left && m_pairs[old].right == right)
                {
                    pair = std::move(m_pairs[old]);
                    m_pairs[old].left = m_pairs[old].right = 0xFF;
                    break;
                }
            if (pair.leftWorlds != leftWorlds)
                pair.estimated = false; // (another picture)
            pair.left = static_cast<uint8_t>(left);
            pair.right = static_cast<uint8_t>(right);
            pair.leftWorlds = leftWorlds;
            m_pairOfRight[right] = static_cast<int8_t>(pairCount);
            ++pairCount;
        };
        for (int k = 0; k < n;)
        {
            const World &first = next[listed[k]];
            if (first.eyes == 3 || first.type == 3)
            {
                ++k;
                continue;
            }
            int e1 = k + 1, e2;
            while (e1 < n && next[listed[e1]].eyes == first.eyes && next[listed[e1]].type == first.type)
                ++e1;
            e2 = e1;
            while (e2 < n && next[listed[e2]].eyes == (first.eyes ^ 3) && next[listed[e2]].type == first.type)
                ++e2;
            const int firstCount = e1 - k, secondCount = e2 - e1, count = std::min(firstCount, secondCount);
            const bool leftFirst = first.eyes == 1;
            for (int i = 0; i < count && pairCount < kMaxPairs; ++i)
            {
                const int left = leftFirst ? listed[k + i] : listed[e1 + i], right = leftFirst ? listed[e1 + i] : listed[k + i];
                uint32_t leftWorlds = 1u << left;
                if (leftFirst && i + 1 == count)
                    for (int j = k + count; j < e1; ++j)
                        leftWorlds |= 1u << listed[j];
                addPair(left, right, leftWorlds);
            }
            k = !count ? e1 : secondCount > firstCount ? e1 + count : e2;
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
        return !VBGO_TAG_IS_OBJ(tag) && (eye ? VBGO_TAG_WORLD(tag) == pair.right : (pair.leftWorlds >> VBGO_TAG_WORLD(tag) & 1) != 0);
    return VBGO_TAG_IS_OBJ(tag) && VBGO_TAG_WORLD(tag) == pair.left && EyeOnly(tag) == (eye ? 2 : 1);
}

int ColorPackRenderer::PairDisparity(unsigned rightWorld, unsigned band) const
{
    if (rightWorld >= 32 || band >= static_cast<unsigned>(kBands) || m_pairOfRight[rightWorld] < 0)
        return kNoDisparity;
    const Pair &pair = m_pairs[m_pairOfRight[rightWorld]];
    return pair.estimated ? pair.disparity[band] : kNoDisparity;
}

int ColorPackRenderer::PairBlockDisparity(unsigned rightWorld, unsigned bx, unsigned by, bool sprites) const
{
    if (rightWorld >= 32 || bx >= static_cast<unsigned>(kBlocksX) || by >= static_cast<unsigned>(kBands))
        return kNoDisparity;
    const int p = sprites ? m_spritePairOf[rightWorld] : m_pairOfRight[rightWorld];
    if (p < 0 || !m_pairs[p].estimated || m_pairs[p].blockDisparity.empty())
        return kNoDisparity;
    return m_pairs[p].blockDisparity[by * kBlocksX + bx];
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

void ColorPackRenderer::Near(int x, int y, unsigned world, ContextGroupBits &near, ContextGroupBits &close)
{
    // Markers drawn within reach this frame (as gathered before coloring
    // it) and over the last two (of a layer-bound group, only ones drawn on
    // this pixel's layer) - in left-eye terms; kept per grid cell and layer
    // for the frame. (Sprites' groups: m_nearSprites.)
    const int cx = x >> 3, cy = y >> 3;
    std::vector<NearCell> &cache = m_nearCache[world & 31];
    if (cache.empty())
        cache.resize(kGridW * kGridH);
    NearCell &known = cache[cy * kGridW + cx];
    if (known.frame != m_frame)
    {
        ContextGroupBits bits, closeBits;
        const ContextGroupBits free = ~m_layerBound;
        for (unsigned k = 0; k <= 2; ++k)
        {
            const unsigned g = (m_gridCurrent + k) % 3;
            const ContextGroupBits *grid = m_markerGrid[g].data();
            const uint8_t *layers = m_layerBound.Any() ? m_markerLayer[g].data() : nullptr;
            for (int gy = std::max(0, cy - kContextReach); gy <= std::min(kGridH - 1, cy + kContextReach); ++gy)
                for (int gx = std::max(0, cx - kContextReach); gx <= std::min(kGridW - 1, cx + kContextReach); ++gx)
                {
                    const int i = gy * kGridW + gx;
                    const ContextGroupBits &here = grid[i];
                    if (!here.Any())
                        continue;
                    const ContextGroupBits found = !layers ? here : layers[i] == world ? here : here & free;
                    bits |= found;
                    if (std::abs(gy - cy) <= 1 && std::abs(gx - cx) <= 1)
                        closeBits |= found;
                }
        }
        known.frame = m_frame, known.bits = bits, known.close = closeBits;
    }
    near = known.bits, close = known.close;
}

bool ColorPackRenderer::SharedWithLeftPicture(unsigned chr, uint32_t hash, int pair)
{
    // (per slot and frame: the pairs whose left pictures draw its tile)
    if (m_slotSharedAt[chr] != m_frame)
    {
        m_slotSharedAt[chr] = m_frame;
        uint32_t pairs = 0;
        if (hash == 0)
            pairs = m_leftPairHashZero;
        else if (!m_leftPairHashes.empty())
        {
            const uint32_t mask = static_cast<uint32_t>(m_leftPairHashes.size() - 1);
            for (uint32_t i = (hash * 2654435761u) & mask; m_leftPairHashes[i]; i = (i + 1) & mask)
                if (m_leftPairHashes[i] == hash)
                {
                    pairs = m_leftPairMasks[i];
                    break;
                }
        }
        m_slotShared[chr] = pairs;
    }
    return m_slotShared[chr] >> pair & 1;
}

const TileColorPack::Tile *ColorPackRenderer::LayerTile(unsigned chr, unsigned world)
{
    // (a tile is mostly drawn on one layer: the last one's colors are kept)
    SlotLayer &layer = m_slotLayer[chr];
    if (layer.world != world)
    {
        layer.world = static_cast<uint8_t>(world);
        layer.tile = m_pack->FindLayer(m_slots[chr].hash, world);
    }
    return layer.tile;
}

ColorPackRenderer::LeftTileCell *ColorPackRenderer::FindLeftTileCell(uint32_t hash, unsigned palette, unsigned pair, bool add)
{
    // (open addressing, entries of earlier frames count as free)
    if (m_leftTileCells.empty())
        m_leftTileCells.resize(1024);
    const uint64_t key = 1ull << 40 | static_cast<uint64_t>(pair) << 34 | static_cast<uint64_t>(palette) << 32 | hash;
    const size_t mask = m_leftTileCells.size() - 1;
    for (size_t i = (key * 0x9E3779B97F4A7C15ull) >> 54 & mask, probes = 0; probes <= mask; i = (i + 1) & mask, ++probes)
    {
        LeftTileCell &e = m_leftTileCells[i];
        if (e.frame != m_frame)
        {
            if (!add)
                return nullptr;
            e = {key, 0, 0}; // (set by the caller)
            return &e;
        }
        if (e.key == key)
            return &e;
    }
    return nullptr;
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
    // picture's cell showing the same tile where the band's disparity puts
    // it - once per right-picture cell and frame, so a cell's pixels
    // all go by the same one. The same tile pixel on the row, nearest that
    // spot; if the left eye doesn't show it (Mario Clash's score digits sit
    // in front, at another depth, hiding other pixels in each eye), any
    // other pixel of the tile's row - or rows below - where it says the tile
    // is (its column in the tile from its left edge; flipped, its right).
    const unsigned palette = VBGO_TAG_PALETTE(tag), index = VBGO_TAG_INDEX(tag), subX = index & 7, subY = index >> 3;
    const uint32_t key = (VBGO_TAG_CELL(tag) << 13) | (palette << 11) | VBGO_TAG_CHAR(tag);
    const uint32_t mask = static_cast<uint32_t>(m_mapCache.size() - 1);
    uint32_t i = (key * 2654435761u) >> 20 & mask;
    while (m_mapCache[i].frame == m_frame && m_mapCache[i].key != key)
        i = (i + 1) & mask;
    MapEntry &entry = m_mapCache[i];
    if (entry.frame == m_frame)
        return entry.cell;
    // (the band's disparity, not the block's: blocks are found by shades,
    // for region colors - a tile's spot is better told by the band, and the
    // tile itself: Teleroboxer's opponent's eyes go by the wrong rim cell
    // otherwise)
    const Pair &p = m_pairs[pair];
    const int d = p.estimated && p.disparity[y >> 3] != kNoDisparity ? p.disparity[y >> 3] : 0;
    const int target = x + d;
    // The same tile pixel on the row, the nearest within reach of that spot.
    auto samePixel = [&](int reach) {
        const LeftPixel *best = nullptr;
        int bestOff = reach + 1;
        for (const LeftPixel &l : m_pairRows[y])
            if (l.hash == hash && l.index == index && l.palette == palette && (p.leftWorlds >> l.world & 1) && std::abs(l.x - target) < bestOff)
                best = &l, bestOff = std::abs(l.x - target);
        return best;
    };
    // Any pixel of the tile's row (or rows below) that says the tile is within reach.
    auto sameTile = [&](int reach) {
        const LeftPixel *best = nullptr;
        int bestOff = reach + 1;
        for (unsigned dy = 0; dy + subY < 8 && y + static_cast<int>(dy) < VBGO_TT_HEIGHT; ++dy)
            for (const LeftPixel &l : m_pairRows[y + dy])
            {
                if (l.hash != hash || l.palette != palette || !(p.leftWorlds >> l.world & 1) || (l.index >> 3) != subY + dy)
                    continue;
                const int lsub = l.index & 7;
                const int off = std::min(std::abs(l.x - lsub - (target - static_cast<int>(subX))),
                                         std::abs(l.x + lsub - (target + static_cast<int>(subX))));
                if (off < bestOff)
                    best = &l, bestOff = off;
            }
        return best;
    };
    // Close to the spot first (the very pixel, else the tile where its
    // pixel is hidden); then - the tile's own copy can be well off what the
    // shades say (Teleroboxer's opponent) - the same pixel further out, the
    // tile further out.
    const LeftPixel *best = samePixel(kMapClose);
    if (!best)
        best = sameTile(kMapClose);
    if (!best)
        best = samePixel(kMaxDisparity);
    if (!best)
        best = sameTile(kMapReach);
    // Not near: the two pictures use the tile in different places (Mario's
    // Tennis's clouds at the screen's edge, where the left eye shows none;
    // Galactic Pinball's title letters) - its colors where the left
    // picture shows it at all (the first such cell from the top), if it does.
    if (!best)
        if (const LeftTileCell *any = FindLeftTileCell(hash, palette, static_cast<unsigned>(pair), false))
        {
            entry = {key, m_frame, FindCell(any->cell, palette, hash)};
            return entry.cell;
        }
    entry = {key, m_frame, best ? FindCell(best->cell, palette, hash) : nullptr};
    return entry.cell;
}

bool ColorPackRenderer::RegionColor(int p, int x, int y, unsigned shade, const uint8_t *frame, uint32_t fbWidth,
                                    uint32_t leftOffset, uint8_t *out)
{
    // Where the pixel's 8x8 block lines up with the left picture (the
    // block's disparity - see EstimatePairs, worked out when the picture
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
    // shifted), within kNear.
    const int lx = x + d;
    const uint8_t want = static_cast<uint8_t>((p + 1) | (shade << 5));
    if (lx >= -kNear && lx < VBGO_TT_WIDTH + kNear)
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

ColorPackRenderer::Run ColorPackRenderer::SetUpRun(uint64_t t, unsigned eye, int x, int y, const RunSetUp &c)
{
    // A new run: everything but the pixel inside the tile (see Run).
    Run run;
    const unsigned chr = VBGO_TAG_CHAR(t), palette = VBGO_TAG_PALETTE(t), world = VBGO_TAG_WORLD(t);
    const bool sprite = VBGO_TAG_IS_OBJ(t) != 0;
    const unsigned colors = m_worlds[world].colors;
    if (m_auto)
        run.ramp = sprite || (m_figureWorlds >> colors & 1) ? &AutoColors::kSpriteRamps[palette] : &m_autoLayer[colors];
    if (c.pack)
    {
        const Slot &slot = m_slots[chr];
        // A sprite drawn in a palette that shows all its shades alike is a
        // silhouette - the game flashing a hit enemy (or boss), a blinking
        // player - which painted colors alone would hide: they're washed
        // toward that shade (see Paint), so the flash still shows.
        run.flash = sprite && (c.flatObjPalettes >> palette & 1);
        run.slow = slot.ambiguous || (c.markers && slot.markerBits.Any()) || (slot.contextCount && c.haveContexts);
        if (c.haveCells && VBGO_TAG_HAS_CELL(t))
            run.cell = FindCell(VBGO_TAG_CELL(t), palette, slot.hash);
        if (!run.slow)
        {
            run.tile = slot.palette[palette];
            if (slot.layered && run.tile == slot.base)
                if (const TileColorPack::Tile *layer = LayerTile(chr, colors))
                    run.tile = layer;
        }
        if (c.pairs && sprite && m_spritePairOf[world] >= 0)
        {
            const int only = EyeOnly(t);
            if (!eye && only == 1)
                c.leftPairSlot[chr] |= 1u << m_spritePairOf[world], run.leftPicture = static_cast<uint8_t>(m_spritePairOf[world] + 1);
            else if (eye && only == 2)
            {
                if (!SharedWithLeftPicture(chr, slot.hash, m_spritePairOf[world]))
                {
                    run.ownPair = m_spritePairOf[world];
                    run.rightPainted = slot.rightPainted;
                    m_pairs[run.ownPair].wantedAt = m_frame;
                }
                else
                    run.copyPair = m_spritePairOf[world];
            }
        }
        if (c.pairs && !sprite)
        {
            if (!eye && m_pairOfLeft[world] >= 0)
            {
                c.leftPairSlot[chr] |= 1u << m_pairOfLeft[world];
                run.leftPicture = static_cast<uint8_t>(m_pairOfLeft[world] + 1);
                run.record = slot.cellColored && VBGO_TAG_HAS_CELL(t);
                if (run.record)
                    if (LeftTileCell *first = FindLeftTileCell(slot.hash, palette, static_cast<unsigned>(m_pairOfLeft[world]), true))
                        if (first->frame != m_frame)
                            *first = {first->key, m_frame, static_cast<uint16_t>(VBGO_TAG_CELL(t))};
            }
            else if (eye && m_pairOfRight[world] >= 0)
            {
                const int p = m_pairOfRight[world];
                if (!SharedWithLeftPicture(chr, slot.hash, p))
                {
                    run.ownPair = p;
                    m_pairs[p].wantedAt = m_frame;
                }
                else
                {
                    run.copyPair = p;
                    // (a tile the left picture draws too goes by the left picture's cells, never
                    // its own cell's colors: right-eye paintings only paint the right picture's
                    // own tiles, so those are some other screen's that used this map cell -
                    // Galactic Pinball's tables share their maps' memory)
                    if (slot.cellColored && VBGO_TAG_HAS_CELL(t))
                        run.cell = MappedCell(p, t, static_cast<int>(x), static_cast<int>(y), slot.hash);
                }
            }
        }
    }
    if (!run.tile)
        run.tile = &kNoTile;
    const int pairOf = run.ownPair >= 0 ? run.ownPair : run.copyPair;
    if (pairOf >= 0 && m_pairs[pairOf].estimated)
    {
        run.blockDisparity = m_pairs[pairOf].blockDisparity.data();
        run.blockExact = m_pairs[pairOf].blockExact.data();
        m_pairs[pairOf].wantedAt = m_frame;
    }
    else
        run.copyPair = -1;
    run.extra = run.leftPicture || run.ownPair >= 0 || run.record || run.slow;
    return run;
}

void ColorPackRenderer::Paint(uint8_t *frame, const uint8_t *raw, uint32_t fbWidth, const uint32_t eyeOffset[2],
                              const ShadeRgb &background)
{
    const TileColorPack *pack = m_packShown ? m_pack : nullptr;
    if (!pack && !m_auto)
        return;
    vbgo_tt_eye_view view[2];
    const bool have[2] = {vbgo_tiletrack_eye_view(0, &view[0]), vbgo_tiletrack_eye_view(1, &view[1])};
    const int bg[3] = {static_cast<int>(background.b * 255.0f + 0.5f), static_cast<int>(background.g * 255.0f + 0.5f),
                       static_cast<int>(background.r * 255.0f + 0.5f)};
    if (!have[0] && !have[1])
    {
        // (nothing drawn from tiles at all - but the game's CPU may have drawn)
        if (m_auto)
            PaintDepth(frame, raw, fbWidth, eyeOffset, view, have, bg);
        return;
    }
    ++m_frame;
    const vbgo_tt_eye_view &any = have[0] ? view[0] : view[1];
    ClassifyWorlds(any.worlds);
    m_oam = any.oam;
    if (m_auto)
        UpdateAutoColors();
    const bool haveCells = pack && !m_cellStart.empty();
    const TileColorPack::ContextTile *contexts = pack ? pack->ContextTiles().data() : nullptr;
    const bool haveContexts = pack && !m_markerGrid[0].empty();
    ContextGroupBits *markers = nullptr; // this frame's marker grid (Near reads it and the two before it)
    uint8_t *markerLayers = nullptr;
    if (haveContexts)
    {
        m_gridCurrent = (m_gridCurrent + 1) % 3;
        markers = m_markerGrid[m_gridCurrent].data();
        std::fill(markers, markers + kGridW * kGridH, ContextGroupBits{});
        if (m_layerBound.Any())
        {
            markerLayers = m_markerLayer[m_gridCurrent].data();
            std::fill(markerLayers, markerLayers + kGridW * kGridH, 0xFF);
        }
        // This frame's own markers, before anything is colored (the left
        // picture's - markers count at their left-eye spot): a figure's
        // colors are on from the first frame it shows its markers, rather
        // than a frame later (each new pose of an animation would otherwise
        // show its shared tiles' usual colors for a frame - a flash of Luigi's
        // green on Mario's back in Mario's Tennis's intro).
        if (have[0])
        {
            const vbgo_tt_eye_view &v = view[0];
            ResolveSlots(v.hashes);
            bool any = false;
            for (unsigned c = 0; c < 2048; ++c)
                any |= (m_slotMarkers[c] = m_slots[c].markerBits).Any();
            for (uint32_t x = 0; any && x < VBGO_TT_WIDTH; ++x)
            {
                const uint64_t *column = v.columns[x];
                if (!column)
                    continue;
                const uint8_t *r = &raw[(static_cast<size_t>(eyeOffset[0]) + x) * 4];
                unsigned lastChr = ~0u, lastBand = ~0u; // (a cell's marker counted once per tile and band)
                for (uint32_t y = 0; y < VBGO_TT_HEIGHT; ++y)
                {
                    const uint64_t t = column[y];
                    if ((t >> 48) != v.stamp)
                        continue;
                    const unsigned chr = VBGO_TAG_CHAR(t);
                    const ContextGroupBits &slotBits = m_slotMarkers[chr];
                    if (!slotBits.Any() || !VBGO_TAG_PIXEL(t) || (chr == lastChr && (y >> 3) == lastBand))
                        continue;
                    uint32_t rawPixel;
                    std::memcpy(&rawPixel, r + static_cast<size_t>(y) * fbWidth * 4, 4);
                    if (!(rawPixel & 0xFFFFFFu))
                        continue; // (a shade the game switched off)
                    lastChr = chr, lastBand = y >> 3;
                    const bool sprite = VBGO_TAG_IS_OBJ(t) != 0;
                    const ContextGroupBits bits = slotBits & (sprite ? ~m_layerBound : m_layerBound);
                    if (!bits.Any())
                        continue;
                    const unsigned cell = (y >> 3) * kGridW + (x >> 3);
                    markers[cell] |= bits;
                    if (!sprite && markerLayers)
                        markerLayers[cell] = static_cast<uint8_t>(m_worlds[VBGO_TAG_WORLD(t)].colors);
                }
            }
        }
        // Sprites' groups: this frame's and the last two frames' markers
        // within reach of each cell, once (rows, then columns), so a lookup
        // is one read.
        // (Close: within one cell - see contextTile.)
        std::vector<ContextGroupBits> rows(kGridW * kGridH), closeRows(kGridW * kGridH), here(kGridW * kGridH);
        const ContextGroupBits *a = m_markerGrid[(m_gridCurrent + 1) % 3].data(), *b = m_markerGrid[(m_gridCurrent + 2) % 3].data();
        const ContextGroupBits free = ~m_layerBound;
        for (int i = 0; i < kGridW * kGridH; ++i)
            here[i] = (a[i] | b[i] | markers[i]) & free;
        for (int gy = 0; gy < kGridH; ++gy)
            for (int gx = 0; gx < kGridW; ++gx)
            {
                ContextGroupBits bits, closeBits;
                for (int x = std::max(0, gx - kContextReach); x <= std::min(kGridW - 1, gx + kContextReach); ++x)
                {
                    bits |= here[gy * kGridW + x];
                    if (std::abs(x - gx) <= 1)
                        closeBits |= here[gy * kGridW + x];
                }
                rows[gy * kGridW + gx] = bits, closeRows[gy * kGridW + gx] = closeBits;
            }
        m_nearSprites.assign(kGridW * kGridH, ContextGroupBits{});
        m_closeSprites.assign(kGridW * kGridH, ContextGroupBits{});
        for (int gy = 0; gy < kGridH; ++gy)
            for (int gx = 0; gx < kGridW; ++gx)
                for (int y = std::max(0, gy - kContextReach); y <= std::min(kGridH - 1, gy + kContextReach); ++y)
                {
                    m_nearSprites[gy * kGridW + gx] |= rows[y * kGridW + gx];
                    if (std::abs(y - gy) <= 1)
                        m_closeSprites[gy * kGridW + gx] |= closeRows[y * kGridW + gx];
                }
    }
    // Pairs: the left pictures' colors per block and shade, the tiles they
    // use, their pixels of tiles colored per map cell.
    const bool pairs = pack && m_pairCount > 0;
    if (!pairs)
        m_rightOwn.clear();
    if (pairs)
    {
        for (int p = 0; p < m_pairCount; ++p)
        {
            if (m_pairs[p].blocks.empty())
                m_pairs[p].blocks.resize(kBlocksX * kBands);
        }
        m_leftPairHashes.assign(4096, 0);
        m_leftPairMasks.assign(4096, 0);
        m_leftPairHashZero = 0;
        m_leftPicture.assign(kMapW * kMapH, 0);
        m_rightOwn.assign(VBGO_TT_EYE_PIXELS, 0);
        for (auto &row : m_pairRows)
            row.clear();
        if (m_mapCache.empty())
            m_mapCache.resize(4096);
    }
    std::array<uint32_t, 2048> leftPairSlot{}; // slots the left pictures drew this frame (bit per pair)
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
    // The tile's colors with context (the slow path - see Run::slow): the
    // same down a run's pixels in one row of grid cells (8 rows).
    struct NearBits
    {
        ContextGroupBits near, close;
    };
    auto contextTile = [&](uint64_t t, unsigned eye, int x, int y, int &nearCell, NearBits &nearBits) -> const TileColorPack::Tile * {
        const unsigned chr = VBGO_TAG_CHAR(t), palette = VBGO_TAG_PALETTE(t);
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
                const int i = (y >> 3) * kGridW + (lx >> 3);
                if (sprite)
                    nearBits.near = m_nearSprites[i], nearBits.close = m_closeSprites[i];
                else if (m_layerBound.Any())
                {
                    Near(lx, y, world, nearBits.near, nearBits.close);
                    nearBits.near = nearBits.near & m_layerBound, nearBits.close = nearBits.close & m_layerBound;
                }
                else
                    nearBits = NearBits{};
            }
            // A group with a marker right next to it (within a cell) first,
            // then one within reach: of two characters side by side, each
            // takes its own colors.
            for (int pass = 0; pass < 2 && !tile; ++pass)
            {
                const ContextGroupBits &bits = pass == 0 ? nearBits.close : nearBits.near;
                for (uint32_t k = 0; bits.Any() && k < slot.contextCount; ++k)
                {
                    const TileColorPack::ContextTile &c = contexts[slot.contextFirst + k];
                    if (bits.Test(c.group))
                    {
                        tile = &c.tile;
                        break;
                    }
                }
            }
        }
        if (!tile)
        {
            tile = slot.palette[palette];
            if (slot.layered && tile == slot.base)
                if (const TileColorPack::Tile *layer = LayerTile(chr, world))
                    tile = layer;
        }
        return tile ? tile : &kNoTile;
    };
    // (Run: see the header; set up once per key and eye - see m_runCache.)
    if (m_runCache.empty())
        m_runCache.resize(kRunCacheSize);
    constexpr uint64_t kRunMask = 0x7FFull | (3ull << 17) | (1ull << 19) | (31ull << 22) | (1ull << 27) | (0xFFFFull << 28);

    RunSetUp setUp{pack, haveCells, markers != nullptr, haveContexts, pairs, leftPairSlot.data(), 0};
    for (unsigned eye = 0; eye < 2; ++eye)
    {
        if (!have[eye])
            continue;
        const vbgo_tt_eye_view &v = view[eye];
        setUp.flatObjPalettes = v.flat_obj_palettes;
        if (pack)
            ResolveSlots(v.hashes);
        if (eye == 1 && pairs)
            EstimatePairs(view, raw, fbWidth, eyeOffset);
        if (eye == 1 && pairs)
        {
            // The tiles each pair's left picture drew (by contents: games
            // keep a tile in a slot per eye - Mario Clash's digits).
            const uint32_t mask = static_cast<uint32_t>(m_leftPairHashes.size() - 1);
            for (unsigned c = 0; c < 2048; ++c)
                if (leftPairSlot[c])
                {
                    const uint32_t hash = view[0].hashes[c];
                    if (!hash)
                    {
                        m_leftPairHashZero |= leftPairSlot[c];
                        continue;
                    }
                    uint32_t i = (hash * 2654435761u) & mask;
                    while (m_leftPairHashes[i] && m_leftPairHashes[i] != hash)
                        i = (i + 1) & mask;
                    m_leftPairHashes[i] = hash;
                    m_leftPairMasks[i] |= leftPairSlot[c];
                }
        }
        const uint64_t stamp = v.stamp;
        if (++m_runCacheAt == 0)
        {
            for (CachedRun &cached : m_runCache)
                cached.at = 0;
            m_runCacheAt = 1;
        }
        const uint32_t runAt = m_runCacheAt;
        // In strips of kStrip columns, row by row: the frame is stored by
        // rows, so a row of a strip is one cache line of it (the tags are by
        // columns - each column of the strip reads on down its own).
        constexpr uint32_t kStrip = 16;
        for (uint32_t x0 = 0; x0 < VBGO_TT_WIDTH; x0 += kStrip)
        {
            uint64_t runKeys[kStrip];
            Run runs[kStrip];
            int nearCells[kStrip]; // (context: the last pixel's markers nearby, by cell and layer)
            NearBits nearBitsOf[kStrip];
            const uint64_t *columns[kStrip];
            uint32_t drawn = 0; // (columns with anything in them)
            for (uint32_t k = 0; k < kStrip; ++k)
            {
                runKeys[k] = ~0ull, nearCells[k] = -1, nearBitsOf[k] = NearBits{};
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
                    const size_t i = stripBase + k * 4 + y * rowBytes;
                    const uint8_t *r = &raw[i];
                    uint32_t rawPixel; // B, G, R, tag (shade, brightness) from the low byte up (little-endian, as everywhere this runs)
                    std::memcpy(&rawPixel, r, 4);
                    const unsigned pixel = VBGO_TAG_PIXEL(t);
                    if (pixel && !(rawPixel & 0xFFFFFFu))
                        continue; // drawn in a shade the game switched off - stays background
                    if ((t & kRunMask) != runKey)
                    {
                        // A new run (see Run): the layers' extents, the brightness...
                        runKey = t & kRunMask;
                        if (!VBGO_TAG_IS_OBJ(t))
                        {
                            const unsigned colors = m_worlds[VBGO_TAG_WORLD(t)].colors;
                            worldsDrawn |= 1u << colors;
                            left[colors] = std::min<int16_t>(left[colors], static_cast<int16_t>(x));
                            right[colors] = std::max<int16_t>(right[colors], static_cast<int16_t>(x));
                            top[colors] = std::min<int16_t>(top[colors], static_cast<int16_t>(y));
                            bottom[colors] = std::max<int16_t>(bottom[colors], static_cast<int16_t>(y));
                        }
                        brightest = std::max<unsigned>(brightest, rawPixel >> 26);
                        // ... and the rest: as the last run with this key in this eye, if any.
                        CachedRun &cached = m_runCache[(runKey * 0x9E3779B97F4A7C15ull) >> (64 - kRunCacheBits)];
                        if (cached.at == runAt && cached.key == runKey)
                            run = cached.run;
                        else
                        {
                            run = SetUpRun(t, eye, static_cast<int>(x), static_cast<int>(y), setUp);
                            cached.at = runAt;
                            cached.key = runKey;
                            cached.run = run;
                        }
                    }
                    const unsigned shade = (rawPixel >> 24) & 3, index = VBGO_TAG_INDEX(t);
                    const uint8_t *rgb = nullptr;
                    if (run.extra && pixel)
                    {
                        if (run.leftPicture && shade)
                            m_leftPicture[MapAt(x, y)] = static_cast<uint8_t>(run.leftPicture | (shade << 5));
                        if (run.ownPair >= 0)
                            m_rightOwn[y * VBGO_TT_WIDTH + x] = 1;
                        if (run.slow)
                        {
                            const unsigned chr = VBGO_TAG_CHAR(t);
                            if (m_slots[chr].ambiguous)
                                m_ambiguousPixels.emplace_back(static_cast<uint16_t>(x), static_cast<uint16_t>(y));
                            // (markers and context: the same for the run's pixels in a row of grid cells)
                            const bool newBand = static_cast<int>(y >> 3) != run.band;
                            if (newBand)
                            {
                                run.band = static_cast<int>(y >> 3);
                                run.tile = contextTile(t, eye, static_cast<int>(x), static_cast<int>(y), nearCells[k], nearBitsOf[k]);
                            }
                            if (newBand && markers && m_slots[chr].markerBits.Any())
                            {
                                // Sprites' groups count sprite markers, background
                                // figures' groups background ones (and remember the
                                // layer) - at the marker's left-eye spot.
                                const bool sprite = VBGO_TAG_IS_OBJ(t) != 0;
                                const ContextGroupBits bits = m_slots[chr].markerBits & (sprite ? ~m_layerBound : m_layerBound);
                                const int lx = LeftX(t, static_cast<int>(x), eye, y);
                                if (bits.Any() && lx >= 0 && lx < VBGO_TT_WIDTH)
                                {
                                    const unsigned cell = (y >> 3) * kGridW + (lx >> 3);
                                    markers[cell] |= bits;
                                    if (!sprite && markerLayers)
                                        markerLayers[cell] = static_cast<uint8_t>(m_worlds[VBGO_TAG_WORLD(t)].colors);
                                }
                            }
                        }
                    }
                    // 0. A tile its left picture draws too, where the pair's pictures are the same
                    // drawing shifted: the very pixel the left eye shows there (Mario Clash's
                    // stage: the right picture's tiles can be other tiles of the same look, or
                    // ones the left picture uses elsewhere in other colors).
                    if (run.copyPair >= 0 && pixel && shade)
                    {
                        if (static_cast<int>(y >> 3) != run.copyBand)
                        {
                            // (per 8 rows: the block's shift, if it's the left drawing shifted and on screen)
                            run.copyBand = static_cast<int>(y >> 3);
                            const int block = run.copyBand * kBlocksX + static_cast<int>(x >> 3);
                            const int lx = static_cast<int>(x) + run.blockDisparity[block];
                            run.copyDx = run.blockExact[block] && lx >= 0 && lx < VBGO_TT_WIDTH ? lx - static_cast<int>(x) : kNoDisparity;
                        }
                        if (run.copyDx != kNoDisparity &&
                            m_leftPicture[MapAt(static_cast<int>(x) + run.copyDx, static_cast<int>(y))] == ((run.copyPair + 1) | (shade << 5)))
                        {
                            std::memcpy(&frame[i], &frame[(static_cast<size_t>(y) * fbWidth + eyeOffset[0] + x + run.copyDx) * 4], 3);
                            continue;
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
                        // picture's color for its shade there, as shown (unless
                        // a right-eye capture painted it).
                        if (run.ownPair >= 0 && shade && !(run.rightPainted >> index & 1))
                        {
                            // (most often the left picture shows the same shade right where
                            // the block's disparity puts it - else see RegionColor)
                            const int d = run.blockDisparity ? run.blockDisparity[(y >> 3) * kBlocksX + (x >> 3)] : kNoDisparity;
                            const int lx = static_cast<int>(x) + d;
                            if (d != kNoDisparity && lx >= 0 && lx < VBGO_TT_WIDTH &&
                                m_leftPicture[MapAt(lx, static_cast<int>(y))] == ((run.ownPair + 1) | (shade << 5)))
                            {
                                std::memcpy(&frame[i], &frame[(static_cast<size_t>(y) * fbWidth + eyeOffset[0] + lx) * 4], 3);
                                continue;
                            }
                            if (RegionColor(run.ownPair, static_cast<int>(x), static_cast<int>(y), shade, frame, fbWidth, eyeOffset[0], &frame[i]))
                                continue;
                        }
                        // 2-4. Context (slow runs' tiles), the palette's, the layer's, or the tile's own colors.
                        if (run.tile->mask >> index & 1)
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
                    if (run.flash && shade && (!run.ramp || rgb != (*run.ramp)[shade - 1].data()))
                    {
                        // (painted, in a flash: 60% of the way to the shade's white, gray or black)
                        static constexpr int kTarget[4] = {0, 0, 128, 255};
                        uint8_t washed[3];
                        for (int ch = 0; ch < 3; ++ch)
                            washed[ch] = static_cast<uint8_t>((rgb[ch] * 2 + kTarget[shade] * 3) / 5);
                        write(&frame[i], washed, rawPixel >> 26);
                        continue;
                    }
                    write(&frame[i], rgb, rawPixel >> 26);
                }
        }
        if (!m_ambiguousPixels.empty())
            PaintAmbiguous(frame, fbWidth, eyeOffset[eye], v);
    }
    // (only where the game's CPU drew - else every pixel shown came from tiles)
    if (m_auto && ((have[0] && view[0].cpu_drawn) || (have[1] && view[1].cpu_drawn) || !have[0] || !have[1]))
        brightest = std::max(brightest, PaintDepth(frame, raw, fbWidth, eyeOffset, view, have, bg));
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

unsigned ColorPackRenderer::PaintDepth(uint8_t *frame, const uint8_t *raw, uint32_t fbWidth, const uint32_t eyeOffset[2],
                                       const vbgo_tt_eye_view view[2], const bool have[2], const int bg[3])
{
    // Each eye's shades, and the lit pixels no tile drew this frame (a
    // shade the game switched off counts as nothing).
    constexpr size_t kPixels = VBGO_TT_EYE_PIXELS;
    const size_t rowBytes = static_cast<size_t>(fbWidth) * 4;
    bool any = false;
    unsigned brightest = 0;
    for (unsigned eye = 0; eye < 2; ++eye)
    {
        m_depthShades[eye].assign(kPixels, 0);
        m_depthWant[eye].assign(kPixels, 0);
        uint8_t *shades = m_depthShades[eye].data(), *want = m_depthWant[eye].data();
        const uint64_t stamp = have[eye] ? view[eye].stamp : 0;
        for (uint32_t x = 0; x < VBGO_TT_WIDTH; ++x)
        {
            const uint64_t *column = have[eye] ? view[eye].columns[x] : nullptr;
            const uint8_t *src = &raw[(static_cast<size_t>(eyeOffset[eye]) + x) * 4];
            for (uint32_t y = 0; y < VBGO_TT_HEIGHT; ++y, src += rowBytes)
            {
                uint32_t rawPixel;
                std::memcpy(&rawPixel, src, 4);
                const unsigned shade = (rawPixel >> 24) & 3;
                if (!shade || !(rawPixel & 0xFFFFFFu))
                    continue;
                shades[y * VBGO_TT_WIDTH + x] = static_cast<uint8_t>(shade);
                if (column && (column[y] >> 48) == stamp && VBGO_TAG_PIXEL(column[y]))
                    continue; // a tile drew it
                want[y * VBGO_TT_WIDTH + x] = 1;
                any = true;
                brightest = std::max<unsigned>(brightest, rawPixel >> 26);
            }
        }
    }
    if (!any)
        return 0;
    if (!m_pack && brightest > m_autoMaxLevel)
    {
        m_autoMaxLevel = brightest; // (before painting: the very first frame fades right too)
        SetFadeReference(brightest);
    }
    const uint8_t *const shades[2] = {m_depthShades[0].data(), m_depthShades[1].data()};
    const uint8_t *const want[2] = {m_depthWant[0].data(), m_depthWant[1].data()};
    m_depth.Update(shades, want);
    for (unsigned eye = 0; eye < 2; ++eye)
        for (uint32_t y = 0; y < VBGO_TT_HEIGHT; ++y)
        {
            const uint8_t *w = &want[eye][y * VBGO_TT_WIDTH];
            for (uint32_t x = 0; x < VBGO_TT_WIDTH; ++x)
            {
                if (!w[x])
                    continue;
                const size_t i = (static_cast<size_t>(y) * fbWidth + eyeOffset[eye] + x) * 4;
                uint8_t rgb[3];
                DepthColors::Color(m_depth.Disparity(eye, x, y), shades[eye][y * VBGO_TT_WIDTH + x], rgb);
                const int fade = m_fade[raw[i + 3] >> 2]; // 0-256 (the tag byte's brightness)
                frame[i + 0] = static_cast<uint8_t>(bg[0] + (((rgb[2] - bg[0]) * fade + 128) >> 8));
                frame[i + 1] = static_cast<uint8_t>(bg[1] + (((rgb[1] - bg[1]) * fade + 128) >> 8));
                frame[i + 2] = static_cast<uint8_t>(bg[2] + (((rgb[0] - bg[2]) * fade + 128) >> 8));
            }
        }
    return brightest;
}

namespace
{
// Per byte of x, how many of its bits are set (0-8).
inline uint64_t ByteCounts(uint64_t x)
{
    x = x - ((x >> 1) & 0x5555555555555555ull);
    x = (x & 0x3333333333333333ull) + ((x >> 2) & 0x3333333333333333ull);
    return (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0Full;
}
} // namespace

void ColorPackRenderer::EstimatePairs(const vbgo_tt_eye_view view[2], const uint8_t *raw, uint32_t fbWidth,
                                      const uint32_t eyeOffset[2])
{
    // The pairs whose disparities are out of date (once a frame, before the
    // right eye is painted): kept while a pair's registers stay put (64
    // frames at most); while both its worlds scroll together, refined
    // around the last estimate every few frames; anything else searches
    // again. Only pairs that needed them lately (or never had any).
    const uint16_t *worlds = view[0].worlds ? view[0].worlds : view[1].worlds;
    if (!worlds)
        return;
    std::array<int, kMaxPairs> todo{};
    std::array<bool, kMaxPairs> local{};
    int count = 0;
    for (int p = 0; p < m_pairCount; ++p)
    {
        Pair &pair = m_pairs[p];
        if (pair.estimated && m_frame - pair.wantedAt > 64)
            continue;
        std::array<uint16_t, 32> attributes;
        std::copy(&worlds[pair.left * 16], &worlds[pair.left * 16] + 16, attributes.begin());
        std::copy(&worlds[pair.right * 16], &worlds[pair.right * 16] + 16, attributes.begin() + 16);
        // (a pair's other left worlds: any change there is another picture)
        uint32_t others = 2166136261u;
        for (int w = 0; w < 32; ++w)
            if (w != pair.left && (pair.leftWorlds >> w & 1))
                for (int f = 0; f < 16; ++f)
                    others = (others ^ worlds[w * 16 + f]) * 16777619u;
        const uint32_t age = m_frame - pair.estimatedAt;
        if (pair.estimated && attributes == pair.attributes && others == pair.otherAttributes && age < 64)
            continue;
        bool scrolled = pair.estimated && others == pair.otherAttributes;
        pair.otherAttributes = others;
        for (int f = 0; f < 16 && scrolled; ++f)
            scrolled = f >= 1 && f <= 6 ? static_cast<uint16_t>(attributes[16 + f] - attributes[f]) ==
                                              static_cast<uint16_t>(pair.attributes[16 + f] - pair.attributes[f])
                                        : attributes[f] == pair.attributes[f] && attributes[16 + f] == pair.attributes[16 + f];
        if (scrolled && age < 4)
            continue;
        pair.attributes = attributes;
        local[count] = scrolled;
        todo[count++] = p;
    }
    if (!count)
        return;
    // Both pictures of every one of them, in one pass: per pair, per eye,
    // per row, per shade 1-3, a guarded bit row.
    std::array<int8_t, 32> layerSlot[2], spriteSlot;
    layerSlot[0].fill(-1), layerSlot[1].fill(-1), spriteSlot.fill(-1);
    for (int k = 0; k < count; ++k)
    {
        const Pair &pair = m_pairs[todo[k]];
        if (pair.sprites)
            spriteSlot[pair.left] = static_cast<int8_t>(k);
        else
        {
            for (int w = 0; w < 32; ++w)
                if (pair.leftWorlds >> w & 1)
                    layerSlot[0][w] = static_cast<int8_t>(k);
            layerSlot[1][pair.right] = static_cast<int8_t>(k);
        }
    }
    const size_t eyeRows = static_cast<size_t>(VBGO_TT_HEIGHT) * 3 * kGuardedRow;
    m_estimateBits.assign(static_cast<size_t>(count) * 2 * eyeRows, 0);
    uint64_t *bits = m_estimateBits.data();
    for (unsigned eye = 0; eye < 2; ++eye)
        for (int x = 0; x < VBGO_TT_WIDTH; ++x)
        {
            const uint64_t *column = view[eye].columns[x];
            if (!column)
                continue;
            const uint8_t *shades = &raw[(static_cast<size_t>(eyeOffset[eye]) + x) * 4 + 3];
            for (int y = 0; y < VBGO_TT_HEIGHT; ++y)
            {
                const uint64_t t = column[y];
                if ((t >> 48) != view[eye].stamp || !VBGO_TAG_PIXEL(t))
                    continue;
                int k;
                if (VBGO_TAG_IS_OBJ(t))
                {
                    k = spriteSlot[VBGO_TAG_WORLD(t)];
                    if (k >= 0 && EyeOnly(t) != (eye ? 2 : 1))
                        continue;
                }
                else
                    k = layerSlot[eye][VBGO_TAG_WORLD(t)];
                if (k < 0)
                    continue;
                const unsigned shade = shades[static_cast<size_t>(y) * fbWidth * 4] & 3;
                if (shade)
                    bits[(static_cast<size_t>(k) * 2 + eye) * eyeRows + (static_cast<size_t>(y) * 3 + shade - 1) * kGuardedRow + 1 +
                         (x >> 6)] |= 1ull << (x & 63);
            }
        }
    for (int k = 0; k < count; ++k)
        EstimatePair(m_pairs[todo[k]], &bits[static_cast<size_t>(k) * 2 * eyeRows], &bits[(static_cast<size_t>(k) * 2 + 1) * eyeRows],
                     local[k]);
}

void ColorPackRenderer::EstimatePair(Pair &pair, const uint64_t *leftBits, const uint64_t *rightBits, bool scrolled)
{
    // Per 8-row band: the shift that lines up the most right-picture pixels
    // with left-picture pixels of the same shade (within kMaxDisparity) -
    // coarse (every other shift and row) then fine around it, or near the
    // last estimate if the pair only scrolled. Only the words of a row the
    // right picture covers in that band count.
    std::array<int16_t, kBands> found;
    found.fill(kNoDisparity);
    std::array<int8_t, kBands> wordFrom{}, wordTo{};
    std::array<uint8_t, kBands> bandMatch{}; // (how much of the band lines up at its disparity, percent)
    for (int band = 0; band < kBands; ++band)
    {
        int lit = 0, from = kRowWords, to = -1;
        for (int y = band * 8; y < band * 8 + 8; ++y)
            for (int s = 0; s < 3; ++s)
                for (int w = 0; w < kRowWords; ++w)
                    if (const uint64_t r = rightBits[(y * 3 + s) * kGuardedRow + 1 + w])
                    {
                        lit += CountSet(r);
                        from = std::min(from, w);
                        to = std::max(to, w);
                    }
        wordFrom[band] = static_cast<int8_t>(from);
        wordTo[band] = static_cast<int8_t>(to);
        if (lit < 16)
            continue;
        const int previous = pair.estimated ? pair.disparity[band] : kNoDisparity;
        const int around = previous != kNoDisparity ? previous : 0;
        auto same = [&](int d, int step) {
            int n = 0;
            for (int y = band * 8; y < band * 8 + 8; y += step)
                for (int s = 0; s < 3; ++s)
                {
                    const uint64_t *r = &rightBits[(y * 3 + s) * kGuardedRow];
                    const uint64_t *l = &leftBits[(y * 3 + s) * kGuardedRow];
                    for (int w = from; w <= to; ++w)
                        n += CountSet(r[1 + w] & Shifted(l, w, d));
                }
            return n;
        };
        // (ties: the one nearer the last estimate, else nearer 0)
        auto search = [&](int lo, int hi, int stride, int step, int &bestD) {
            int best = -1;
            for (int d = std::max(-kMaxDisparity, lo); d <= std::min(kMaxDisparity, hi); d += stride)
            {
                const int n = same(d, step);
                if (n > best || (n == best && std::abs(d - around) < std::abs(bestD - around)))
                    best = n, bestD = d;
            }
            return best;
        };
        int bestD = around, best;
        if (scrolled && previous != kNoDisparity)
            best = search(previous - 4, previous + 4, 1, 1, bestD);
        else
        {
            search(-kMaxDisparity, kMaxDisparity, 2, 2, bestD);
            const int coarse = bestD;
            best = search(coarse - 2, coarse + 2, 1, 1, bestD);
        }
        if (best > 0)
        {
            found[band] = static_cast<int16_t>(bestD);
            bandMatch[band] = static_cast<uint8_t>(100 * best / lit);
        }
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
    // Per 8x8 block of the right picture: the shift within kBlockReach of
    // its band's that lines up the most of its pixels in a 24-pixel-wide
    // window around it (pictures have depth within a band - Wario Land's
    // title: Wario's head before his plane's nose). All of a band's blocks
    // at once: per shift, per 8 columns, how many line up (bytes of a word
    // add up 8 columns at a time - at most 8 rows x 8 pixels, so no carry).
    // Then each block the median of itself and its neighbours (no lone
    // outliers), except blocks that are the left drawing shifted.
    constexpr int kBlockReach = 8, kShifts = 2 * kMaxDisparity + 1;
    std::vector<int16_t> blocks(kBlocksX * kBands, static_cast<int16_t>(kNoDisparity));
    std::vector<uint8_t> exact(kBlocksX * kBands, 0);
    const bool keepBlocks = scrolled && pair.blockDisparity.size() == blocks.size();
    for (int by = 0; by < kBands; ++by)
    {
        const int band = pair.disparity[by], from = wordFrom[by], to = wordTo[by];
        if (band == kNoDisparity || to < 0)
            continue;
        uint8_t lit[kBlocksX] = {};
        {
            uint64_t sum[kRowWords] = {};
            for (int y = by * 8; y < by * 8 + 8; ++y)
                for (int s = 0; s < 3; ++s)
                    for (int w = from; w <= to; ++w)
                        sum[w] += ByteCounts(rightBits[(y * 3 + s) * kGuardedRow + 1 + w]);
            for (int w = from; w <= to; ++w)
                for (int j = 0; j < 8; ++j)
                    lit[w * 8 + j] = static_cast<uint8_t>(sum[w] >> (8 * j));
        }
        // Per shift (worked out when first needed), per block: how many of
        // its pixels line up.
        uint8_t score[kShifts][kBlocksX];
        bool scored[kShifts] = {};
        auto scoreAt = [&](int d) -> const uint8_t * {
            uint8_t *row = score[d + kMaxDisparity];
            if (scored[d + kMaxDisparity])
                return row;
            scored[d + kMaxDisparity] = true;
            std::memset(row, 0, kBlocksX);
            uint64_t sum[kRowWords] = {};
            for (int y = by * 8; y < by * 8 + 8; ++y)
                for (int s = 0; s < 3; ++s)
                {
                    const uint64_t *r = &rightBits[(y * 3 + s) * kGuardedRow];
                    const uint64_t *l = &leftBits[(y * 3 + s) * kGuardedRow];
                    for (int w = from; w <= to; ++w)
                        sum[w] += ByteCounts(r[1 + w] & Shifted(l, w, d));
                }
            for (int w = from; w <= to; ++w)
                for (int j = 0; j < 8; ++j)
                    row[w * 8 + j] = static_cast<uint8_t>(sum[w] >> (8 * j));
            return row;
        };
        for (int bx = 0; bx < kBlocksX; ++bx)
        {
            if (lit[bx] < 6)
                continue;
            auto window = [&](const uint8_t *row) {
                return row[bx] + (bx ? row[bx - 1] : 0) + (bx + 1 < kBlocksX ? row[bx + 1] : 0);
            };
            const int windowLit = window(lit);
            // (ties: the one nearer the band's)
            int best = -1, bestD = band;
            auto search = [&](int lo, int hi, int stride = 1) {
                for (int d = std::max(-kMaxDisparity, lo); d <= std::min(kMaxDisparity, hi); d += stride)
                {
                    const int n = window(scoreAt(d));
                    if (n > best || (n == best && std::abs(d - band) < std::abs(bestD - band)))
                        best = n, bestD = d;
                }
            };
            // Near the block's last estimate if the picture only scrolled,
            // else near the band's...
            const int previous = keepBlocks ? pair.blockDisparity[by * kBlocksX + bx] : kNoDisparity;
            if (previous != kNoDisparity)
                search(previous - 3, previous + 3);
            else
                search(band - kBlockReach, band + kBlockReach);
            // ... unless under half its pixels line up there: then anywhere,
            // if somewhere is clearly better (a band can hold things at very
            // different depths - Galactic Pinball's title planets). But not
            // where the band is sure and puts the block off the left eye's
            // screen: it just doesn't show it (Mario's Tennis's clouds at
            // the edge would match something else).
            const int windowFrom = std::max(0, bx - 1) * 8 + bestD, windowTo = std::min(kBlocksX, bx + 2) * 8 + bestD;
            const bool offScreen = windowFrom < 0 || windowTo > VBGO_TT_WIDTH;
            if (best * 2 < windowLit && !(offScreen && bandMatch[by] >= 60))
            {
                const int near = best, nearD = bestD;
                search(-kMaxDisparity, kMaxDisparity, 2); // (every other shift, then around the best)
                search(bestD - 1, bestD + 1);
                if ((best - near) * 5 < windowLit)
                    best = near, bestD = nearD;
            }
            if (best > 0)
            {
                blocks[by * kBlocksX + bx] = static_cast<int16_t>(bestD);
                // (nearly every pixel lines up: the same drawing, shifted - its pixels correspond exactly)
                exact[by * kBlocksX + bx] = best * 10 >= windowLit * 9;
            }
        }
    }
    pair.blockExact = exact;
    pair.blockDisparity.assign(blocks.size(), static_cast<int16_t>(kNoDisparity));
    for (int by = 0; by < kBands; ++by)
        for (int bx = 0; bx < kBlocksX; ++bx)
        {
            const int i = by * kBlocksX + bx;
            if (blocks[i] == kNoDisparity)
            {
                pair.blockDisparity[i] = pair.disparity[by]; // (few pixels: its band's)
                continue;
            }
            if (exact[i])
            {
                pair.blockDisparity[i] = blocks[i]; // (as found)
                continue;
            }
            int values[9], n = 0;
            for (int yy = std::max(0, by - 1); yy <= std::min(kBands - 1, by + 1); ++yy)
                for (int xx = std::max(0, bx - 1); xx <= std::min(kBlocksX - 1, bx + 1); ++xx)
                    if (blocks[yy * kBlocksX + xx] != kNoDisparity)
                        values[n++] = blocks[yy * kBlocksX + xx];
            std::nth_element(values, values + n / 2, values + n);
            pair.blockDisparity[i] = static_cast<int16_t>(values[n / 2]);
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
    // (a neighbour's tag must match in these: drawn this frame, the pixel's value, its layer, sprite or not)
    constexpr uint64_t kSame = (0xFFFFull << 48) | (31ull << 22) | (3ull << 20) | (1ull << 19);
    static constexpr int kDirections[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
    for (const auto &p : m_ambiguousPixels)
    {
        const int x = p.first, y = p.second;
        const uint64_t t = view.columns[x] ? view.columns[x][y] : 0;
        if ((t >> 48) != view.stamp)
            continue;
        const uint64_t same = t & kSame;
        const bool sprite = m_oam && VBGO_TAG_IS_OBJ(t);
        const int jp = sprite ? vbgo_obj_parallax(m_oam, VBGO_TAG_OBJ_NO(t)) : 0;
        int from = -1;
        for (int d = 1; d <= 8 && from < 0; ++d)
            for (const auto &dir : kDirections)
            {
                const int nx = x + dir[0] * d, ny = y + dir[1] * d;
                if (nx < 0 || nx >= VBGO_TT_WIDTH || ny < 0 || ny >= VBGO_TT_HEIGHT || !view.columns[nx])
                    continue;
                const uint64_t n = view.columns[nx][ny];
                if ((n & kSame) == same && !m_slots[VBGO_TAG_CHAR(n)].ambiguous &&
                    (!sprite || vbgo_obj_parallax(m_oam, VBGO_TAG_OBJ_NO(n)) == jp))
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
