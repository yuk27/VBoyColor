#include "emu/TileColorPack.h"
#include "emu/vbgo_tiletrack.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr char kPackMagicV1[8] = {'V', 'B', 'G', 'O', 'C', 'P', '0', '1'};
    constexpr char kPackMagic[8] = {'V', 'B', 'G', 'O', 'C', 'P', '0', '2'}; // + per-layer colors
    constexpr char kSidecarMagic[8] = {'V', 'B', 'G', 'O', 'T', 'I', 'L', '1'};
    constexpr char kPaletteMagic[8] = {'V', 'B', 'G', 'O', 'P', 'A', 'L', '1'};

    constexpr uint32_t kNoColor = 0x01000000; // a magenta vote: leave uncolored
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
} // namespace

void TileColorPack::Clear()
{
    m_tiles.clear();
    m_layerTiles.clear();
    m_leftUncolored.clear();
    m_votes.clear();
    m_sheetVotes.clear();
    m_layerVotes.clear();
}

void TileColorPack::AppendSidecarPalette(std::vector<uint8_t> &sidecar, const std::array<std::array<uint8_t, 3>, 4> &palette)
{
    sidecar.insert(sidecar.end(), kPaletteMagic, kPaletteMagic + 8);
    for (const auto &color : palette)
        sidecar.insert(sidecar.end(), color.begin(), color.end());
}

const TileColorPack::Tile *TileColorPack::Find(uint32_t hash) const
{
    const auto it = m_tiles.find(hash);
    return it == m_tiles.end() ? nullptr : &it->second;
}

void TileColorPack::Set(uint32_t hash, unsigned index, uint8_t r, uint8_t g, uint8_t b)
{
    Tile &tile = m_tiles[hash];
    tile.mask |= 1ull << index;
    tile.rgb[index][0] = r;
    tile.rgb[index][1] = g;
    tile.rgb[index][2] = b;
}

bool TileColorPack::AddPainting(const uint8_t *pixels, int width, int height, int channels,
                                const std::vector<uint8_t> &sidecar, ImportStats &stats)
{
    // The sidecar says what the painting covers: a screen capture (384x224
    // pixels) or a tile sheet (any size, e.g. the whole tile memory).
    const size_t headerSize = 16;
    if (sidecar.size() < headerSize || std::memcmp(sidecar.data(), kSidecarMagic, 8) != 0)
    {
        stats.lastError = "missing or unreadable .tiles file";
        ++stats.rejected;
        return false;
    }
    const uint32_t w = ReadLe32(&sidecar[8]), h = ReadLe32(&sidecar[12]);
    if (w == 0 || h == 0 || w > 4096 || h > 4096 || sidecar.size() < headerSize + static_cast<size_t>(w) * h * 8)
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
    const bool sheet = !(w == VBGO_TT_WIDTH && h == VBGO_TT_HEIGHT);
    auto &votes = sheet ? m_sheetVotes : m_votes;

    // The colors the capture showed (palette block after the tile
    // dictionary, if any): a pixel still in one of the 3 shade colors was
    // left unpainted.
    uint8_t shown[4][3];
    std::memcpy(shown, kDefaultCapturePalette, sizeof(shown));
    size_t offset = headerSize + static_cast<size_t>(w) * h * 8;
    if (sidecar.size() >= offset + 4)
    {
        offset += 4 + static_cast<size_t>(ReadLe32(&sidecar[offset])) * (4 + 16);
        if (sidecar.size() >= offset + 8 + 12 && std::memcmp(&sidecar[offset], kPaletteMagic, 8) == 0)
            std::memcpy(shown, &sidecar[offset + 8], sizeof(shown));
    }
    constexpr int kSameColor = 24;  // |dR|+|dG|+|dB| still counted as the capture's own color
    constexpr int kMagentaReach = 40;
    static const uint8_t kMagenta[3] = {255, 0, 255};

    // Sample the middle of each pixel's block.
    for (uint32_t y = 0; y < h; ++y)
    {
        const int py = std::min(height - 1, static_cast<int>((y + 0.5) * sy));
        for (uint32_t x = 0; x < w; ++x)
        {
            const uint64_t t = ReadLe64(&sidecar[headerSize + (static_cast<size_t>(y) * w + x) * 8]);
            if (!VBGO_TT_VALID(t))
                continue; // background - not part of any tile
            const int px = std::min(width - 1, static_cast<int>((x + 0.5) * sx));
            const uint8_t *p = &pixels[(static_cast<size_t>(py) * width + px) * channels];
            if (Distance(p, shown[1]) <= kSameColor || Distance(p, shown[2]) <= kSameColor ||
                Distance(p, shown[3]) <= kSameColor)
                continue; // left as captured - not painted
            const uint32_t rgb = Distance(p, kMagenta) <= kMagentaReach ? kNoColor : (p[0] << 16) | (p[1] << 8) | p[2];
            const uint64_t key = (static_cast<uint64_t>(VBGO_TT_HASH(t)) << 6) | (VBGO_TT_SUBY(t) * 8 + VBGO_TT_SUBX(t));
            ++votes[key][rgb];
            if (!sheet)
                ++m_layerVotes[(key << 5) | VBGO_TT_WORLD(t)][rgb];
        }
    }
    ++(sheet ? stats.sheets : stats.paintings);
    return true;
}

