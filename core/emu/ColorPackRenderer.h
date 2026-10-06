#pragma once

#include "emu/AutoColors.h"
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
//    automatic ramp). Tiles both pictures use are colored by tile like
//    anything else (a tile the pack colors per map cell: by the left
//    picture's cell showing the same tile pixel on the row). The right
//    picture's own tiles (Galactic Pinball, Wario Land's title: each eye's
//    picture drawn separately) by their own map cells' colors if the pack has
//    them (right-eye paintings), else from the left picture: per 8x8 block a
//    disparity, found from the two pictures' shades when they change (not
//    per frame), and the color of the nearest left-picture pixel of the same
//    shade where it puts the pixel - the same spot of the same object,
//    whatever the two dithers do - else the most common color of that shade
//    around it. Nothing searches per pixel per frame, so nothing flickers.
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
    // doesn't color - all of them, without a pack.
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

    void ResolveSlots(const uint32_t *hashes);
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
        bool sprites = false; // a sprite world's left-only and right-only sprites (left = right = that world)
        std::array<uint16_t, 32> attributes{}; // both worlds' attribute blocks, as last estimated
        bool estimated = false;
        uint32_t estimatedAt = 0, checkedAt = 0;
        std::array<int16_t, kBands> disparity{}; // right x -> left x, per band
        std::vector<int16_t> blockDisparity;     // the same per 8x8 block of the right picture
        std::vector<Block> blocks; // per 8x8 block of the right eye: its region colors this frame (see RegionColor)
    };
    std::array<Pair, kMaxPairs> m_pairs{};
    int m_pairCount = 0;
    uint32_t m_frame = 0;
    // A pair's disparities, if they're needed this frame and out of date.
    void EstimatePair(int pair, const vbgo_tt_eye_view view[2], const uint8_t *raw, uint32_t fbWidth, const uint32_t eyeOffset[2]);
    std::vector<uint64_t> m_estimateBits; // (EstimatePair: both pictures' shades as bit rows)
    // Tiles the left pictures of pairs draw this frame (by hash): a right
    // picture's pixel of such a tile is colored by tile; the right
    // picture's own tiles by region (see the class comment).
    std::vector<uint32_t> m_leftPairHashes; // open addressing, 0 = empty (hash 0 kept apart)
    bool m_leftPairHashZero = false;
    std::array<uint32_t, 2048> m_slotSharedAt{}; // per slot: 2 x frame (+1: shared) when last checked
    bool SharedWithLeftPicture(unsigned chr, uint32_t hash);
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
    uint64_t Near(int x, int y, unsigned world) const;
    std::vector<uint64_t> m_nearSprites; // per grid cell: sprites' groups with a marker within reach (last two frames)

    const TileColorPack *m_pack = nullptr;
    std::vector<uint32_t> m_cellStart;  // 65537 entries: cell n's CellTiles are [m_cellStart[n], m_cellStart[n + 1])
    std::vector<uint8_t> m_fillCells;
    std::unordered_set<uint32_t> m_layered; // hashes with per-layer colors
    std::array<Slot, 2048> m_slots{};
    int m_fade[64] = {}; // brightness level -> 0-256, relative to the pack's reference brightness
};
