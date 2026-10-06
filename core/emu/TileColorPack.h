#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
//  - Pixels still in the capture's own colors (left unpainted) don't vote.
//  - Magenta (#FF00FF) votes for "no color": that tile pixel keeps the
//    Multicolor palette's color, whatever else colors it.
//  - Variants, most specific first - what the runtime looks up:
//    1. Map cell: where a painting shows a background tile at a fixed spot
//       in a different color than the tile gets elsewhere (the same letter
//       yellow in one heading and white in the next, a stripe tile on the lit
//       and the shaded side of a pyramid), that spot keeps the painted color.
//       Also where transparent parts of background tiles were painted over
//       ("fills" - a court surface between its speckles).
//    2. Palette: games show a tile in another palette to mark it (a menu
//       option that isn't selected); when a palette consistently shows a tile
//       in other colors, it gets its own.
//    3. Layer (world): a tile reused on another layer (a cloud tile inside a
//       mountain) that's consistently painted differently there.
//    4. The tile itself.
//  - Context (before 2-4): games reuse the very same tiles in different
//    characters (Jack Bros.' Lantern and Skelton share their hat brims,
//    Mario's Tennis gives Mario and Luigi one cap emblem). Where objects
//    painted differently share a tile, each one that differs from the
//    tile's usual colors gets its own colors, switched on by "marker" tiles
//    only that object uses (Skelton's skull): if a marker was drawn near the
//    tile in the last frame, its colors apply. Found automatically - an
//    object is a connected group of sprite pixels in a painting, or the one
//    figure on a background layer that holds nothing else (Mario's Tennis
//    draws its players as backgrounds).
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

    // Colors one map cell gives the tile it shows (in one palette): only
    // where they differ from the tile's own, plus fills.
    struct CellTile
    {
        uint16_t cell = 0;    // BG map cell (halfword index in VIP DRAM)
        uint8_t palette = 0;  // the palette the cell shows the tile in
        uint32_t hash = 0;    // the tile
        uint64_t mask = 0;    // pixels with a color here (drawn or fill)
        uint64_t keep = 0;    // pixels left uncolored here on purpose (magenta)
        uint8_t rgb[64][3] = {};
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
        size_t erased = 0;          // tile pixels painted magenta - left uncolored
        size_t layerPixels = 0;     // tile pixels with their own colors on some layer
        size_t palettePixels = 0;   // tile pixels with their own colors in some palette
        size_t cellPixels = 0;      // tile pixels with their own colors at some map cell
        size_t fillPixels = 0;      // transparent tile pixels painted (fills)
        size_t contextTiles = 0;    // shared tiles with an object's own colors (context variants)
        size_t contextGroups = 0;   // objects telling them apart (marker sets)
        size_t rightPixels = 0;     // right-eye captures: right-picture pixels painted (their cells' / sprites' own colors)
        std::string lastError;      // why the last rejected painting was rejected
    };

    void Clear();
    bool Empty() const { return m_tiles.empty() && m_cellTiles.empty() && m_contextTiles.empty(); }
    size_t TileCount() const { return m_tiles.size(); }
    const Tile *Find(uint32_t hash) const;
    // The tile's colors for one layer (world 0-31) if that layer has its own,
    // else nullptr - then Find(hash) applies.
    const Tile *FindLayer(uint32_t hash, unsigned world) const
    {
        if (m_layerTiles.empty())
            return nullptr;
        const auto it = m_layerTiles.find(LayerKey(hash, world));
        return it == m_layerTiles.end() ? nullptr : &it->second;
    }
    // Same for a palette (0-3).
    const Tile *FindPalette(uint32_t hash, unsigned palette) const
    {
        if (m_paletteTiles.empty())
            return nullptr;
        const auto it = m_paletteTiles.find(PaletteKey(hash, palette));
        return it == m_paletteTiles.end() ? nullptr : &it->second;
    }
    // A shared tile's colors in one object (see "Context" above): they apply
    // where any of the group's marker tiles was drawn nearby.
    struct ContextTile
    {
        uint32_t hash = 0;
        uint32_t group = 0; // index into ContextGroups()
        Tile tile;
    };
    // Per group, its marker tiles (sorted); at most kMaxContextGroups groups.
    static constexpr size_t kMaxContextGroups = 64;
    const std::vector<std::vector<uint32_t>> &ContextGroups() const { return m_contextGroups; }
    // Sorted by hash, then group.
    const std::vector<ContextTile> &ContextTiles() const { return m_contextTiles; }
    // Groups (bit per group) found on background figures only: their markers
    // switch them on only on the layer the marker is drawn on - so a figure
    // next to another (portraits in a row) doesn't take its colors.
    uint64_t LayerBoundGroups() const { return m_contextLayerBound; }

    // Tiles a character sheet (VBGOFIG1) paints two ways within one frame (a
    // plain filled tile white on the shirt, green on the cap): no context can
    // tell those apart, so the runtime gives such a tile's pixels the color
    // the same shade has right next to it (see ColorPackRenderer).
    bool IsAmbiguous(uint32_t hash) const { return !m_ambiguous.empty() && m_ambiguous.count(hash) != 0; }

    // Tile pixels of right-eye-only sprites painted in right-eye captures (see
    // AppendSidecarRightPicture): the renderer shows those as painted rather
    // than taking the left picture's colors (0: none).
    uint64_t RightEyePainted(uint32_t hash) const
    {
        if (m_eyeTiles.empty())
            return 0;
        const auto it = m_eyeTiles.find(hash);
        return it == m_eyeTiles.end() ? 0 : it->second;
    }

    bool HasLayerColors() const { return !m_layerTiles.empty(); }
    bool HasPaletteColors() const { return !m_paletteTiles.empty(); }
    const std::unordered_map<uint32_t, Tile> &Tiles() const { return m_tiles; }
    // Sorted by cell, then palette, then hash.
    const std::vector<CellTile> &CellTiles() const { return m_cellTiles; }

    // True if that tile pixel has a color.
    bool Has(uint32_t hash, unsigned index) const
    {
        const Tile *tile = Find(hash);
        return tile && (tile->mask >> index & 1);
    }
    // True if that tile pixel was deliberately left uncolored (magenta).
    bool IsLeftUncolored(uint32_t hash, unsigned index) const
    {
        const auto it = m_leftUncolored.find(hash);
        return it != m_leftUncolored.end() && (it->second >> index & 1);
    }
    // Colored or deliberately left uncolored - nothing left to paint there.
    bool IsDone(uint32_t hash, unsigned index) const { return Has(hash, index) || IsLeftUncolored(hash, index); }
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

    // Optional block a .tiles sidecar ends with: the colors the capture showed
    // for the background and the 3 shades, so the importer knows which
    // pixels were left unpainted - and the game's brightness level at the
    // time (0-63, see ShadeColorizer; 255 = not a screen capture, e.g. a
    // sheet drawn at full brightness).
    static void AppendSidecarPalette(std::vector<uint8_t> &sidecar, const std::array<std::array<uint8_t, 3>, 4> &palette,
                                     uint8_t brightnessLevel = 255);
    // Optional block after the palette: what every pixel of the capture
    // showed (RGB, at 1x) - captures show the pack's colors where it already
    // has some, and a pixel still showing them was left alone, so it doesn't
    // vote (a stray color from another object that happened to show doesn't
    // get painted in for good).
    static void AppendSidecarShown(std::vector<uint8_t> &sidecar, const uint8_t *rgb, size_t pixels);
    // Optional block: "VBGOEYE1" + a byte per pixel - a capture of the right
    // eye, 1 where a right picture shows a tile of its own (a right-only
    // layer's or right-eye-only sprite's tile the left picture doesn't use -
    // see ColorPackRenderer): what's painted there is kept as is - a layer's
    // in its map cell, whole; a sprite's as its tile's, marked as painted for
    // that eye - and votes for nothing else.
    static void AppendSidecarRightPicture(std::vector<uint8_t> &sidecar, const uint8_t *own, size_t pixels);
    // Another optional block: "VBGOFIG1" + a figure id per pixel (uint16, 0 =
    // none) - a sheet of separate figures (tools/export_sprite_map.py's
    // character sheets: every animation frame of one character side by side).
    // Each id is one object, as if every figure had been captured on a
    // screen of its own, and the sheet votes like a screen painting whatever
    // its size.

    // The game's usual brightness level (0-63) in the screen paintings: the
    // painted colors are what the painter wants to see at that brightness,
    // so the runtime fades them relative to it (63 if unknown).
    uint8_t ReferenceLevel() const { return m_referenceLevel; }

    // The ROM the pack was made for - the CRC-32 of the ROM file and its
    // size - so a game finds its pack whatever its file is called (0 if
    // unknown: packs made before this). Kept in a fixed 16-byte footer
    // ("VBGOROM1", CRC, size; older builds stop reading before it) that
    // RomOf reads without parsing the rest.
    void SetRom(uint32_t crc, uint32_t size)
    {
        m_romCrc = crc;
        m_romSize = size;
    }
    uint32_t RomCrc() const { return m_romCrc; }
    static bool RomOf(const std::vector<uint8_t> &packBytes, uint32_t &crc, uint32_t &size);
    // A pack file's bytes with that ROM recorded (footer added or replaced).
    static std::vector<uint8_t> WithRom(std::vector<uint8_t> packBytes, uint32_t crc, uint32_t size);
    static uint32_t Crc32(const uint8_t *data, size_t size);

