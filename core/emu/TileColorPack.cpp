#include "emu/TileColorPack.h"
#include "emu/vbgo_tiletrack.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr char kPackMagicV1[8] = {'V', 'B', 'G', 'O', 'C', 'P', '0', '1'};
    constexpr char kPackMagicV2[8] = {'V', 'B', 'G', 'O', 'C', 'P', '0', '2'}; // + per-layer colors, left uncolored
    constexpr char kPackMagic[8] = {'V', 'B', 'G', 'O', 'C', 'P', '0', '3'};   // + per-palette colors, map cells
    constexpr char kSidecarMagicV1[8] = {'V', 'B', 'G', 'O', 'T', 'I', 'L', '1'};
    constexpr char kSidecarMagic[8] = {'V', 'B', 'G', 'O', 'T', 'I', 'L', '2'}; // + a map cell per pixel
    constexpr char kPaletteMagicV1[8] = {'V', 'B', 'G', 'O', 'P', 'A', 'L', '1'};
    constexpr char kPaletteMagic[8] = {'V', 'B', 'G', 'O', 'P', 'A', 'L', '2'}; // + the brightness level
    constexpr char kShownMagic[8] = {'V', 'B', 'G', 'O', 'S', 'H', 'W', '1'};   // + RGB per pixel, as captured
    constexpr char kFiguresMagic[8] = {'V', 'B', 'G', 'O', 'F', 'I', 'G', '1'}; // + figure id per pixel (uint16)
    constexpr char kContextMagic[8] = {'V', 'B', 'G', 'O', 'C', 'T', 'X', '1'}; // context variants, after the v3 pack
    constexpr char kLayerBoundMagic[8] = {'V', 'B', 'G', 'O', 'C', 'T', 'X', 'L'}; // which of them are layer-bound, after that

    constexpr uint32_t kNoColor = 0x01000000;   // a magenta vote: leave uncolored
    constexpr uint32_t kBackground = 0x02000000; // a transparent pixel left as the background (fills only)
    constexpr uint32_t kFillVote = 0x100;     // Vote::extra: a transparent pixel painted over
    // What captures without a palette block were made with: the Ember
    // Multicolor palette (see Settings.h), as 0-255 RGB.
    constexpr uint8_t kDefaultCapturePalette[4][3] = {{8, 3, 0}, {166, 38, 5}, {242, 140, 26}, {255, 242, 191}};

    int Distance(const uint8_t *a, const uint8_t *b) { return std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]); }

    uint32_t ReadLe32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
    uint64_t ReadLe64(const uint8_t *p) { return ReadLe32(p) | (static_cast<uint64_t>(ReadLe32(p + 4)) << 32); }
    void AppendLe(std::vector<uint8_t> &v, uint64_t x, int bytes)
    {
        for (int i = 0; i < bytes; ++i)
            v.push_back(static_cast<uint8_t>(x >> (i * 8)));
    }

    template <typename T> void Store(T &tile, unsigned index, uint32_t rgb) // a Tile or a CellTile
    {
        tile.mask |= 1ull << index;
        tile.rgb[index][0] = static_cast<uint8_t>(rgb >> 16);
        tile.rgb[index][1] = static_cast<uint8_t>(rgb >> 8);
        tile.rgb[index][2] = static_cast<uint8_t>(rgb);
    }

    // A pixel key's winning color: the most votes; ties go to the lower value
    // (the same result on every platform). Votes must be sorted by (key, rgb);
    // [begin, end) is one key's run.
    template <typename It> uint32_t Winner(It begin, It end, uint32_t &bestVotes, uint32_t &allVotes, size_t &distinct)
    {
        uint32_t best = 0;
        bestVotes = allVotes = 0;
        distinct = 0;
        for (It run = begin; run != end;)
        {
            It next = run;
            uint32_t count = 0;
            while (next != end && next->rgb == run->rgb)
                ++next, ++count;
            ++distinct;
            allVotes += count;
            if (count > bestVotes) // runs come in increasing rgb order, so ">" keeps the lower value on ties
            {
                best = run->rgb;
                bestVotes = count;
            }
            run = next;
        }
        return best;
    }

    // Calls fn(begin, end) for every run of equal keys (sorted vector).
    template <typename V, typename Fn> void ForEachKey(V &votes, Fn fn)
    {
        for (auto run = votes.begin(); run != votes.end();)
        {
            auto next = run;
            while (next != votes.end() && next->key == run->key)
                ++next;
            fn(run, next);
            run = next;
        }
    }

    // Sorted (key, chosen color) list - a lookup table without a hash map.
    struct Chosen
    {
        std::vector<std::pair<uint64_t, uint32_t>> entries;
        bool Get(uint64_t key, uint32_t &rgb) const
        {
            const auto it = std::lower_bound(entries.begin(), entries.end(), std::make_pair(key, 0u),
                                             [](const auto &a, const auto &b) { return a.first < b.first; });
            if (it == entries.end() || it->first != key)
                return false;
            rgb = it->second;
            return true;
        }
    };
} // namespace

void TileColorPack::Clear()
{
    m_tiles.clear();
    m_layerTiles.clear();
    m_paletteTiles.clear();
    m_leftUncolored.clear();
    m_cellTiles.clear();
    m_contextGroups.clear();
    m_contextTiles.clear();
    m_contextLayerBound = 0;
    m_objectTiles.clear();
    m_objectIsFigure.clear();
    m_objectFamily.clear();
    m_families = 0;
    m_looseTiles.clear();
    m_contextVotes.clear();
    m_tileShades.clear();
    m_votes.clear();
    m_sheetVotes.clear();
    m_layerVotes.clear();
    m_paletteVotes.clear();
    m_cellVotes.clear();
    m_paintingLevels.clear();
    m_tileRows.clear();
    m_referenceLevel = 63;
}

void TileColorPack::AppendSidecarPalette(std::vector<uint8_t> &sidecar, const std::array<std::array<uint8_t, 3>, 4> &palette,
                                         uint8_t brightnessLevel)
{
    sidecar.insert(sidecar.end(), kPaletteMagic, kPaletteMagic + 8);
    for (const auto &color : palette)
        sidecar.insert(sidecar.end(), color.begin(), color.end());
    sidecar.push_back(brightnessLevel);
}

void TileColorPack::AppendSidecarShown(std::vector<uint8_t> &sidecar, const uint8_t *rgb, size_t pixels)
{
    sidecar.insert(sidecar.end(), kShownMagic, kShownMagic + 8);
    sidecar.insert(sidecar.end(), rgb, rgb + pixels * 3);
}

const TileColorPack::Tile *TileColorPack::Find(uint32_t hash) const
{
    const auto it = m_tiles.find(hash);
    return it == m_tiles.end() ? nullptr : &it->second;
}

void TileColorPack::Set(uint32_t hash, unsigned index, uint8_t r, uint8_t g, uint8_t b)
{
    Store(m_tiles[hash], index, (r << 16) | (g << 8) | b);
}

