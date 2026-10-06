#pragma once

#include "emu/AutoColors.h"
#include "emu/DepthColors.h"
#include "emu/ShadeColorizer.h"
#include "emu/TileColorPack.h"
#include "emu/vbgo_tiletrack.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Paints a color pack (see TileColorPack.h) over a frame the shade
// colorizer has already colored, from the tile tracker's per-pixel tags (see
// vbgo_tiletrack.h). Kept free of any GPU/UI code so tools and tests can use
// it as-is.
//
// Both eyes, same colors: a pixel's color depends only on what both eyes
// share - the tile pixel, its palette, its map cell, its layer, what's
// around it in left-eye terms - never on a per-frame search of the other
// eye. How the Virtual Boy makes 3D decides what that is:
//  - a layer both eyes draw (LON and RON) and every sprite both eyes show:
//    the same tile pixels, shifted - looked up the same, so colored the same
//    (context markers and ambiguous tiles go by where the left eye shows
//    things, so they agree too);
//  - a per-eye pair - a left-only layer next to a right-only one, or a
//    sprite world's left-only and right-only sprites (JLON / JRON): the
//    right picture takes its left partner's layer colors (variants,
//    automatic ramp). Tiles its left partner also draws are colored by tile
//    like anything else (a tile the pack colors per map cell: by the left
//    picture's cell showing the same tile pixel on the row, else wherever
//    the left picture shows the tile). The right picture's own tiles
//    (Galactic Pinball, Wario Land's title: each eye's picture drawn
//    separately) by their own map cells' colors if the pack has them
//    (right-eye paintings), else from the left picture: per 8x8 block a
//    disparity, found from the two pictures' shades when they change (not
//    per frame) - near its band's, or anywhere if that lines up poorly
//    (things at very different depths side by side) - and the color of the
//    nearest left-picture pixel of the same shade where it puts the pixel -
//    the same spot of the same object, whatever the two dithers do - else
//    the most common color of that shade around it. Nothing searches per
//    pixel per frame, so nothing flickers.
//
// Cost is kept per tile and per map cell rather than per pixel: a character
// slot's colors (tile, palette variants) are looked up only when the slot's
// contents change, map cells index straight into a table built once when
// the pack is set, and a run of pixels from one tile in one map cell shares
// one lookup - so a pixel is a few array reads, no hash lookups.
class ColorPackRenderer
{
public:
    // Builds the lookup tables (call again whenever the pack changes; the
    // pack must outlive its use here). nullptr / an empty pack disables.
    void SetPack(const TileColorPack *pack);
    bool HasPack() const { return m_pack != nullptr; }
    // Whether the pack's colors show (off: only automatic colors, if on - to
    // compare).
    void SetPackShown(bool shown) { m_packShown = shown; }
    // Automatic colors (see AutoColors.h) for every tracked pixel the pack
    // doesn't color - all of them, without a pack - and by depth for what
    // no tile drew (see DepthColors.h).
    void SetAutoColors(bool on) { m_auto = on; }
    bool AutoColorsOn() const { return m_auto; }
    // Something to paint: a pack, or automatic colors.
    bool Active() const { return m_pack != nullptr || m_auto; }

    // The map cells the pack has fill colors for, as the 65536-bit map
    // vbgo_tiletrack_set_fill_cells takes; empty if there are none.
    const std::vector<uint8_t> &FillCells() const { return m_fillCells; }

    // frame: B,G,R,A bytes as the shade colorizer left them; raw: the core's
    // frame (B,G,R,tag); both fbWidth pixels per row, the eyes side by side
    // starting at eyeOffset[0/1]. background: the Multicolor palette's
    // background, which painted colors fade toward with the game's
    // brightness (same as the palette itself - see ShadeColorizer).
    void Paint(uint8_t *frame, const uint8_t *raw, uint32_t fbWidth, const uint32_t eyeOffset[2], const ShadeRgb &background);