private:
    static uint64_t LayerKey(uint32_t hash, unsigned world) { return (static_cast<uint64_t>(hash) << 5) | (world & 31); }
    static uint64_t PaletteKey(uint32_t hash, unsigned palette) { return (static_cast<uint64_t>(hash) << 2) | (palette & 3); }

    struct Vote
    {
        uint64_t key;
        uint32_t rgb; // 0xRRGGBB, or the "no color" marker
        uint32_t extra; // cell votes: the world (bits 0-4) and whether it's a fill (bit 8); layer votes: the palette
    };

    std::unordered_map<uint32_t, Tile> m_tiles;
    std::unordered_map<uint64_t, Tile> m_layerTiles;   // LayerKey -> that layer's own colors (only where they differ)
    std::unordered_map<uint64_t, Tile> m_paletteTiles; // PaletteKey -> that palette's own colors (only where they differ)
    std::unordered_map<uint32_t, uint64_t> m_leftUncolored; // hash -> tile pixels painted magenta
    std::vector<CellTile> m_cellTiles;
    std::vector<std::vector<uint32_t>> m_contextGroups;
    std::vector<ContextTile> m_contextTiles;
    uint64_t m_contextLayerBound = 0;
    std::unordered_set<uint32_t> m_ambiguous;
    std::unordered_map<uint32_t, uint64_t> m_eyeTiles;   // hash -> right-eye sprite pixels painted (RightEyePainted)
    std::unordered_map<uint32_t, uint64_t> m_eyePending; // (import: the same, before resolving)
    uint32_t m_romCrc = 0, m_romSize = 0;
    uint8_t m_referenceLevel = 63;
    // Objects in the screen paintings being imported (connected tile pixels
    // of one layer): the tiles each one is made of. Context votes carry the
    // object's index in Vote::extra.
    std::vector<std::vector<uint32_t>> m_objectTiles;
    std::vector<uint8_t> m_objectIsFigure; // per object: a background figure (not a sprite)
    std::vector<int32_t> m_objectFamily;   // per object: its figure sheet (one character's frames), or -1
    std::vector<uint8_t> m_objectPalette;  // per object: the palette all its pixels were drawn in, or 0xFF
    int32_t m_families = 0;
    std::unordered_set<uint32_t> m_looseTiles; // background tiles seen outside figures
    std::vector<Vote> m_contextVotes;
    std::unordered_map<uint32_t, std::array<uint8_t, 64>> m_tileShades; // their tiles' pixel values (1-3; 0xFF unknown)
    void ResolveContexts(ImportStats &stats);
    std::unordered_map<uint32_t, std::array<uint16_t, 8>> m_tileRows; // tiles' pixels, from the sidecars being imported
    void CompleteFills(CellTile &cell, uint64_t knownBackground) const;
    std::vector<uint8_t> m_paintingLevels; // known brightness levels of the screen paintings being imported
    // Pending votes, by kind - (hash << 6 | pixel) keys, extended by world /
    // palette / cell. Screen paintings and tile sheets apart, since sheets
    // only fill in.
    std::vector<Vote> m_votes, m_sheetVotes, m_layerVotes, m_paletteVotes, m_cellVotes;
};