bool TileColorPack::AddPainting(const uint8_t *pixels, int width, int height, int channels,
                                const std::vector<uint8_t> &sidecar, ImportStats &stats)
{
    // The sidecar says what the painting covers: a screen capture (384x224
    // pixels) or a tile sheet (any size, e.g. the whole tile memory).
    // Version 2 adds a map cell per pixel after the records.
    const size_t headerSize = 16;
    const bool v2 = sidecar.size() >= headerSize && std::memcmp(sidecar.data(), kSidecarMagic, 8) == 0;
    if (sidecar.size() < headerSize || (!v2 && std::memcmp(sidecar.data(), kSidecarMagicV1, 8) != 0))
    {
        stats.lastError = "missing or unreadable .tiles file";
        ++stats.rejected;
        return false;
    }
    const uint32_t w = ReadLe32(&sidecar[8]), h = ReadLe32(&sidecar[12]);
    const size_t count = static_cast<size_t>(w) * h;
    if (w == 0 || h == 0 || w > 4096 || h > 4096 || sidecar.size() < headerSize + count * (v2 ? 12 : 8))
    {
        stats.lastError = "missing or unreadable .tiles file";
        ++stats.rejected;
        return false;
    }
    // Any size with the same shape as the capture (so a painting resized by
    // an editor still works); 3x is what captures are saved at.
    const double sx = static_cast<double>(width) / w, sy = static_cast<double>(height) / h;
    if (!pixels || channels < 3 || sx < 1.0 || sy < 1.0 || std::abs(sx / sy - 1.0) > 0.02)
    {
        stats.lastError = "image must be the reference's size (" + std::to_string(w * 3) + "x" + std::to_string(h * 3) +
                          ", or the same shape) - got " + std::to_string(width) + "x" + std::to_string(height);
        ++stats.rejected;
        return false;
    }
    const uint8_t *cells = v2 ? &sidecar[headerSize + count * 8] : nullptr;

    // The colors the capture showed (palette block after the tile
    // dictionary, if any): a pixel still in one of the 3 shade colors was
    // left unpainted, and so was a transparent one still in the background color.
    uint8_t shown[4][3];
    std::memcpy(shown, kDefaultCapturePalette, sizeof(shown));
    // Optional blocks after the palette, in any order:
    //  - what each pixel showed - pack colors included (captures show what
    //    the pack already colors, so painting can carry on from there): a
    //    pixel still showing it was left alone, and doesn't vote - a stray
    //    color the pack showed then isn't painted in again;
    //  - figure ids: a sheet of separate figures (one character's animation
    //    frames side by side), each one an object of its own - as if every
    //    figure had been painted on a screen of its own.
    const uint8_t *shownPixels = nullptr;
    const uint8_t *figureIds = nullptr;
    int level = -1;
    size_t offset = headerSize + count * (v2 ? 12 : 8);
    if (sidecar.size() >= offset + 4)
    {
        // The tiles' pixels (fills need to know which pixels are transparent).
        const size_t tiles = ReadLe32(&sidecar[offset]);
        for (size_t k = 0; k < tiles && sidecar.size() >= offset + 4 + (k + 1) * 20; ++k)
        {
            const uint8_t *e = &sidecar[offset + 4 + k * 20];
            std::array<uint16_t, 8> &rows = m_tileRows[ReadLe32(e)];
            for (int r = 0; r < 8; ++r)
                rows[r] = static_cast<uint16_t>(e[4 + r * 2] | (e[5 + r * 2] << 8));
        }
        offset += 4 + tiles * (4 + 16);
        const bool v2Palette = sidecar.size() >= offset + 8 + 13 && std::memcmp(&sidecar[offset], kPaletteMagic, 8) == 0;
        const bool v1Palette = !v2Palette && sidecar.size() >= offset + 8 + 12 &&
                               std::memcmp(&sidecar[offset], kPaletteMagicV1, 8) == 0;
        if (v2Palette || v1Palette)
            std::memcpy(shown, &sidecar[offset + 8], sizeof(shown));
        if (v2Palette)
            level = sidecar[offset + 20];
        offset += v2Palette ? 8 + 13 : v1Palette ? 8 + 12 : 0;
        while ((v2Palette || v1Palette) && sidecar.size() >= offset + 8)
        {
            if (std::memcmp(&sidecar[offset], kShownMagic, 8) == 0 && sidecar.size() >= offset + 8 + count * 3)
            {
                shownPixels = &sidecar[offset + 8];
                offset += 8 + count * 3;
            }
            else if (std::memcmp(&sidecar[offset], kFiguresMagic, 8) == 0 && sidecar.size() >= offset + 8 + count * 2)
            {
                figureIds = &sidecar[offset + 8];
                offset += 8 + count * 2;
            }
            else
                break;
        }
    }
    // A figure sheet votes like a screen painting, whatever its size; any
    // other painting that isn't exactly a screen is a tile sheet (fills in).
    const bool sheet = !figureIds && !(w == VBGO_TT_WIDTH && h == VBGO_TT_HEIGHT);
    if (!sheet && !figureIds && level >= 0 && level <= 63)
        m_paintingLevels.push_back(static_cast<uint8_t>(level));
    constexpr int kSameColor = 24;  // |dR|+|dG|+|dB| still counted as the capture's own color
    constexpr int kMagentaReach = 40;
    static const uint8_t kMagenta[3] = {255, 0, 255};

    // Objects (for context colors - see the header): connected drawn sprite
    // pixels of one layer. Background tiles mostly aren't - map cells already
    // tell their spots apart (a heading's letters vs. the next heading's) -
    // except a background layer that holds one figure and nothing else, clear
    // of the screen's sides (the players Mario's Tennis draws as backgrounds,
    // which share their solid tiles with every other player).
    constexpr size_t kMaxObjectPixels = 4096, kMaxObjectTiles = 96, kMinFigurePixels = 16;
    std::vector<int32_t> object;
    if (figureIds)
    {
        std::unordered_map<uint16_t, int32_t> idOf;
        std::vector<std::vector<uint32_t>> figureTiles;
        std::vector<bool> figureBg;
        object.assign(count, -1);
        for (size_t i = 0; i < count; ++i)
        {
            const uint16_t id = static_cast<uint16_t>(figureIds[i * 2] | (figureIds[i * 2 + 1] << 8));
            const uint64_t t = ReadLe64(&sidecar[headerSize + i * 8]);
            if (id == 0 || !VBGO_TT_VALID(t) || VBGO_TT_PIXEL(t) == 0)
                continue;
            const auto it = idOf.emplace(id, static_cast<int32_t>(figureTiles.size())).first;
            if (it->second == static_cast<int32_t>(figureTiles.size()))
            {
                figureTiles.emplace_back();
                figureBg.push_back(!VBGO_TT_IS_OBJ(t));
            }
            figureTiles[it->second].push_back(VBGO_TT_HASH(t));
            object[i] = it->second; // made global below
        }
        const int32_t first = static_cast<int32_t>(m_objectTiles.size());
        const int32_t family = m_families++; // one character's frames: one family
        for (size_t k = 0; k < figureTiles.size(); ++k)
        {
            std::vector<uint32_t> &tiles = figureTiles[k];
            std::sort(tiles.begin(), tiles.end());
            tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
            m_objectTiles.push_back(std::move(tiles));
            m_objectIsFigure.push_back(figureBg[k]);
            m_objectFamily.push_back(family);
        }
        for (int32_t &o : object)
            if (o >= 0)
                o += first;
    }
    else if (!sheet)
    {
        std::vector<int32_t> parent(count, -1);
        std::vector<uint8_t> layerOf(count, 0xFF); // world; +32 for background pixels
        auto root = [&parent](int32_t i) {
            while (parent[i] != i)
                i = parent[i] = parent[parent[i]];
            return i;
        };
        for (size_t i = 0; i < count; ++i)
        {
            const uint64_t t = ReadLe64(&sidecar[headerSize + i * 8]);
            if (VBGO_TT_VALID(t) && VBGO_TT_PIXEL(t) != 0)
            {
                layerOf[i] = static_cast<uint8_t>(VBGO_TT_WORLD(t) + (VBGO_TT_IS_OBJ(t) ? 0 : 32));
                parent[i] = static_cast<int32_t>(i);
            }
        }
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const size_t i = static_cast<size_t>(y) * w + x;
                if (layerOf[i] == 0xFF)
                    continue;
                const int dxs[4] = {-1, -1, 0, 1}, dys[4] = {0, -1, -1, -1};
                for (int k = 0; k < 4; ++k)
                {
                    const int nx = static_cast<int>(x) + dxs[k], ny = static_cast<int>(y) + dys[k];
                    if (nx < 0 || ny < 0 || nx >= static_cast<int>(w))
                        continue;
                    const size_t j = static_cast<size_t>(ny) * w + nx;
                    if (layerOf[j] == layerOf[i])
                        parent[root(static_cast<int32_t>(j))] = root(static_cast<int32_t>(i));
                }
            }
        // Groups in screen order (their first pixel), so object numbers don't
        // depend on hashing.
        std::unordered_map<int32_t, size_t> groupOf;
        std::vector<std::pair<int32_t, std::vector<size_t>>> members;
        for (size_t i = 0; i < count; ++i)
            if (parent[i] >= 0)
            {
                const int32_t r = root(static_cast<int32_t>(i));
                const auto it = groupOf.emplace(r, members.size()).first;
                if (it->second == members.size())
                    members.emplace_back(r, std::vector<size_t>());
                members[it->second].second.push_back(i);
            }
        // Background layers holding a single figure, clear of the screen's sides.
        size_t figures[32] = {};
        bool atSide[32] = {};
        for (const auto &group : members)
        {
            const uint8_t layer = layerOf[group.first];
            if (layer < 32 || group.second.size() < kMinFigurePixels)
                continue;
            ++figures[layer - 32];
            for (const size_t i : group.second)
                if (i % w == 0 || i % w == w - 1 || i < w)
                    atSide[layer - 32] = true;
        }
        object.assign(count, -1);
        for (const auto &group : members)
        {
            const uint8_t layer = layerOf[group.first];
            if (layer >= 32 && (figures[layer - 32] != 1 || atSide[layer - 32] || group.second.size() < kMinFigurePixels))
                continue;
            if (group.second.size() > kMaxObjectPixels)
                continue;
            std::vector<uint32_t> tiles;
            for (const size_t i : group.second)
                tiles.push_back(VBGO_TT_HASH(ReadLe64(&sidecar[headerSize + i * 8])));
            std::sort(tiles.begin(), tiles.end());
            tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
            if (tiles.size() > kMaxObjectTiles)
                continue;
            const int32_t id = static_cast<int32_t>(m_objectTiles.size());
            m_objectTiles.push_back(std::move(tiles));
            m_objectIsFigure.push_back(layer >= 32);
            m_objectFamily.push_back(-1);
            for (const size_t i : group.second)
                object[i] = id;
        }
        // Background tiles drawn outside figures: they can't tell a figure
        // apart (a figure's tile that also turns up in the scenery or a row
        // of portraits isn't one of its markers).
        for (size_t i = 0; i < count; ++i)
            if (layerOf[i] != 0xFF && layerOf[i] >= 32 && object[i] < 0)
                m_looseTiles.insert(VBGO_TT_HASH(ReadLe64(&sidecar[headerSize + i * 8])));
    }

    // Sample the middle of each pixel's block.
    for (uint32_t y = 0; y < h; ++y)
    {
        const int py = std::min(height - 1, static_cast<int>((y + 0.5) * sy));
        for (uint32_t x = 0; x < w; ++x)
        {
            const size_t i = static_cast<size_t>(y) * w + x;
            const uint64_t t = ReadLe64(&sidecar[headerSize + i * 8]);
            if (!VBGO_TT_VALID(t))
                continue; // background - not part of any tile
            const uint32_t cell = cells ? ReadLe32(&cells[i * 4]) : 0;
            const bool fill = VBGO_TT_PIXEL(t) == 0;
            if (fill && (sheet || !VBGO_TT_CELL_VALID(cell)))
                continue; // fills only count at their map cell
            const int px = std::min(width - 1, static_cast<int>((x + 0.5) * sx));
            const uint8_t *p = &pixels[(static_cast<size_t>(py) * width + px) * channels];
            if (!fill && (Distance(p, shown[1]) <= kSameColor || Distance(p, shown[2]) <= kSameColor ||
                          Distance(p, shown[3]) <= kSameColor))
                continue; // left as captured - not painted
            if (shownPixels && Distance(p, &shownPixels[i * 3]) <= kSameColor &&
                (!fill || Distance(&shownPixels[i * 3], shown[0]) > kSameColor))
                continue; // still the pack's color it was captured with (a background left as such still counts)
            // (A transparent pixel left as the background still counts: it
            // says where a painted fill ends.)
            const uint32_t rgb = fill && Distance(p, shown[0]) <= kSameColor ? kBackground
                                 : Distance(p, kMagenta) <= kMagentaReach   ? kNoColor
                                                                            : (p[0] << 16) | (p[1] << 8) | p[2];
            const uint64_t key = (static_cast<uint64_t>(VBGO_TT_HASH(t)) << 6) | (VBGO_TT_SUBY(t) * 8 + VBGO_TT_SUBX(t));
            if (VBGO_TT_CELL_VALID(cell))
                m_cellVotes.push_back({(static_cast<uint64_t>(VBGO_TT_CELL(cell)) << 40) |
                                           (static_cast<uint64_t>(VBGO_TT_PALETTE(t)) << 38) | key,
                                       rgb, VBGO_TT_WORLD(t) | (fill ? kFillVote : 0)});
            if (fill)
                continue;
            if (sheet)
            {
                m_sheetVotes.push_back({key, rgb, 0});
                continue;
            }
            m_votes.push_back({key, rgb, 0});
            if (!object.empty() && object[i] >= 0 && rgb < kNoColor)
            {
                m_contextVotes.push_back({key, rgb, static_cast<uint32_t>(object[i])});
                auto &shades = m_tileShades[VBGO_TT_HASH(t)];
                if (shades[0] == 0) // new entry: all unknown
                    shades.fill(0xFF);
                shades[key & 63] = static_cast<uint8_t>(VBGO_TT_PIXEL(t));
            }
            m_layerVotes.push_back({(key << 5) | VBGO_TT_WORLD(t), rgb, 0});
            m_paletteVotes.push_back({(key << 2) | VBGO_TT_PALETTE(t), rgb, 0});
        }
    }
    ++(sheet ? stats.sheets : stats.paintings);
    return true;
}