void TileColorPack::FinishImport(ImportStats &stats)
{
    // Tile sheets only fill in: a tile pixel any screen painting colored
    // keeps the screen paintings' color.
    for (auto &entry : m_sheetVotes)
        if (!m_votes.count(entry.first))
        {
            m_votes[entry.first] = std::move(entry.second);
            ++stats.fromSheets;
        }
    m_sheetVotes.clear();

    // Cleanup 1: merge stray near-duplicate shades (a slightly-off brush
    // color on a handful of pixels) into the closest color the painter used
    // a lot - never introduces a color that isn't already in the paintings.
    std::unordered_map<uint32_t, uint64_t> usage;
    uint64_t totalVotes = 0;
    for (const auto &entry : m_votes)
        for (const auto &vote : entry.second)
            if (vote.first != kNoColor)
            {
                usage[vote.first] += vote.second;
                totalVotes += vote.second;
            }
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

    // Cleanup 2: one color per tile pixel - the most-voted one across every
    // place the tile appears in every painting.
    auto winner = [&merged](const std::unordered_map<uint32_t, uint32_t> &raw, uint32_t &bestVotes, uint32_t &allVotes,
                            size_t &distinct) {
        std::unordered_map<uint32_t, uint32_t> votes;
        for (const auto &vote : raw)
        {
            const auto m = merged.find(vote.first);
            votes[m == merged.end() ? vote.first : m->second] += vote.second;
        }
        uint32_t best = 0;
        bestVotes = allVotes = 0;
        for (const auto &vote : votes)
        {
            allVotes += vote.second;
            if (vote.second > bestVotes || (vote.second == bestVotes && vote.first < best))
            {
                best = vote.first;
                bestVotes = vote.second;
            }
        }
        distinct = votes.size();
        return best;
    };
    auto store = [](Tile &tile, unsigned index, uint32_t rgb) {
        tile.mask |= 1ull << index;
        tile.rgb[index][0] = static_cast<uint8_t>(rgb >> 16);
        tile.rgb[index][1] = static_cast<uint8_t>(rgb >> 8);
        tile.rgb[index][2] = static_cast<uint8_t>(rgb);
    };
    m_tiles.clear();
    m_layerTiles.clear();
    m_leftUncolored.clear();
    std::unordered_map<uint64_t, uint32_t> chosen;
    for (const auto &entry : m_votes)
    {
        uint32_t bestVotes = 0, allVotes = 0;
        size_t distinct = 0;
        const uint32_t best = winner(entry.second, bestVotes, allVotes, distinct);
        chosen[entry.first] = best;
        if (distinct > 1)
            ++stats.inconsistent;
        if (best == kNoColor)
        {
            m_leftUncolored[static_cast<uint32_t>(entry.first >> 6)] |= 1ull << (entry.first & 63);
            ++stats.erased;
            continue;
        }
        store(m_tiles[static_cast<uint32_t>(entry.first >> 6)], entry.first & 63, best);
        ++stats.tilePixels;
    }

    // Cleanup 3: a layer that consistently shows a tile pixel in another
    // color than the overall choice (at least twice, two thirds of its
    // votes) keeps that color for itself - tiles reused in different places.
    for (const auto &entry : m_layerVotes)
    {
        const uint64_t key = entry.first >> 5;
        const auto overall = chosen.find(key);
        if (overall == chosen.end() || overall->second == kNoColor)
            continue;
        uint32_t bestVotes = 0, allVotes = 0;
        size_t distinct = 0;
        const uint32_t best = winner(entry.second, bestVotes, allVotes, distinct);
        if (best == kNoColor || best == overall->second || bestVotes < 2 || bestVotes * 3 < allVotes * 2)
            continue;
        store(m_layerTiles[LayerKey(static_cast<uint32_t>(key >> 6), entry.first & 31)], key & 63, best);
        ++stats.layerPixels;
    }
    // A layer's own colors only list the pixels that differ - fill in the
    // rest from the overall colors, so a lookup needs just one tile.
    for (auto &entry : m_layerTiles)
    {
        const auto base = m_tiles.find(static_cast<uint32_t>(entry.first >> 5));
        if (base == m_tiles.end())
            continue;
        for (unsigned i = 0; i < 64; ++i)
            if ((base->second.mask >> i & 1) && !(entry.second.mask >> i & 1))
                store(entry.second, i, (base->second.rgb[i][0] << 16) | (base->second.rgb[i][1] << 8) | base->second.rgb[i][2]);
    }
    m_votes.clear();
    m_layerVotes.clear();
}