    // How the last frame drew its worlds (for tools).
    static constexpr int kNoDisparity = 0x7FFF;
    struct World
    {
        uint8_t eyes = 0;    // bit 0: the left eye draws it, bit 1: the right eye (0: not drawn)
        uint8_t type = 0;    // 0 normal, 1 H-bias, 2 affine, 3 OBJ
        int8_t partner = -1; // the other world of a per-eye pair
        uint8_t colors = 0;  // the world whose colors it takes: its left partner, else itself
    };
    const std::array<World, 32> &Worlds() const { return m_worlds; }
    // A pair's right world, per 8-row band: right-eye x -> left-eye x
    // (kNoDisparity if not known) - as last estimated.
    int PairDisparity(unsigned rightWorld, unsigned band) const;
    // The same for one 8x8 block of the right picture (sprite pairs: pass
    // the sprite world with sprites = true).
    int PairBlockDisparity(unsigned rightWorld, unsigned bx, unsigned by, bool sprites = false) const;
    // The last frame's right eye (384x224): 1 where a right picture showed a
    // tile of its own - colored from the left picture, what a right-eye
    // capture lets the painter paint (see TileColorPack::
    // AppendSidecarRightPicture). Empty if the frame had no pairs.
    const std::vector<uint8_t> &RightPictureOwn() const { return m_rightOwn; }

private:
    static constexpr int kBands = VBGO_TT_HEIGHT / 8, kBlocksX = VBGO_TT_WIDTH / 8;
    struct Slot
    {
        uint32_t hash = 0;
        bool resolved = false;
        bool layered = false;                        // the tile has per-layer colors - look them up
        const TileColorPack::Tile *base = nullptr;   // the tile's own colors
        const TileColorPack::Tile *palette[4] = {};  // per palette: its own colors, else base
        uint64_t markerBits = 0;                     // context groups this tile is a marker of
        uint32_t contextFirst = 0, contextCount = 0; // its context variants in the pack's ContextTiles()
        bool ambiguous = false;                      // painted two ways in a frame: takes its surroundings' colors
        bool cellColored = false;                    // some map cell has colors of its own for it
        uint64_t rightPainted = 0;                   // its pixels painted in right-eye captures (right-eye sprites)
    };
    struct SlotLayer // (a layered slot's last layer looked up, and its colors - see LayerTile)
    {
        uint8_t world = 0xFF;
        const TileColorPack::Tile *tile = nullptr;
    };
    std::array<SlotLayer, 2048> m_slotLayer{};

    void ResolveSlots(const uint32_t *hashes);
    const TileColorPack::Tile *LayerTile(unsigned chr, unsigned world); // the pack's colors for the slot's tile on that layer, or nullptr
    void SetFadeReference(unsigned level);
    void ClassifyWorlds(const uint16_t *worlds);

    // Automatic colors: every background layer's ramp (by world), from the
    // layers drawn since the pack was set (bit per world).
    bool m_auto = false, m_packShown = true;
    uint32_t m_autoWorlds = 0, m_autoWorldsUsed = ~0u;
    unsigned m_autoMaxLevel = 0;
    std::array<AutoColors::Ramp, 32> m_autoLayer{};
    // Figures: background layers no bigger than kFigureSize pixels across
    // (lately) - characters some games draw on layers of their own - colored
    // like sprites, so they stand out from the scenery (bit per world).
    static constexpr int kFigureSize = 128;
    uint32_t m_figureWorlds = 0, m_figuresUsed = ~0u;
    std::array<int16_t, 32> m_worldExtent{};
    void UpdateAutoColors();

    // The worlds this frame (see World), and per world: for a layer both
    // eyes draw, how far right of its left-eye spot the right eye shows a
    // pixel (2 x (GP - MP)); for a pair's left / right world, its pair.
    std::array<World, 32> m_worlds{};
    std::array<int16_t, 32> m_rightShift{};
    std::array<int8_t, 32> m_pairOfLeft{}, m_pairOfRight{}, m_spritePairOf{};
    const uint16_t *m_oam = nullptr; // this frame's OBJ attributes (sprites' parallax), or nullptr
    // Where a right-eye pixel would be in the left eye, as far as both eyes
    // agree on it (sprites and layers both eyes draw: exactly).
    int LeftX(const uint64_t tag, int x, unsigned eye, unsigned y) const;