void TileColorPack::CompleteFills(CellTile &cell, uint64_t knownBackground) const
{
    // A painting only shows some of a fill's pixels - a scaled layer (a court
    // in perspective) skips texture rows in the distance, and other camera
    // angles show the ones it skipped. Every transparent pixel nobody painted
    // or saw left as the background takes the nearest known one's: painted
    // color, or background.
    const auto rows = m_tileRows.find(cell.hash);
    if (rows == m_tileRows.end())
        return;
    uint64_t transparent = 0;
    for (unsigned i = 0; i < 64; ++i)
        if (!((rows->second[i >> 3] >> ((i & 7) * 2)) & 3))
            transparent |= 1ull << i;
    const uint64_t filled = cell.mask & transparent, known = filled | (knownBackground & transparent);
    if (!filled || known == transparent)
        return;
    for (unsigned i = 0; i < 64; ++i)
    {
        if (!(transparent >> i & 1) || (known >> i & 1))
            continue;
        int bestDistance = 99;
        unsigned best = 64;
        for (unsigned j = 0; j < 64; ++j)
            if (known >> j & 1)
            {
                const int d = std::abs(static_cast<int>(i & 7) - static_cast<int>(j & 7)) +
                              std::abs(static_cast<int>(i >> 3) - static_cast<int>(j >> 3));
                if (d < bestDistance)
                    bestDistance = d, best = j;
            }
        if (best < 64 && (filled >> best & 1))
            Store(cell, i, (cell.rgb[best][0] << 16) | (cell.rgb[best][1] << 8) | cell.rgb[best][2]);
    }
}

