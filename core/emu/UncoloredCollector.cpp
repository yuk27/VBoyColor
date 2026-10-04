#include "emu/UncoloredCollector.h"
#include "emu/vbgo_tiletrack.h"

#include <algorithm>
#include <cstring>

namespace
{
    constexpr int W = VBGO_TT_WIDTH, H = VBGO_TT_HEIGHT;
    constexpr int kSpriteMargin = 3;
    constexpr int kBackgroundMargin = 8;            // a bit of surrounding scenery, to recognize it
    constexpr int kSpriteMax = 96;                  // a sprite's crop grows to its whole sprite, up to this
    constexpr int kCropMaxW = 128, kCropMaxH = 112; // bigger areas are split
    constexpr int kSplitW = 64, kSplitH = 56;
    constexpr int kSceneFresh = 400;                // this much new background in one frame: keep the whole frame
    constexpr int kSceneMaxWait = 150;              // frames to wait for a new scene to settle
    constexpr int kSceneStill = 8;                  // settled = this many frames in a row without changes
    constexpr int kGap = 2;                         // between crops on a sheet

    uint64_t Key(uint64_t t) { return (static_cast<uint64_t>(VBGO_TT_HASH(t)) << 6) | (VBGO_TT_SUBY(t) * 8 + VBGO_TT_SUBX(t)); }

    // Worth keeping: shows a few tile pixels nobody has collected yet, and
    // isn't mostly a repeat.
    bool Worth(size_t fresh, size_t total) { return fresh >= 2 && fresh * 10 >= total; }
} // namespace

void UncoloredCollector::Reset()
{
    m_crops.clear();
    m_collected.clear();
    m_scene = {};
    m_sceneFresh = 0;
    m_sceneFramesLeft = 0;
    m_sceneStill = 0;
    m_lastHashes.clear();
}

void UncoloredCollector::Keep(Crop &&crop)
{
    m_collected.insert(crop.keys.begin(), crop.keys.end());
    m_crops.push_back(std::move(crop));
}

UncoloredCollector::Crop UncoloredCollector::MakeCrop(const uint64_t *tiles, const uint8_t *raw, size_t rawStride,
                                                      const TileColorPack &pack,
                                                      const std::array<std::array<uint8_t, 3>, 4> &palette,
                                                      const std::vector<uint8_t> &state, int x0, int y0, int x1, int y1,
                                                      int world) const
{
    Crop crop;
    crop.w = x1 - x0 + 1;
    crop.h = y1 - y0 + 1;
    crop.rgb.resize(static_cast<size_t>(crop.w) * crop.h * 3);
    crop.tiles.assign(static_cast<size_t>(crop.w) * crop.h, 0);
    for (int y = 0; y < crop.h; ++y)
        for (int x = 0; x < crop.w; ++x)
        {
            const int i = (y0 + y) * W + (x0 + x);
            uint8_t *dst = &crop.rgb[(static_cast<size_t>(y) * crop.w + x) * 3];
            const uint64_t t = tiles[i];
            if (!state[i] || (world >= 0 && static_cast<int>(VBGO_TT_WORLD(t)) != world))
            {
                std::memcpy(dst, palette[0].data(), 3); // other layers / background: left out
                continue;
            }
            crop.tiles[static_cast<size_t>(y) * crop.w + x] = t;
            const TileColorPack::Tile *tile = pack.Find(VBGO_TT_HASH(t));
            if (state[i] == 1 && tile)
                std::memcpy(dst, tile->rgb[VBGO_TT_SUBY(t) * 8 + VBGO_TT_SUBX(t)], 3);
            else
            {
                // Full brightness, whatever the game's fade level right now.
                const uint8_t shade = raw[(static_cast<size_t>(y0 + y) * rawStride + x0 + x) * 4 + 3] & 3;
                std::memcpy(dst, palette[shade].data(), 3);
                crop.keys.push_back(Key(t));
            }
        }
    std::sort(crop.keys.begin(), crop.keys.end());
    crop.keys.erase(std::unique(crop.keys.begin(), crop.keys.end()), crop.keys.end());
    return crop;
}