    // Per-eye pairs (at most kMaxPairs a frame).
    static constexpr int kMaxPairs = 16;
    struct Block
    {
        uint32_t frame = 0;   // when worked out
        uint32_t bgr[3] = {}; // per shade 1-3: the left picture's most common color there (bit 24 set), as shown; 0 none
    };
    struct Pair
    {
        uint8_t left = 0, right = 0;
        uint32_t leftWorlds = 0; // the left picture's worlds (bit per world): left, and any that draw with it for one right world
        bool sprites = false; // a sprite world's left-only and right-only sprites (left = right = that world)
        std::array<uint16_t, 32> attributes{}; // both worlds' attribute blocks, as last estimated
        uint32_t otherAttributes = 0;          // (a hash of the other left worlds' blocks, as last estimated)
        bool estimated = false;
        uint32_t estimatedAt = 0, wantedAt = 0; // (frames: last estimated, last needed)
        std::array<int16_t, kBands> disparity{}; // right x -> left x, per band
        std::vector<int16_t> blockDisparity;     // the same per 8x8 block of the right picture
        std::vector<uint8_t> blockExact;         // per block: 1 where nearly all its pixels line up (the left drawing, shifted)
        std::vector<Block> blocks; // per 8x8 block of the right eye: its region colors this frame (see RegionColor)
    };
    std::array<Pair, kMaxPairs> m_pairs{};
    int m_pairCount = 0;
    uint32_t m_frame = 0;
    // Pairs' disparities, where out of date (once a frame, before the right eye).
    void EstimatePairs(const vbgo_tt_eye_view view[2], const uint8_t *raw, uint32_t fbWidth, const uint32_t eyeOffset[2]);
    void EstimatePair(Pair &pair, const uint64_t *leftBits, const uint64_t *rightBits, bool scrolled);
    std::vector<uint64_t> m_estimateBits; // (EstimatePairs: the pictures' shades as bit rows)
    // Tiles the left pictures of pairs draw this frame (by hash, with the
    // pairs whose left pictures draw them - bit per pair): a right
    // picture's pixel of a tile its own left picture draws is colored by
    // tile; the right picture's own tiles by region (see the class comment).
    std::vector<uint32_t> m_leftPairHashes; // open addressing, 0 = empty (hash 0 kept apart)
    std::vector<uint32_t> m_leftPairMasks;
    uint32_t m_leftPairHashZero = 0;
    std::array<uint32_t, 2048> m_slotSharedAt{}, m_slotShared{}; // per slot: the frame last checked, and the pairs then
    bool SharedWithLeftPicture(unsigned chr, uint32_t hash, int pair);
    // The left pictures' pixels of tiles some map cell colors, by row - and
    // per right-picture cell, the left one it goes by (see MappedCell).
    struct LeftPixel
    {
        int16_t x;
        uint8_t index, palette, world;
        uint16_t cell;
        uint32_t hash;
    };
    std::array<std::vector<LeftPixel>, VBGO_TT_HEIGHT> m_pairRows;
    struct MapEntry
    {
        uint32_t key = 0, frame = 0;
        const TileColorPack::CellTile *cell = nullptr;
    };
    std::vector<MapEntry> m_mapCache;
    // Per tile, palette and pair: the first cell of the pair's left picture
    // showing it this frame (see MappedCell).
    struct LeftTileCell
    {
        uint64_t key = 0;
        uint32_t frame = 0;
        uint16_t cell = 0;
    };
    std::vector<LeftTileCell> m_leftTileCells;
    LeftTileCell *FindLeftTileCell(uint32_t hash, unsigned palette, unsigned pair, bool add);
    static constexpr int kMapClose = 4, kMapReach = 16; // how far from the band's disparity a left tile is looked for (see MappedCell)
    // The left eye's pixels of pairs' left pictures this frame: that pair + 1
    // (bits 0-4), the shade (bits 5-6) - 0 elsewhere (and in a guard band
    // all round, see the .cpp).
    std::vector<uint8_t> m_leftPicture;
    std::vector<uint8_t> m_rightOwn; // see RightPictureOwn
    int EyeOnly(uint64_t tag) const; // a sprite only the left (1) or right (2) eye shows, else 0
    bool InPicture(const Pair &pair, uint64_t tag, unsigned eye) const;
    // A pair's right picture, a tile of its own: its color from the left picture (see the .cpp).
    bool RegionColor(int pair, int x, int y, unsigned shade, const uint8_t *frame, uint32_t fbWidth, uint32_t leftOffset, uint8_t *out);
    const TileColorPack::CellTile *MappedCell(int pair, uint64_t tag, int x, int y, uint32_t hash);
    const TileColorPack::CellTile *FindCell(unsigned cell, unsigned palette, uint32_t hash) const;
    std::unordered_set<uint32_t> m_cellHashes; // tiles some map cell has colors for