void TileColorPack::ResolveContexts(ImportStats &stats)
{
    // A tile some objects paint clearly otherwise than its usual colors
    // (Skelton's brown hat brim, a tile Lantern's green hat has too): those
    // objects' colors become a context variant, switched on by marker tiles -
    // the objects' own tiles that no other object with that tile has.
    m_contextGroups.clear();
    m_contextTiles.clear();
    m_contextLayerBound = 0;
    if (m_contextVotes.empty() || m_objectTiles.empty())
        return;
    constexpr int kDiffers = 60; // |dR|+|dG|+|dB| that's another color, not just another brush shade
    auto distance = [](uint32_t a, uint32_t b) {
        return std::abs(static_cast<int>((a >> 16) & 255) - static_cast<int>((b >> 16) & 255)) +
               std::abs(static_cast<int>((a >> 8) & 255) - static_cast<int>((b >> 8) & 255)) +
               std::abs(static_cast<int>(a & 255) - static_cast<int>(b & 255));
    };
    auto rgbOf = [](const uint8_t *c) { return (static_cast<uint32_t>(c[0]) << 16) | (static_cast<uint32_t>(c[1]) << 8) | c[2]; };
    std::sort(m_contextVotes.begin(), m_contextVotes.end(), [](const Vote &a, const Vote &b) {
        if ((a.key >> 6) != (b.key >> 6))
            return (a.key >> 6) < (b.key >> 6);
        if (a.extra != b.extra)
            return a.extra < b.extra;
        return a.key != b.key ? a.key < b.key : a.rgb < b.rgb;
    });

    struct Object
    {
        uint32_t id = 0;
        uint64_t mask = 0;
        uint32_t rgb[64] = {};
        size_t painted = 0, differs = 0;
    };
    struct Pending
    {
        uint32_t hash;
        std::vector<uint32_t> markers;
        Tile tile;
        size_t pixels;
        uint64_t painted = 0; // pixels the objects showed
        bool figures = false; // only background figures paint it so (their markers count on their own layer)
        int32_t family = -1;  // all its objects are frames of one figure sheet's character
    };
    std::vector<Pending> pending;
    const size_t total = m_contextVotes.size();
    for (size_t run = 0; run < total;)
    {
        const uint64_t hashKey = m_contextVotes[run].key >> 6;
        size_t end = run;
        while (end < total && (m_contextVotes[end].key >> 6) == hashKey)
            ++end;
        const uint32_t hash = static_cast<uint32_t>(hashKey);
        const Tile *base = Find(hash);
        std::vector<Object> objects;
        for (size_t i = run; base && i < end;)
        {
            Object o;
            o.id = m_contextVotes[i].extra;
            while (i < end && m_contextVotes[i].extra == o.id)
            {
                size_t next = i;
                while (next < end && m_contextVotes[next].extra == o.id && m_contextVotes[next].key == m_contextVotes[i].key)
                    ++next;
                uint32_t bestVotes, allVotes;
                size_t distinct;
                const uint32_t color = Winner(m_contextVotes.begin() + i, m_contextVotes.begin() + next, bestVotes, allVotes, distinct);
                const unsigned px = m_contextVotes[i].key & 63;
                o.mask |= 1ull << px;
                o.rgb[px] = color;
                ++o.painted;
                if ((base->mask >> px & 1) && distance(color, rgbOf(base->rgb[px])) > kDiffers)
                    ++o.differs;
                i = next;
            }
            objects.push_back(o);
        }
        run = end;
        // Sprites and background figures apart: a figure's colors never come
        // into play by a sprite's markers, nor a sprite's by a figure's.
        const std::vector<Object> all = std::move(objects);
        for (int kind = 0; kind < 2; ++kind)
        {
            const bool figures = kind == 1;
            std::vector<Object> objects;
            for (const Object &o : all)
                if ((o.id < m_objectIsFigure.size() && m_objectIsFigure[o.id] != 0) == figures)
                    objects.push_back(o);
            if (objects.size() < 2)
                continue;

            // The objects that paint it otherwise, grouped by how.
            std::vector<std::vector<size_t>> clusters;
            std::vector<int> clusterOf(objects.size(), -1);
            for (size_t a = 0; a < objects.size(); ++a)
            {
                const Object &o = objects[a];
                if (o.differs < 3 || o.differs * 4 < o.painted)
                    continue;
                for (size_t c = 0; c < clusters.size() && clusterOf[a] < 0; ++c)
                {
                    const Object &r = objects[clusters[c][0]];
                    const uint64_t common = o.mask & r.mask;
                    size_t same = 0, n = 0;
                    for (unsigned px = 0; px < 64; ++px)
                        if (common >> px & 1)
                        {
                            ++n;
                            same += distance(o.rgb[px], r.rgb[px]) <= kDiffers;
                        }
                    if (n && same * 4 >= n * 3)
                    {
                        clusters[c].push_back(a);
                        clusterOf[a] = static_cast<int>(c);
                    }
                }
                if (clusterOf[a] < 0)
                {
                    clusterOf[a] = static_cast<int>(clusters.size());
                    clusters.push_back({a});
                }
            }
            for (size_t c = 0; c < clusters.size(); ++c)
            {
                std::vector<uint32_t> others, markers;
                for (size_t a = 0; a < objects.size(); ++a)
                    if (clusterOf[a] != static_cast<int>(c))
                        others.insert(others.end(), m_objectTiles[objects[a].id].begin(), m_objectTiles[objects[a].id].end());
                std::sort(others.begin(), others.end());
                for (const size_t a : clusters[c])
                    for (const uint32_t m : m_objectTiles[objects[a].id])
                        if (m != hash && !std::binary_search(others.begin(), others.end(), m))
                            markers.push_back(m);
                std::sort(markers.begin(), markers.end());
                markers.erase(std::unique(markers.begin(), markers.end()), markers.end());
                if (figures)
                {
                    // A figure's markers also mustn't turn up in the scenery, nor in
                    // any other figure that isn't another frame of it (one sharing
                    // less than a third of its tiles with the cluster's).
                    std::vector<uint32_t> own;
                    for (const size_t a : clusters[c])
                        own.insert(own.end(), m_objectTiles[objects[a].id].begin(), m_objectTiles[objects[a].id].end());
                    std::sort(own.begin(), own.end());
                    own.erase(std::unique(own.begin(), own.end()), own.end());
                    std::vector<uint32_t> foreign;
                    for (size_t o = 0; o < m_objectTiles.size(); ++o)
                    {
                        if (!m_objectIsFigure[o])
                            continue;
                        const auto &tiles = m_objectTiles[o];
                        size_t shared = 0;
                        for (const uint32_t t : tiles)
                            shared += std::binary_search(own.begin(), own.end(), t);
                        if (shared * 3 < tiles.size())
                            foreign.insert(foreign.end(), tiles.begin(), tiles.end());
                    }
                    std::sort(foreign.begin(), foreign.end());
                    markers.erase(std::remove_if(markers.begin(), markers.end(),
                                                 [this, &foreign](uint32_t m) {
                                                     return m_looseTiles.count(m) != 0 || std::binary_search(foreign.begin(), foreign.end(), m);
                                                 }),
                                  markers.end());
                }
                if (markers.empty())
                    continue;
                // The objects' colors (their majority per pixel), the tile's own elsewhere.
                Pending p{hash, std::move(markers), *base, 0, 0};
                p.figures = figures;
                p.family = m_objectFamily[objects[clusters[c][0]].id];
                for (const size_t a : clusters[c])
                    if (m_objectFamily[objects[a].id] != p.family)
                        p.family = -1;
                for (unsigned px = 0; px < 64; ++px)
                {
                    std::vector<uint32_t> colors;
                    for (const size_t a : clusters[c])
                        if (objects[a].mask >> px & 1)
                            colors.push_back(objects[a].rgb[px]);
                    if (colors.empty())
                        continue;
                    std::sort(colors.begin(), colors.end());
                    uint32_t best = colors[0];
                    size_t bestCount = 0;
                    for (size_t i = 0; i < colors.size();)
                    {
                        size_t j = i;
                        while (j < colors.size() && colors[j] == colors[i])
                            ++j;
                        if (j - i > bestCount)
                            best = colors[i], bestCount = j - i;
                        i = j;
                    }
                    Store(p.tile, px, best);
                    p.painted |= 1ull << px;
                    ++p.pixels;
                }
                // Pixels these objects never showed (covered, cut off): the color
                // they gave the same shade elsewhere in the tile, rather than the
                // other objects' colors.
                const auto shades = m_tileShades.find(hash);
                if (shades != m_tileShades.end())
                {
                    auto shadeOf = [&shades](unsigned px) { return shades->second[px]; };
                    for (unsigned px = 0; px < 64; ++px)
                    {
                        if ((p.painted >> px & 1) || !(base->mask >> px & 1))
                            continue;
                        uint32_t counts[64] = {}, best = 64, bestCount = 0;
                        for (unsigned q = 0; q < 64; ++q)
                            if ((p.painted >> q & 1) && shadeOf(q) == shadeOf(px))
                                for (unsigned r = 0; r <= q; ++r) // the first pixel with that color counts them
                                    if ((p.painted >> r & 1) && shadeOf(r) == shadeOf(px) &&
                                        std::memcmp(p.tile.rgb[r], p.tile.rgb[q], 3) == 0)
                                    {
                                        if (++counts[r] > bestCount)
                                            best = r, bestCount = counts[r];
                                        break;
                                    }
                        if (best < 64)
                            Store(p.tile, px, rgbOf(p.tile.rgb[best]));
                    }
                }
                pending.push_back(std::move(p));
            }
        }
    }
    if (pending.empty())
        return;

    // Variants whose markers overlap belong to the same object (Skelton's
    // hat brim and his coat): one group, all their markers together.
    std::vector<size_t> parent(pending.size());
    for (size_t i = 0; i < parent.size(); ++i)
        parent[i] = i;
    auto root = [&parent](size_t i) {
        while (parent[i] != i)
            i = parent[i] = parent[parent[i]];
        return i;
    };
    std::unordered_map<uint64_t, size_t> firstWith; // (marker, figures) -> first variant with it
    for (size_t i = 0; i < pending.size(); ++i)
        for (const uint32_t m : pending[i].markers)
        {
            const auto it = firstWith.emplace(static_cast<uint64_t>(m) | (static_cast<uint64_t>(pending[i].figures) << 32), i).first;
            parent[root(i)] = root(it->second);
        }
    // A figure sheet's character is one object too: its frames' variants go
    // together (all its markers switch its colors on) - except a tile it
    // paints two ways (a shirt's white and a cap's red on one solid tile),
    // whose second way stays with the frames showing it.
    std::unordered_map<uint64_t, size_t> familyGroup; // (family, nth variant of a tile in it) -> first variant
    std::unordered_map<uint64_t, uint32_t> seenInFamily; // (family, tile) -> variants so far
    for (size_t i = 0; i < pending.size(); ++i)
    {
        if (pending[i].family < 0)
            continue;
        const uint64_t fam = static_cast<uint64_t>(pending[i].family);
        const uint32_t nth = seenInFamily[fam << 32 | pending[i].hash]++;
        const auto it = familyGroup.emplace(fam << 32 | nth, i).first;
        parent[root(i)] = root(it->second);
    }
    std::unordered_map<size_t, std::pair<size_t, size_t>> groups; // root -> (pixels, group index)
    for (size_t i = 0; i < pending.size(); ++i)
        groups[root(i)].first += pending[i].pixels;
    std::vector<std::pair<size_t, size_t>> order; // (-pixels, root)
    for (const auto &g : groups)
        order.emplace_back(g.second.first, g.first);
    std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    if (order.size() > kMaxContextGroups)
        order.resize(kMaxContextGroups);
    // Deterministic group order: by their smallest marker.
    std::vector<std::pair<uint32_t, size_t>> byMarker;
    for (const auto &o : order)
    {
        uint32_t smallest = ~0u;
        for (size_t i = 0; i < pending.size(); ++i)
            if (root(i) == o.second)
                smallest = std::min(smallest, pending[i].markers.front());
        byMarker.emplace_back(smallest, o.second);
    }
    std::sort(byMarker.begin(), byMarker.end());
    for (const auto &g : byMarker)
    {
        const uint32_t index = static_cast<uint32_t>(m_contextGroups.size());
        std::vector<uint32_t> markers;
        bool figures = true;
        for (size_t i = 0; i < pending.size(); ++i)
            if (root(i) == g.second)
            {
                markers.insert(markers.end(), pending[i].markers.begin(), pending[i].markers.end());
                m_contextTiles.push_back({pending[i].hash, index, pending[i].tile});
                figures = figures && pending[i].figures;
            }
        if (figures)
            m_contextLayerBound |= 1ull << index;
        std::sort(markers.begin(), markers.end());
        markers.erase(std::unique(markers.begin(), markers.end()), markers.end());
        m_contextGroups.push_back(std::move(markers));
    }
    std::sort(m_contextTiles.begin(), m_contextTiles.end(),
              [](const ContextTile &a, const ContextTile &b) { return a.hash != b.hash ? a.hash < b.hash : a.group < b.group; });
    stats.contextTiles = m_contextTiles.size();
    stats.contextGroups = m_contextGroups.size();
}