int UncoloredCollector::AddFrame(const uint64_t *tiles, const uint8_t *raw, size_t rawStride, const TileColorPack &pack,
                                 const std::array<std::array<uint8_t, 3>, 4> &palette)
{
    // 0 = unlit, 1 = lit and colored by the pack, 2 = lit, no pack color.
    std::vector<uint8_t> state(W * H, 0);
    std::vector<uint64_t> freshBackground;
    bool any = false;
    int changed = 0; // pixels showing a different tile than last frame
    m_lastHashes.resize(W * H, 0);
    for (int i = 0; i < W * H; ++i)
    {
        const uint64_t t = tiles[i];
        const uint32_t shown = VBGO_TT_VALID(t) ? VBGO_TT_HASH(t) : 0;
        changed += shown != m_lastHashes[i];
        m_lastHashes[i] = shown;
        const uint8_t *px = &raw[(static_cast<size_t>(i / W) * rawStride + i % W) * 4];
        if (!VBGO_TT_VALID(t) || (px[0] | px[1] | px[2]) == 0)
            continue;
        const bool painted = pack.Has(VBGO_TT_HASH(t), VBGO_TT_SUBY(t) * 8 + VBGO_TT_SUBX(t));
        state[i] = painted ? 1 : 2;
        if (!painted)
        {
            any = true;
            if (!VBGO_TT_IS_OBJ(t) && !m_collected.count(Key(t)))
                freshBackground.push_back(Key(t));
        }
    }
    int added = 0;
    // Lots of new scenery: keep a whole frame, all layers - the most
    // complete one within a moment of it showing up.
    std::sort(freshBackground.begin(), freshBackground.end());
    freshBackground.erase(std::unique(freshBackground.begin(), freshBackground.end()), freshBackground.end());
    const bool still = changed * 100 < W * H; // under 1% of the screen changed
    if (freshBackground.size() >= kSceneFresh && !m_sceneFramesLeft)
        m_sceneFramesLeft = kSceneMaxWait, m_sceneStill = 0, m_sceneFresh = 0;
    if (m_sceneFramesLeft)
    {
        m_sceneStill = still ? m_sceneStill + 1 : 0;
        const bool settled = m_sceneStill >= kSceneStill && freshBackground.size() * 2 >= kSceneFresh;
        if (settled || freshBackground.size() > m_sceneFresh)
        {
            m_scene = MakeCrop(tiles, raw, rawStride, pack, palette, state, 0, 0, W - 1, H - 1, -1);
            m_sceneFresh = std::max(m_sceneFresh, freshBackground.size());
        }
        if (settled || --m_sceneFramesLeft == 0)
        {
            Keep(std::move(m_scene));
            m_scene = {};
            m_sceneFresh = 0;
            m_sceneFramesLeft = 0;
            ++added;
        }
    }
    if (!any)
        return added;

    auto world = [&](int i) { return static_cast<int>(VBGO_TT_WORLD(tiles[i])); };
    m_label.assign(W * H, 0);
    int nextLabel = 0;
    std::vector<uint8_t> seen;
    for (int start = 0; start < W * H; ++start)
    {
        if (state[start] != 2 || m_label[start])
            continue;

        // One object: unpainted pixels of one layer, gaps of up to 2 pixels
        // bridged (so speckled areas stay together).
        const int label = ++nextLabel;
        const int w = world(start);
        m_queue.assign(1, start);
        m_label[start] = label;
        int x0 = W, y0 = H, x1 = -1, y1 = -1;
        size_t total = 0, fresh = 0;
        for (size_t q = 0; q < m_queue.size(); ++q)
        {
            const int i = m_queue[q], x = i % W, y = i / W;
            x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
            ++total;
            fresh += !m_collected.count(Key(tiles[i]));
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx)
                {
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || xx >= W || yy < 0 || yy >= H)
                        continue;
                    const int j = yy * W + xx;
                    if (state[j] == 2 && !m_label[j] && world(j) == w)
                    {
                        m_label[j] = label;
                        m_queue.push_back(j);
                    }
                }
        }
        if (!Worth(fresh, total))
            continue;

        const bool sprite = VBGO_TT_IS_OBJ(tiles[start]);
        if (!sprite && m_sceneFramesLeft)
            continue; // background: part of the scene being kept
        if (sprite)
        {
            // Widen the crop to the whole sprite (its painted parts too), so
            // it's recognizable on the sheet.
            const int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
            const int lx = std::max(0, cx - kSpriteMax / 2), hx = std::min(W - 1, cx + kSpriteMax / 2);
            const int ly = std::max(0, cy - kSpriteMax / 2), hy = std::min(H - 1, cy + kSpriteMax / 2);
            seen.assign(W * H, 0);
            std::vector<int> grow(m_queue);
            for (int i : grow)
                seen[i] = 1;
            for (size_t q = 0; q < grow.size(); ++q)
            {
                const int i = grow[q], x = i % W, y = i / W;
                x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int xx = x + dx, yy = y + dy;
                        if (xx < lx || xx > hx || yy < ly || yy > hy)
                            continue;
                        const int j = yy * W + xx;
                        if (state[j] && !seen[j] && world(j) == w)
                        {
                            seen[j] = 1;
                            grow.push_back(j);
                        }
                    }
            }
        }
        const int margin = sprite ? kSpriteMargin : kBackgroundMargin;
        x0 = std::max(0, x0 - margin), y0 = std::max(0, y0 - margin);
        x1 = std::min(W - 1, x1 + margin), y1 = std::min(H - 1, y1 + margin);

        if (x1 - x0 + 1 <= kCropMaxW && y1 - y0 + 1 <= kCropMaxH)
            m_crops.push_back(MakeCrop(tiles, raw, rawStride, pack, palette, state, x0, y0, x1, y1, w));
        else // big area: pieces that hold some of it
            for (int wy = y0; wy <= y1; wy += kSplitH)
                for (int wx = x0; wx <= x1; wx += kSplitW)
                {
                    const int ex = std::min(x1, wx + kSplitW - 1), ey = std::min(y1, wy + kSplitH - 1);
                    bool has = false;
                    for (int y = wy; y <= ey && !has; ++y)
                        for (int x = wx; x <= ex && !has; ++x)
                            has = m_label[y * W + x] == label;
                    if (has)
                        m_crops.push_back(MakeCrop(tiles, raw, rawStride, pack, palette, state, wx, wy, ex, ey, w));
                }
        for (const int i : m_queue)
            m_collected.insert(Key(tiles[i]));
        ++added;
    }
    return added;
}