std::vector<uint8_t> TileColorPack::Serialize() const
{
    // "VBGOCP02", tile count, then per tile: hash, mask, 64 x RGB; then the
    // per-layer colors: count, then per entry: hash, world, mask, 64 x RGB;
    // then the pixels left uncolored on purpose.
    // Sorted, so the same pack always gives the same bytes.
    std::vector<uint32_t> hashes;
    hashes.reserve(m_tiles.size());
    for (const auto &entry : m_tiles)
        hashes.push_back(entry.first);
    std::sort(hashes.begin(), hashes.end());
    std::vector<uint8_t> out(kPackMagic, kPackMagic + 8);
    AppendLe(out, m_tiles.size(), 4);
    for (const uint32_t hash : hashes)
    {
        const Tile &tile = m_tiles.at(hash);
        AppendLe(out, hash, 4);
        AppendLe(out, tile.mask, 8);
        out.insert(out.end(), &tile.rgb[0][0], &tile.rgb[0][0] + sizeof(tile.rgb));
    }
    std::vector<uint64_t> layerKeys;
    for (const auto &entry : m_layerTiles)
        layerKeys.push_back(entry.first);
    std::sort(layerKeys.begin(), layerKeys.end());
    AppendLe(out, layerKeys.size(), 4);
    for (const uint64_t key : layerKeys)
    {
        const Tile &tile = m_layerTiles.at(key);
        AppendLe(out, key >> 5, 4);
        AppendLe(out, key & 31, 4);
        AppendLe(out, tile.mask, 8);
        out.insert(out.end(), &tile.rgb[0][0], &tile.rgb[0][0] + sizeof(tile.rgb));
    }
    // then the tile pixels left uncolored on purpose: count, (hash, mask) each
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
    return out;
}

bool TileColorPack::Deserialize(const std::vector<uint8_t> &bytes)
{
    Clear();
    constexpr size_t kEntry = 4 + 8 + 64 * 3, kLayerEntry = 4 + 4 + 8 + 64 * 3;
    if (bytes.size() < 12)
        return false;
    const bool v2 = std::memcmp(bytes.data(), kPackMagic, 8) == 0;
    if (!v2 && std::memcmp(bytes.data(), kPackMagicV1, 8) != 0)
        return false;
    const uint32_t count = ReadLe32(&bytes[8]);
    size_t offset = 12;
    if (bytes.size() < offset + static_cast<size_t>(count) * kEntry)
        return false;
    for (uint32_t i = 0; i < count; ++i, offset += kEntry)
    {
        const uint8_t *p = &bytes[offset];
        Tile &tile = m_tiles[ReadLe32(p)];
        tile.mask = ReadLe64(p + 4);
        std::memcpy(tile.rgb, p + 12, sizeof(tile.rgb));
    }
    if (v2 && bytes.size() >= offset + 4)
    {
        const uint32_t layers = ReadLe32(&bytes[offset]);
        offset += 4;
        if (bytes.size() < offset + static_cast<size_t>(layers) * kLayerEntry)
            return false;
        for (uint32_t i = 0; i < layers; ++i, offset += kLayerEntry)
        {
            const uint8_t *p = &bytes[offset];
            Tile &tile = m_layerTiles[LayerKey(ReadLe32(p), ReadLe32(p + 4))];
            tile.mask = ReadLe64(p + 8);
            std::memcpy(tile.rgb, p + 16, sizeof(tile.rgb));
        }
        if (bytes.size() >= offset + 4)
        {
            const uint32_t left = ReadLe32(&bytes[offset]);
            offset += 4;
            if (bytes.size() < offset + static_cast<size_t>(left) * 12)
                return false;
            for (uint32_t i = 0; i < left; ++i, offset += 12)
                m_leftUncolored[ReadLe32(&bytes[offset])] = ReadLe64(&bytes[offset + 4]);
        }
    }
    return true;
}