void TileColorPack::FinishImport(ImportStats &stats)
{
    auto byKeyThenColor = [](const Vote &a, const Vote &b) { return a.key != b.key ? a.key < b.key : a.rgb < b.rgb; };

    // Tile sheets only fill in: a tile pixel any screen painting colored
    // keeps the screen paintings' color.
    std::sort(m_votes.begin(), m_votes.end(), byKeyThenColor);
    if (!m_sheetVotes.empty())
    {
        std::sort(m_sheetVotes.begin(), m_sheetVotes.end(), byKeyThenColor);
        const size_t screenVotes = m_votes.size();
        uint64_t lastKey = ~0ull;
        for (const Vote &vote : m_sheetVotes)
        {
            const bool painted = std::binary_search(m_votes.begin(), m_votes.begin() + screenVotes, vote,
                                                    [](const Vote &a, const Vote &b) { return a.key < b.key; });
            if (painted)
                continue;
            m_votes.push_back(vote);
            if (vote.key != lastKey)
                ++stats.fromSheets;
            lastKey = vote.key;
        }
        m_sheetVotes.clear();
        std::sort(m_votes.begin(), m_votes.end(), byKeyThenColor);
    }

    // Cleanup 1: merge stray near-duplicate shades (a slightly-off brush
    // color on a handful of pixels) into the closest color the painter used
    // a lot - never introduces a color that isn't already in the paintings.
    std::unordered_map<uint32_t, uint64_t> usage;
    uint64_t totalVotes = 0;
    auto use = [&](const Vote &vote) {
        if (vote.rgb < kNoColor)
        {
            ++usage[vote.rgb];
            ++totalVotes;
        }
    };
    for (const Vote &vote : m_votes)
        use(vote);
    for (const Vote &vote : m_cellVotes)
        if (vote.extra & kFillVote)
            use(vote);
    const uint64_t rareBelow = std::max<uint64_t>(20, totalVotes / 2000);
    constexpr int kMergeDistance = 40; // sum of |dR|+|dG|+|dB|
    // The common colors, bucketed by 16x16x16 color cells, so each rare
    // color only looks at the cells within reach (a painting can hold tens
    // of thousands of slightly different shades - e.g. a smoothed one).
    constexpr int kCell = 16, kReach = (kMergeDistance + kCell - 1) / kCell;
    auto cellOf = [](int r, int g, int b) { return (r / kCell) * 256 + (g / kCell) * 16 + b / kCell; };
    std::vector<std::vector<uint32_t>> cells(16 * 16 * 16);
    for (const auto &color : usage)
        if (color.second >= rareBelow)
            cells[cellOf((color.first >> 16) & 255, (color.first >> 8) & 255, color.first & 255)].push_back(color.first);
    std::unordered_map<uint32_t, uint32_t> merged;
    for (const auto &color : usage)
    {
        if (color.second >= rareBelow)
            continue;
        const int r = (color.first >> 16) & 255, g = (color.first >> 8) & 255, b = color.first & 255;
        uint32_t best = color.first;
        int bestDistance = kMergeDistance + 1;
        uint64_t bestUsage = 0;
        for (int cr = std::max(0, r / kCell - kReach); cr <= std::min(15, r / kCell + kReach); ++cr)
            for (int cg = std::max(0, g / kCell - kReach); cg <= std::min(15, g / kCell + kReach); ++cg)
                for (int cb = std::max(0, b / kCell - kReach); cb <= std::min(15, b / kCell + kReach); ++cb)
                    for (const uint32_t other : cells[cr * 256 + cg * 16 + cb])
                    {
                        const int d = std::abs(r - static_cast<int>((other >> 16) & 255)) +
                                      std::abs(g - static_cast<int>((other >> 8) & 255)) +
                                      std::abs(b - static_cast<int>(other & 255));
                        const uint64_t u = usage[other];
                        // closest; ties: the more used, then the lower value (same result everywhere)
                        if (d < bestDistance || (d == bestDistance && (u > bestUsage || (u == bestUsage && other < best))))
                        {
                            best = other;
                            bestDistance = d;
                            bestUsage = u;
                        }
                    }
        if (bestDistance <= kMergeDistance)
        {
            merged[color.first] = best;
            ++stats.mergedColors;
        }
    }
    auto remap = [&merged](std::vector<Vote> &votes, auto order) {
        if (!merged.empty())
            for (Vote &vote : votes)
            {
                const auto m = merged.find(vote.rgb);
                if (m != merged.end())
                    vote.rgb = m->second;
            }
        std::sort(votes.begin(), votes.end(), order);
    };
    remap(m_votes, byKeyThenColor);
    remap(m_layerVotes, byKeyThenColor);
    remap(m_paletteVotes, byKeyThenColor);
    remap(m_cellVotes, byKeyThenColor);
    remap(m_contextVotes, byKeyThenColor);

    // Cleanup 2: one color per tile pixel - the most-voted one across every
    // place the tile appears in every painting.
    m_tiles.clear();
    m_layerTiles.clear();
    m_paletteTiles.clear();
    m_leftUncolored.clear();
    m_cellTiles.clear();
    Chosen chosen;
    ForEachKey(m_votes, [&](auto begin, auto end) {
        uint32_t bestVotes, allVotes;
        size_t distinct;
        const uint32_t best = Winner(begin, end, bestVotes, allVotes, distinct);
        const uint64_t key = begin->key;
        chosen.entries.emplace_back(key, best);
        if (distinct > 1)
            ++stats.inconsistent;
        if (best == kNoColor)
        {
            m_leftUncolored[static_cast<uint32_t>(key >> 6)] |= 1ull << (key & 63);
            ++stats.erased;
            return;
        }
        Store(m_tiles[static_cast<uint32_t>(key >> 6)], key & 63, best);
        ++stats.tilePixels;
    });

    // Cleanup 3: a palette or layer that consistently shows a tile pixel in
    // another color than the overall choice (at least twice, two thirds of
    // its votes) keeps that color for itself - a menu option drawn in its
    // "not selected" palette, a tile reused in different places.
    auto variants = [&](std::vector<Vote> &votes, unsigned bits, std::unordered_map<uint64_t, Tile> &into, size_t &counted,
                        Chosen *record) {
        ForEachKey(votes, [&](auto begin, auto end) {
            const uint64_t key = begin->key >> bits;
            uint32_t overall;
            if (!chosen.Get(key, overall) || overall == kNoColor)
                return;
            uint32_t bestVotes, allVotes;
            size_t distinct;
            const uint32_t best = Winner(begin, end, bestVotes, allVotes, distinct);
            if (best == kNoColor || best == overall || bestVotes < 2 || bestVotes * 3 < allVotes * 2)
                return;
            const uint64_t variant = begin->key & ((1ull << bits) - 1);
            Store(into[((key >> 6) << bits) | variant], key & 63, best);
            if (record)
                record->entries.emplace_back(begin->key, best);
            ++counted;
        });
        // A variant only lists the pixels that differ - fill in the rest from
        // the overall colors, so a lookup needs just one tile.
        for (auto &entry : into)
        {
            const auto base = m_tiles.find(static_cast<uint32_t>(entry.first >> bits));
            if (base == m_tiles.end())
                continue;
            for (unsigned i = 0; i < 64; ++i)
                if ((base->second.mask >> i & 1) && !(entry.second.mask >> i & 1))
                    Store(entry.second, i, (base->second.rgb[i][0] << 16) | (base->second.rgb[i][1] << 8) | base->second.rgb[i][2]);
        }
    };
    Chosen paletteChosen;
    variants(m_paletteVotes, 2, m_paletteTiles, stats.palettePixels, &paletteChosen);
    variants(m_layerVotes, 5, m_layerTiles, stats.layerPixels, nullptr);

    // Cleanup 4: map cells. Wherever a painting shows a background tile at a
    // fixed spot, the color painted there wins at that spot if it differs
    // from what the tile gets anyway (palette variant, else the tile's own) -
    // and that's the only place fills (transparent pixels painted over) live.
    CellTile current;
    uint64_t knownBackground = 0; // transparent pixels of the current entry seen left as the background
    bool open = false;
    auto flush = [&]() {
        if (open && (current.mask || current.keep))
        {
            CompleteFills(current, knownBackground);
            m_cellTiles.push_back(current);
        }
        open = false;
    };
    ForEachKey(m_cellVotes, [&](auto begin, auto end) {
        const uint64_t key = begin->key;
        const uint64_t tileKey = key >> 6; // cell, palette, hash
        if (!open || (static_cast<uint64_t>(current.cell) << 34 | static_cast<uint64_t>(current.palette) << 32 | current.hash) != tileKey)
        {
            flush();
            current = CellTile{};
            knownBackground = 0;
            current.cell = static_cast<uint16_t>(key >> 40);
            current.palette = static_cast<uint8_t>((key >> 38) & 3);
            current.hash = static_cast<uint32_t>(key >> 6);
            open = true;
        }
        uint32_t bestVotes, allVotes;
        size_t distinct;
        const uint32_t best = Winner(begin, end, bestVotes, allVotes, distinct);
        const unsigned index = key & 63;
        if (begin->extra & kFillVote)
        {
            if (best == kNoColor || best == kBackground)
                knownBackground |= 1ull << index;
            else
            {
                Store(current, index, best);
                ++stats.fillPixels;
            }
            return;
        }
        const uint64_t pixelKey = key & ((1ull << 38) - 1); // hash << 6 | pixel
        uint32_t expected;
        if (!paletteChosen.Get((pixelKey << 2) | current.palette, expected) && !chosen.Get(pixelKey, expected))
            expected = kNoColor;
        if (best == expected)
            return;
        if (best == kNoColor)
            current.keep |= 1ull << index;
        else
            Store(current, index, best);
        ++stats.cellPixels;
    });
    flush();

    // Cleanup 5: shared tiles that objects paint differently (context).
    ResolveContexts(stats);

    m_tileRows.clear();

    // The paintings' usual brightness (median), if their captures said.
    m_referenceLevel = 63;
    if (!m_paintingLevels.empty())
    {
        std::nth_element(m_paintingLevels.begin(), m_paintingLevels.begin() + m_paintingLevels.size() / 2, m_paintingLevels.end());
        m_referenceLevel = std::max<uint8_t>(1, m_paintingLevels[m_paintingLevels.size() / 2]);
    }
    m_paintingLevels.clear();

    m_votes.clear();
    m_layerVotes.clear();
    m_paletteVotes.clear();
    m_cellVotes.clear();
    m_contextVotes.clear();
    m_objectTiles.clear();
    m_objectIsFigure.clear();
    m_objectFamily.clear();
    m_families = 0;
    m_looseTiles.clear();
    m_tileShades.clear();
    m_contextVotes.shrink_to_fit();
    m_votes.shrink_to_fit();
    m_layerVotes.shrink_to_fit();
    m_paletteVotes.shrink_to_fit();
    m_cellVotes.shrink_to_fit();
}