std::vector<UncoloredCollector::Sheet> UncoloredCollector::TakeSheets(const std::array<uint8_t, 3> &background)
{
    if (m_sceneFramesLeft && !m_scene.tiles.empty()) // a scene still settling: keep what there is
        Keep(std::move(m_scene));
    m_scene = {};
    m_sceneFresh = 0;
    m_sceneFramesLeft = 0;

    // Drop crops that later, bigger ones made redundant: greedy, most
    // uncolored tile pixels first.
    std::stable_sort(m_crops.begin(), m_crops.end(), [](const Crop &a, const Crop &b) { return a.keys.size() > b.keys.size(); });
    std::unordered_set<uint64_t> covered;
    std::vector<Crop> kept;
    for (Crop &crop : m_crops)
    {
        size_t fresh = 0;
        for (uint64_t key : crop.keys)
            fresh += !covered.count(key);
        if (!Worth(fresh, crop.keys.size()))
            continue;
        covered.insert(crop.keys.begin(), crop.keys.end());
        kept.push_back(std::move(crop));
    }
    m_crops.clear();

    std::vector<Sheet> sheets;
    auto newSheet = [&]() {
        Sheet sheet;
        sheet.rgb.resize(static_cast<size_t>(W) * H * 3);
        for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i)
            std::memcpy(&sheet.rgb[i * 3], background.data(), 3);
        sheet.tiles.assign(static_cast<size_t>(W) * H, 0);
        sheets.push_back(std::move(sheet));
    };
    // Whole frames: a sheet each.
    for (const Crop &crop : kept)
        if (crop.w == W && crop.h == H)
        {
            newSheet();
            sheets.back().rgb = crop.rgb;
            sheets.back().tiles = crop.tiles;
        }
    // The rest: shelves, tallest first.
    std::stable_sort(kept.begin(), kept.end(), [](const Crop &a, const Crop &b) { return a.h > b.h; });
    bool open = false;
    int x = 0, y = 0, shelf = 0;
    for (const Crop &crop : kept)
    {
        if (crop.w == W && crop.h == H)
            continue;
        if (open && x + crop.w + kGap > W) // next shelf
        {
            x = kGap;
            y += shelf + kGap;
            shelf = 0;
        }
        if (!open || y + crop.h + kGap > H) // next sheet
        {
            newSheet();
            open = true;
            x = kGap;
            y = kGap;
            shelf = 0;
        }
        Sheet &sheet = sheets.back();
        for (int cy = 0; cy < crop.h; ++cy)
        {
            std::memcpy(&sheet.rgb[(static_cast<size_t>(y + cy) * W + x) * 3], &crop.rgb[static_cast<size_t>(cy) * crop.w * 3],
                        static_cast<size_t>(crop.w) * 3);
            std::memcpy(&sheet.tiles[static_cast<size_t>(y + cy) * W + x], &crop.tiles[static_cast<size_t>(cy) * crop.w],
                        static_cast<size_t>(crop.w) * 8);
        }
        x += crop.w + kGap;
        shelf = std::max(shelf, crop.h);
    }
    return sheets;
}