    // What a run of pixels (down a column, from one tile in one map cell, or
    // one sprite's tile) shares - looked up once for the run. A run's key
    // (its tag but for the pixel in the tile) decides all of it within an
    // eye's frame, so the last kRunCacheSize runs set up are kept by key: a
    // tile's other columns, a layer's other cells showing it, reuse them.
    struct Run
    {
        const TileColorPack::CellTile *cell = nullptr; // its map cell's colors (or the left partner's)
        const TileColorPack::Tile *tile = nullptr;     // the tile's colors (palette, layer, context or its own) - never null
        const AutoColors::Ramp *ramp = nullptr;        // automatic colors, if on
        int ownPair = -1;                              // a pair's right picture's own tile: region colors
        int copyPair = -1;                             // a pair's right picture, a tile its left picture draws too
        const int16_t *blockDisparity = nullptr;       // (own or copy: the pair's per 8x8 block, if estimated)
        const uint8_t *blockExact = nullptr;           // (and whether the block is the left drawing shifted)
        uint8_t leftPicture = 0;                       // a pair's left picture: that pair + 1
        uint64_t rightPainted = 0;                     // (own tile: its pixels painted in right-eye captures)
        bool record = false;                           // (a left picture's tile colored per map cell: remember where)
        bool slow = false;                             // context, ambiguous or a marker: per 8 rows (see Paint)
        bool extra = false;                            // any of the four above: more to do per pixel than color it
        int band = -1;                                 // (slow: the row of grid cells its markers and tile are for)
        int copyBand = -1, copyDx = 0;                 // (copy: the row of blocks, and its shift there or kNoDisparity)
    };
    struct CachedRun
    {
        uint64_t key = 0;
        uint32_t at = 0; // m_runCacheAt when set up (0: never)
        Run run;
    };
    struct RunSetUp // (what Paint has worked out for the frame that a run's set-up needs)
    {
        const TileColorPack *pack;
        bool haveCells, markers, haveContexts, pairs;
        uint32_t *leftPairSlot;
    };
    Run SetUpRun(uint64_t tag, unsigned eye, int x, int y, const RunSetUp &setUp);
    static constexpr int kRunCacheBits = 10, kRunCacheSize = 1 << kRunCacheBits;
    std::vector<CachedRun> m_runCache;
    uint32_t m_runCacheAt = 0; // (one more each eye painted)

    // Pixels of ambiguous tiles (see TileColorPack::IsAmbiguous) painted this
    // eye, (x, y) - they take the color the same shade has next to them.
    std::vector<std::pair<uint16_t, uint16_t>> m_ambiguousPixels;
    void PaintAmbiguous(uint8_t *frame, uint32_t fbWidth, uint32_t eyeOffset, const vbgo_tt_eye_view &view);

    // Context (see TileColorPack.h): where markers were drawn, on a grid of
    // 8x8-pixel cells in the left eye's terms (a right-eye pixel at its
    // left-eye spot - so both eyes see the same markers around the same
    // thing), over the last two frames (a marker the game shows every other
    // frame still counts). A shared tile takes a group's colors if one of
    // its markers is within kContextReach cells. Sprites' groups only color
    // sprites; layer-bound groups (background figures) only their own
    // layer, by a marker drawn on it (each cell remembers the layer its last
    // such marker was drawn on).
    static constexpr int kGridW = VBGO_TT_WIDTH / 8, kGridH = VBGO_TT_HEIGHT / 8, kContextReach = 4;
    std::unordered_map<uint32_t, uint64_t> m_markerBits;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> m_contextRange; // hash -> (first, count)
    std::vector<uint64_t> m_markerGrid[3];
    std::vector<uint8_t> m_markerLayer[3];
    uint64_t m_layerBound = 0;
    unsigned m_gridCurrent = 0;
    uint64_t Near(int x, int y, unsigned world);
    struct NearCell
    {
        uint32_t frame = 0; // (when worked out)
        uint64_t bits = 0;
    };
    std::array<std::vector<NearCell>, 32> m_nearCache; // (Near's, per layer and grid cell)
    std::vector<uint64_t> m_nearSprites; // per grid cell: sprites' groups with a marker within reach (last two frames)

    // Auto mode, what no tile drew (the game's CPU wrote it into the frame
    // buffer): colored by depth. Returns the brightest level it painted.
    unsigned PaintDepth(uint8_t *frame, const uint8_t *raw, uint32_t fbWidth, const uint32_t eyeOffset[2], const vbgo_tt_eye_view view[2],
                        const bool have[2], const int bg[3]);
    DepthColors m_depth;
    std::vector<uint8_t> m_depthShades[2], m_depthWant[2];

    const TileColorPack *m_pack = nullptr;
    std::vector<uint32_t> m_cellStart;  // 65537 entries: cell n's CellTiles are [m_cellStart[n], m_cellStart[n + 1])
    std::vector<uint8_t> m_fillCells;
    std::unordered_set<uint32_t> m_layered; // hashes with per-layer colors
    std::array<Slot, 2048> m_slots{};
    int m_fade[64] = {}; // brightness level -> 0-256, relative to the pack's reference brightness
};