std::vector<uint8_t> TileColorPack::Serialize() const
{
    // "VBGOCP03", tile count, then per tile: hash, mask, 64 x RGB; then the
    // per-layer colors: count, then per entry: hash, world, mask, 64 x RGB;
    // then the pixels left uncolored on purpose: count, (hash, mask) each;
    // then the per-palette colors: count, (hash, palette, mask, 64 x RGB)
    // each; then the map cells: count, (cell, palette, hash, mask, keep,
    // 64 x RGB) each; then the reference brightness level. Sorted, so the
    // same pack always gives the same bytes.
    auto appendTile = [](std::vector<uint8_t> &out, const Tile &tile) {
        AppendLe(out, tile.mask, 8);
        out.insert(out.end(), &tile.rgb[0][0], &tile.rgb[0][0] + sizeof(tile.rgb));
    };
    std::vector<uint32_t> hashes;
    hashes.reserve(m_tiles.size());
    for (const auto &entry : m_tiles)
        hashes.push_back(entry.first);
    std::sort(hashes.begin(), hashes.end());
    std::vector<uint8_t> out(kPackMagic, kPackMagic + 8);
    AppendLe(out, m_tiles.size(), 4);
    for (const uint32_t hash : hashes)
    {
        AppendLe(out, hash, 4);
        appendTile(out, m_tiles.at(hash));
    }
    auto appendVariants = [&](const std::unordered_map<uint64_t, Tile> &variants, unsigned bits) {
        std::vector<uint64_t> keys;
        for (const auto &entry : variants)
            keys.push_back(entry.first);
        std::sort(keys.begin(), keys.end());
        AppendLe(out, keys.size(), 4);
        for (const uint64_t key : keys)
        {
            AppendLe(out, key >> bits, 4);
            AppendLe(out, key & ((1ull << bits) - 1), 4);
            appendTile(out, variants.at(key));
        }
    };
    appendVariants(m_layerTiles, 5);
    std::vector<uint32_t> left;
    for (const auto &entry : m_leftUncolored)
        left.push_back(entry.first);
    std::sort(left.begin(), left.end());
    AppendLe(out, left.size(), 4);
    for (const uint32_t hash : left)
    {
        AppendLe(out, hash, 4);
        AppendLe(out, m_leftUncolored.at(hash), 8);
    }
    appendVariants(m_paletteTiles, 2);
    AppendLe(out, m_cellTiles.size(), 4);
    for (const CellTile &cell : m_cellTiles)
    {
        AppendLe(out, cell.cell, 2);
        AppendLe(out, cell.palette, 2);
        AppendLe(out, cell.hash, 4);
        AppendLe(out, cell.mask, 8);
        AppendLe(out, cell.keep, 8);
        out.insert(out.end(), &cell.rgb[0][0], &cell.rgb[0][0] + sizeof(cell.rgb));
    }
    AppendLe(out, m_referenceLevel, 4);
    // Optional (older builds stop reading before it): context variants -
    // "VBGOCTX1", group count, per group: marker count, markers; then
    // variant count, (hash, group, mask, 64 x RGB) each.
    if (!m_contextGroups.empty())
    {
        out.insert(out.end(), kContextMagic, kContextMagic + 8);
        AppendLe(out, m_contextGroups.size(), 4);
        for (const auto &markers : m_contextGroups)
        {
            AppendLe(out, markers.size(), 4);
            for (const uint32_t m : markers)
                AppendLe(out, m, 4);
        }
        AppendLe(out, m_contextTiles.size(), 4);
        for (const ContextTile &c : m_contextTiles)
        {
            AppendLe(out, c.hash, 4);
            AppendLe(out, c.group, 4);
            appendTile(out, c.tile);
        }
        // Optional again: "VBGOCTXL", the groups (bit per group) whose markers
        // only count on the layer they're drawn on.
        if (m_contextLayerBound)
        {
            out.insert(out.end(), kLayerBoundMagic, kLayerBoundMagic + 8);
            AppendLe(out, m_contextLayerBound, 8);
        }
    }
    return out;
}

