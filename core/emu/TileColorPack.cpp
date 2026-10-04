#include "emu/TileColorPack.h"
#include "emu/vbgo_tiletrack.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr char kPackMagic[8] = {'V', 'B', 'G', 'O', 'C', 'P', '0', '1'};
    constexpr char kSidecarMagic[8] = {'V', 'B', 'G', 'O', 'T', 'I', 'L', '1'};

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
    m_votes.clear();
    m_sheetVotes.clear();
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
            const uint32_t rgb = (p[0] << 16) | (p[1] << 8) | p[2];
            const uint64_t key = (static_cast<uint64_t>(VBGO_TT_HASH(t)) << 6) | (VBGO_TT_SUBY(t) * 8 + VBGO_TT_SUBX(t));
            ++votes[key][rgb];
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
    m_tiles.clear();
    for (const auto &entry : m_votes)
    {
        std::unordered_map<uint32_t, uint32_t> votes;
        for (const auto &vote : entry.second)
        {
            const auto m = merged.find(vote.first);
            votes[m == merged.end() ? vote.first : m->second] += vote.second;
        }
        uint32_t best = 0, bestVotes = 0;
        for (const auto &vote : votes)
            if (vote.second > bestVotes || (vote.second == bestVotes && vote.first < best))
            {
                best = vote.first;
                bestVotes = vote.second;
            }
        Tile &tile = m_tiles[static_cast<uint32_t>(entry.first >> 6)];
        const unsigned index = entry.first & 63;
        tile.mask |= 1ull << index;
        tile.rgb[index][0] = static_cast<uint8_t>(best >> 16);
        tile.rgb[index][1] = static_cast<uint8_t>(best >> 8);
        tile.rgb[index][2] = static_cast<uint8_t>(best);
        ++stats.tilePixels;
        if (votes.size() > 1)
            ++stats.inconsistent;
    }
    m_votes.clear();
}

std::vector<uint8_t> TileColorPack::Serialize() const
{
    // "VBGOCP01", tile count, then per tile: hash, mask, 64 x RGB.
    // Sorted by hash, so the same pack always gives the same bytes.
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
    return out;
}

bool TileColorPack::Deserialize(const std::vector<uint8_t> &bytes)
{
    Clear();
    constexpr size_t kEntry = 4 + 8 + 64 * 3;
    if (bytes.size() < 12 || std::memcmp(bytes.data(), kPackMagic, 8) != 0)
        return false;
    const uint32_t count = ReadLe32(&bytes[8]);
    if (bytes.size() < 12 + static_cast<size_t>(count) * kEntry)
        return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint8_t *p = &bytes[12 + static_cast<size_t>(i) * kEntry];
        Tile &tile = m_tiles[ReadLe32(p)];
        tile.mask = ReadLe64(p + 4);
        std::memcpy(tile.rgb, p + 12, sizeof(tile.rgb));
    }
    return true;
}
