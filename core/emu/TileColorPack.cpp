#include "emu/TileColorPack.h"
#include "emu/vbgo_tiletrack.h"

#include <algorithm>
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
}

const TileColorPack::Tile *TileColorPack::Find(uint32_t hash) const
{
    const auto it = m_tiles.find(hash);
    return it == m_tiles.end() ? nullptr : &it->second;
}

bool TileColorPack::AddPainting(const uint8_t *pixels, int width, int height, int channels,
                                const std::vector<uint8_t> &sidecar, ImportStats &stats)
{
    const int scale = width / VBGO_TT_WIDTH;
    if (!pixels || channels < 3 || scale < 1 || width != VBGO_TT_WIDTH * scale || height != VBGO_TT_HEIGHT * scale)
    {
        stats.lastError = "image must be the reference's size (" + std::to_string(VBGO_TT_WIDTH * 3) + "x" +
                          std::to_string(VBGO_TT_HEIGHT * 3) + ") - got " + std::to_string(width) + "x" +
                          std::to_string(height);
        ++stats.rejected;
        return false;
    }
    const size_t headerSize = 16, pixelBytes = static_cast<size_t>(VBGO_TT_EYE_PIXELS) * 8;
    if (sidecar.size() < headerSize + pixelBytes || std::memcmp(sidecar.data(), kSidecarMagic, 8) != 0 ||
        ReadLe32(&sidecar[8]) != VBGO_TT_WIDTH || ReadLe32(&sidecar[12]) != VBGO_TT_HEIGHT)
    {
        stats.lastError = "missing or unreadable .tiles file";
        ++stats.rejected;
        return false;
    }

    // Sample the middle of each scale x scale block - one VB pixel.
    const int c = scale / 2;
    for (int y = 0; y < VBGO_TT_HEIGHT; ++y)
    {
        for (int x = 0; x < VBGO_TT_WIDTH; ++x)
        {
            const uint64_t t = ReadLe64(&sidecar[headerSize + (static_cast<size_t>(y) * VBGO_TT_WIDTH + x) * 8]);
            if (!VBGO_TT_VALID(t))
                continue; // background - not part of any tile
            const uint8_t *px = &pixels[(static_cast<size_t>(y * scale + c) * width + (x * scale + c)) * channels];
            const uint32_t rgb = (px[0] << 16) | (px[1] << 8) | px[2];
            const uint64_t key = (static_cast<uint64_t>(VBGO_TT_HASH(t)) << 6) | (VBGO_TT_SUBY(t) * 8 + VBGO_TT_SUBX(t));
            ++m_votes[key][rgb];
        }
    }
    ++stats.paintings;
    return true;
}

void TileColorPack::FinishImport(ImportStats &stats)
{
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
    std::unordered_map<uint32_t, uint32_t> merged;
    for (const auto &color : usage)
    {
        if (color.second >= rareBelow)
            continue;
        uint32_t best = color.first;
        int bestDistance = kMergeDistance + 1;
        for (const auto &other : usage)
        {
            if (other.second < rareBelow)
                continue;
            const int d = std::abs(static_cast<int>((color.first >> 16) & 255) - static_cast<int>((other.first >> 16) & 255)) +
                          std::abs(static_cast<int>((color.first >> 8) & 255) - static_cast<int>((other.first >> 8) & 255)) +
                          std::abs(static_cast<int>(color.first & 255) - static_cast<int>(other.first & 255));
            if (d < bestDistance)
            {
                best = other.first;
                bestDistance = d;
            }
        }
        if (best != color.first)
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
    std::vector<uint8_t> out(kPackMagic, kPackMagic + 8);
    AppendLe(out, m_tiles.size(), 4);
    for (const auto &entry : m_tiles)
    {
        AppendLe(out, entry.first, 4);
        AppendLe(out, entry.second.mask, 8);
        out.insert(out.end(), &entry.second.rgb[0][0], &entry.second.rgb[0][0] + sizeof(entry.second.rgb));
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