bool TileColorPack::Deserialize(const std::vector<uint8_t> &bytes)
{
    Clear();
    constexpr size_t kTileBytes = 8 + 64 * 3;
    if (bytes.size() < 12)
        return false;
    const int version = std::memcmp(bytes.data(), kPackMagic, 8) == 0     ? 3
                        : std::memcmp(bytes.data(), kPackMagicV2, 8) == 0 ? 2
                        : std::memcmp(bytes.data(), kPackMagicV1, 8) == 0 ? 1
                                                                          : 0;
    if (!version)
        return false;
    size_t offset = 8;
    auto have = [&](size_t n) { return bytes.size() >= offset + n; };
    auto readTile = [&](Tile &tile) {
        tile.mask = ReadLe64(&bytes[offset]);
        std::memcpy(tile.rgb, &bytes[offset + 8], sizeof(tile.rgb));
        offset += kTileBytes;
    };
    const uint32_t count = ReadLe32(&bytes[offset]);
    offset += 4;
    if (!have(static_cast<size_t>(count) * (4 + kTileBytes)))
        return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t hash = ReadLe32(&bytes[offset]);
        offset += 4;
        readTile(m_tiles[hash]);
    }
    auto readVariants = [&](std::unordered_map<uint64_t, Tile> &into, unsigned bits) {
        if (!have(4))
            return false;
        const uint32_t n = ReadLe32(&bytes[offset]);
        offset += 4;
        if (!have(static_cast<size_t>(n) * (8 + kTileBytes)))
            return false;
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint64_t key = (static_cast<uint64_t>(ReadLe32(&bytes[offset])) << bits) | ReadLe32(&bytes[offset + 4]);
            offset += 8;
            readTile(into[key]);
        }
        return true;
    };
    if (version >= 2 && have(4))
    {
        if (!readVariants(m_layerTiles, 5))
            return false;
        if (have(4))
        {
            const uint32_t left = ReadLe32(&bytes[offset]);
            offset += 4;
            if (!have(static_cast<size_t>(left) * 12))
                return false;
            for (uint32_t i = 0; i < left; ++i, offset += 12)
                m_leftUncolored[ReadLe32(&bytes[offset])] = ReadLe64(&bytes[offset + 4]);
        }
    }
    if (version >= 3)
    {
        if (!readVariants(m_paletteTiles, 2) || !have(4))
            return false;
        const uint32_t cellCount = ReadLe32(&bytes[offset]);
        offset += 4;
        constexpr size_t kCellBytes = 2 + 2 + 4 + 8 + 8 + 64 * 3;
        if (!have(static_cast<size_t>(cellCount) * kCellBytes))
            return false;
        m_cellTiles.resize(cellCount);
        for (CellTile &cell : m_cellTiles)
        {
            const uint8_t *p = &bytes[offset];
            cell.cell = static_cast<uint16_t>(p[0] | (p[1] << 8));
            cell.palette = static_cast<uint8_t>(p[2] & 3);
            cell.hash = ReadLe32(p + 4);
            cell.mask = ReadLe64(p + 8);
            cell.keep = ReadLe64(p + 16);
            std::memcpy(cell.rgb, p + 24, sizeof(cell.rgb));
            offset += kCellBytes;
        }
        if (have(4))
        {
            m_referenceLevel = static_cast<uint8_t>(std::min<uint32_t>(63, std::max<uint32_t>(1, ReadLe32(&bytes[offset]))));
            offset += 4;
        }
        std::sort(m_cellTiles.begin(), m_cellTiles.end(), [](const CellTile &a, const CellTile &b) {
            return a.cell != b.cell ? a.cell < b.cell : a.palette != b.palette ? a.palette < b.palette : a.hash < b.hash;
        });
        // Context variants (optional; a damaged block is dropped, the rest stays).
        if (have(12) && std::memcmp(&bytes[offset], kContextMagic, 8) == 0)
        {
            offset += 8;
            std::vector<std::vector<uint32_t>> groups;
            std::vector<ContextTile> tiles;
            bool ok = true;
            const uint32_t groupCount = ReadLe32(&bytes[offset]);
            offset += 4;
            for (uint32_t g = 0; ok && g < groupCount && g < kMaxContextGroups; ++g)
            {
                ok = have(4);
                const uint32_t n = ok ? ReadLe32(&bytes[offset]) : 0;
                offset += 4;
                ok = ok && have(static_cast<size_t>(n) * 4);
                std::vector<uint32_t> markers;
                for (uint32_t i = 0; ok && i < n; ++i, offset += 4)
                    markers.push_back(ReadLe32(&bytes[offset]));
                groups.push_back(std::move(markers));
            }
            ok = ok && groupCount <= kMaxContextGroups && have(4);
            const uint32_t tileCount = ok ? ReadLe32(&bytes[offset]) : 0;
            offset += 4;
            ok = ok && have(static_cast<size_t>(tileCount) * (8 + kTileBytes));
            for (uint32_t i = 0; ok && i < tileCount; ++i)
            {
                ContextTile c;
                c.hash = ReadLe32(&bytes[offset]);
                c.group = ReadLe32(&bytes[offset + 4]);
                offset += 8;
                readTile(c.tile);
                ok = c.group < groups.size();
                tiles.push_back(c);
            }
            if (ok)
            {
                m_contextGroups = std::move(groups);
                m_contextTiles = std::move(tiles);
                if (have(16) && std::memcmp(&bytes[offset], kLayerBoundMagic, 8) == 0)
                {
                    const uint64_t bits = ReadLe64(&bytes[offset + 8]);
                    offset += 16;
                    m_contextLayerBound = m_contextGroups.size() >= 64 ? bits : bits & ((1ull << m_contextGroups.size()) - 1);
                }
            }
        }
    }
    return true;
}
