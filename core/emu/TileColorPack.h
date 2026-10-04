#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// A per-game color pack (experimental): painted colors for individual 8x8
// tiles, keyed by the tile's content hash from the tile tracker (see
// vbgo_tiletrack.h), so a tile gets its colors wherever the game draws it -
// any level, any position, either eye.
//
// Built by importing paintings: copies of reference captures (F10 in the
// desktop build - a 3x PNG plus its .tiles sidecar) that someone painted
// over without moving any pixels. Every painted pixel votes for the color of
// the tile pixel it shows; the most-voted color wins, so a tile painted
// slightly differently in two places still gets one consistent result.
// Tile sheets (F6: everything in the game's tile memory, laid out like a
// tile viewer) import the same way, but only fill in tile pixels no screen
// painting colors.
//
// Saved as "<rom>.vbcp" next to the ROM, so a finished pack can be copied to
// any platform without the paintings.
class TileColorPack
{
public:
    struct Tile
    {
        uint64_t mask = 0;       // bit (y * 8 + x) set = that tile pixel has a painted color
        uint8_t rgb[64][3] = {}; // indexed the same way
    };

    struct ImportStats
    {
        int paintings = 0;          // screen paintings accepted
        int sheets = 0;             // tile sheets accepted (F6 - fill in what the screen paintings don't color)
        int rejected = 0;           // wrong size / bad sidecar
        size_t tilePixels = 0;      // distinct (tile, pixel) colors resolved
        size_t inconsistent = 0;    // of those, painted differently in different places (majority wins)
        size_t mergedColors = 0;    // stray near-duplicate shades folded into a main color
        size_t fromSheets = 0;      // tile pixels only a tile sheet colored
        std::string lastError;      // why the last rejected painting was rejected
    };

    void Clear();
    bool Empty() const { return m_tiles.empty(); }
    size_t TileCount() const { return m_tiles.size(); }
    const Tile *Find(uint32_t hash) const;
    const std::unordered_map<uint32_t, Tile> &Tiles() const { return m_tiles; }

    // True if that tile pixel has a color.
    bool Has(uint32_t hash, unsigned index) const
    {
        const Tile *tile = Find(hash);
        return tile && (tile->mask >> index & 1);
    }
    void Set(uint32_t hash, unsigned index, uint8_t r, uint8_t g, uint8_t b);

    // Import, step 1 (call per painting): pixels = 8-bit RGB/RGBA rows of a
    // painted capture, with the .tiles sidecar the capture was saved with -
    // a screen (F10, F7 sheets: 384x224) or a tile sheet (F6: the whole tile
    // memory). Any size with the capture's shape (normally 3x). False (and
    // stats.lastError) if they don't fit together.
    bool AddPainting(const uint8_t *pixels, int width, int height, int channels, const std::vector<uint8_t> &sidecar,
                     ImportStats &stats);
    // Import, step 2: resolve the votes into the pack (replacing its contents).
    void FinishImport(ImportStats &stats);

    std::vector<uint8_t> Serialize() const;
    bool Deserialize(const std::vector<uint8_t> &bytes);

private:
    std::unordered_map<uint32_t, Tile> m_tiles;
    // (hash << 6 | pixel index) -> (0xRRGGBB -> votes); screen paintings and
    // tile sheets apart, since sheets only fill in
    std::unordered_map<uint64_t, std::unordered_map<uint32_t, uint32_t>> m_votes, m_sheetVotes;
};
