#include "emu/DepthColors.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

namespace
{
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

// Per byte of x, how many of its bits are set (0-8) - summed over a
// block's 8 rows, each byte still fits (at most 64).
inline uint64_t ByteCounts(uint64_t x)
{
    x = x - ((x >> 1) & 0x5555555555555555ull);
    x = (x & 0x3333333333333333ull) + ((x >> 2) & 0x3333333333333333ull);
    return (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0Full;
}

// Word k of a guarded row (pixel x at bit x + 64) shifted so that bit j
// holds the row's pixel 64 k + j + d.
inline uint64_t Shifted(const uint64_t *row, int k, int d)
{
    const int bit = 64 * (k + 1) + d, w = bit >> 6, s = bit & 63;
    return s ? (row[w] >> s) | (row[w + 1] << (64 - s)) : row[w];
}

// Pixels x - 3 to x + 3 of a guarded row, as bits 0-6.
inline unsigned Window7(const uint64_t *row, int x)
{
    const int bit = 64 + x - 3, w = bit >> 6, s = bit & 63;
    const uint64_t v = s ? (row[w] >> s) | (row[w + 1] << (64 - s)) : row[w];
    return static_cast<unsigned>(v & 0x7F);
}

// Costs are 0-1000 (1000: nothing lines up).
constexpr int kNeutral = 500;            // what a disparity costs where there's nothing to tell (capped)
constexpr int kSmallFull = 32;           // pixels a 24x24 window needs to count fully
constexpr int kWideFull = 64;            // ... and an 88-wide strip
constexpr int kWideBlocks = 5;           // the strip: 5 blocks either side
constexpr int kStep = 30, kJump = 250;   // smoothing: a step of 1 in disparity to the next block, any bigger jump
constexpr int kPlaneBonus = 60;          // the pull of the frame's main depths
constexpr int kKeepNow = 154, kKeepLast = 102; // (out of 256: this frame's costs, the last frame's - 60 / 40 %)
constexpr int kSwitch = 2;               // votes a pixel's neighbours' disparity needs over its own block's
constexpr int kPrior = 6;                // cost per pixel of disparity (ties go to the screen's plane)

// Far to near (indigo, violet, rose, orange, gold); per stop the shade 1, 2
// and 3 color. Disparity kFar and beyond is the first, kNear and nearer the last.
constexpr uint8_t kStops[5][3][3] = {
    {{34, 26, 104}, {92, 74, 222}, {176, 166, 255}},
    {{78, 22, 104}, {176, 62, 214}, {236, 160, 255}},
    {{110, 20, 52}, {232, 64, 104}, {255, 168, 176}},
    {{118, 48, 12}, {246, 132, 40}, {255, 212, 136}},
    {{132, 76, 18}, {244, 196, 56}, {255, 242, 170}},
};
constexpr int kFar = 24, kNear = -16;
} // namespace

void DepthColors::Color(int disparity, unsigned shade, uint8_t rgb[3])
{
    if (disparity == kUnknown)
        disparity = 0;
    const unsigned s = shade < 1 ? 0 : shade > 3 ? 2 : shade - 1;
    // t: 0 (far) - 4 (near), in 1/256ths
    int t = (kFar - disparity) * 4 * 256 / (kFar - kNear);
    t = std::max(0, std::min(4 * 256, t));
    const int i = std::min(3, t >> 8), f = t - i * 256;
    for (int c = 0; c < 3; ++c)
        rgb[c] = static_cast<uint8_t>((kStops[i][s][c] * (256 - f) + kStops[i + 1][s][c] * f + 128) >> 8);
}

void DepthColors::Reset()
{
    m_state = State{};
    for (int e = 0; e < 2; ++e)
    {
        m_lastShades[e].clear();
        m_lastWant[e].clear();
        m_disparity[e].clear();
    }
    m_lastDisparity.clear();
    m_haveLast = false;
}

int DepthColors::Votes(unsigned s, const unsigned left[7], int x, int y, int d) const
{
    // How many of the left picture's pixels of this shade in the 7x7 window
    // around (x, y) (left: its rows, see LeftWindow) the right picture shows
    // in the same shade d to the right.
    const std::vector<Row> &r = m_bits[1][s];
    int votes = 0;
    for (int k = 0; k < 7; ++k)
    {
        const int yy = y - 3 + k;
        if (left[k] && yy >= 0 && yy < kHeight)
            votes += CountSet(left[k] & Window7(r[yy].data(), x + d));
    }
    return votes;
}

void DepthColors::LeftWindow(unsigned s, int x, int y, unsigned left[7]) const
{
    const std::vector<Row> &l = m_bits[0][s];
    for (int k = 0; k < 7; ++k)
    {
        const int yy = y - 3 + k;
        left[k] = yy >= 0 && yy < kHeight ? Window7(l[yy].data(), x) : 0;
    }
}

void DepthColors::Update(const uint8_t *const shades[2], const uint8_t *const want[2])
{
    const size_t n = static_cast<size_t>(kWidth) * kHeight;
    if (m_haveLast && std::memcmp(shades[0], m_lastShades[0].data(), n) == 0 && std::memcmp(shades[1], m_lastShades[1].data(), n) == 0 &&
        std::memcmp(want[0], m_lastWant[0].data(), n) == 0 && std::memcmp(want[1], m_lastWant[1].data(), n) == 0)
        return; // both pictures as last time
    for (int e = 0; e < 2; ++e)
    {
        m_disparity[e].assign(n, kUnknown);
        for (int s = 0; s < 3; ++s)
        {
            m_bits[e][s].resize(kHeight);
            for (Row &row : m_bits[e][s])
                row.fill(0);
        }
    }
    // Each eye's shades as bit rows, and the shades anything needs a color in.
    bool wanted[3] = {false, false, false};
    for (int e = 0; e < 2; ++e)
        for (int y = 0; y < kHeight; ++y)
        {
            const uint8_t *row = &shades[e][y * kWidth], *w = &want[e][y * kWidth];
            for (int x = 0; x < kWidth; ++x)
                if (const unsigned s = row[x] & 3)
                {
                    m_bits[e][s - 1][y][1 + (x >> 6)] |= 1ull << (x & 63);
                    wanted[s - 1] |= w[x] != 0;
                }
        }
    if (wanted[0] || wanted[1] || wanted[2])
        Estimate(shades, want);

    // Flat fills: inside an area of one shade (the whole 7x7 window around a
    // pixel that shade) nothing says how far away it is - any shift lines up
    // as well. Each connected area of such pixels takes one surface from the
    // disparities around it (its own shade's pixels just outside it, where
    // its edges or the lines drawn across it say how far away they are): a
    // plane through them (a floor or a wall in perspective - one level if
    // they don't spread), fitted leaving out the ones far off it, kept
    // within the range they span. (Rows taking their own ends' disparities
    // streaked: neighbouring rows meet different lines.)
    {
        auto flat = [&](int x, int y) {
            const unsigned s = shades[0][static_cast<size_t>(y) * kWidth + x];
            if (!s || y < 3 || y >= kHeight - 3 || x < 3 || x >= kWidth - 3)
                return false;
            const std::vector<Row> &l = m_bits[0][s - 1];
            for (int yy = y - 3; yy <= y + 3; ++yy)
                if (Window7(l[yy].data(), x) != 0x7F)
                    return false;
            return true;
        };
        std::vector<int16_t> &left = m_disparity[0];
        m_flatLabel.assign(n, -1);
        for (int y = 3; y < kHeight - 3; ++y)
            for (int x = 3; x < kWidth - 3; ++x)
            {
                const size_t i = static_cast<size_t>(y) * kWidth + x;
                if (want[0][i] && flat(x, y))
                    m_flatLabel[i] = -2; // flat, not yet in an area
            }
        auto inner = [&](int x, int y) { return x >= 3 && x < kWidth - 3 && y >= 3 && y < kHeight - 3; };
        for (size_t seed = 0; seed < n; ++seed)
        {
            if (m_flatLabel[seed] != -2)
                continue;
            // The area (4-connected), and the samples around it.
            m_area.clear();
            m_samples.clear();
            m_area.push_back(static_cast<int32_t>(seed));
            m_flatLabel[seed] = 0;
            const uint8_t shade = shades[0][seed];
            for (size_t k = 0; k < m_area.size(); ++k)
            {
                const int i = m_area[k], x = i % kWidth, y = i / kWidth;
                const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
                for (int j = 0; j < 4; ++j)
                {
                    if (nx[j] < 0 || nx[j] >= kWidth || ny[j] < 0 || ny[j] >= kHeight)
                        continue;
                    const int ni = ny[j] * kWidth + nx[j];
                    if (m_flatLabel[ni] == -2)
                    {
                        m_flatLabel[ni] = 0;
                        m_area.push_back(ni);
                    }
                    else if (m_flatLabel[ni] == -1 && shades[0][ni] == shade && want[0][ni] && left[ni] != kUnknown && inner(nx[j], ny[j]))
                        m_samples.push_back({static_cast<int16_t>(nx[j]), static_cast<int16_t>(ny[j]), left[ni]});
                }
            }
            if (m_samples.empty())
                continue; // nothing says - their blocks' disparities stay
            // A level: the samples' median; then a plane through the ones near it, twice.
            auto median = [&](std::vector<int> &v) {
                std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                return v[v.size() / 2];
            };
            std::vector<int> &ds = m_scratch;
            ds.clear();
            for (const Sample &p : m_samples)
                ds.push_back(p.d);
            const int level = median(ds);
            for (int &d : ds)
                d = std::abs(d - level);
            const int spread = std::max(2, median(ds) * 3);
            double a = level, b = 0, c = 0, cx = 0, cy = 0;
            int lo = level, hi = level;
            for (int round = 0; round < 2; ++round)
            {
                // (centred, in units of 64 pixels, the slopes held back a little: samples along one line say nothing across it)
                double sw = 0, sx = 0, sy = 0;
                for (const Sample &p : m_samples)
                    if (std::abs(p.d - (a + b * (p.x - cx) / 64 + c * (p.y - cy) / 64)) <= spread)
                        sw += 1, sx += p.x, sy += p.y;
                if (sw < 1)
                    break;
                const double mx = sx / sw, my = sy / sw;
                double xx = 0, xy = 0, yy = 0, xd = 0, yd = 0, sd = 0;
                int nlo = 999, nhi = -999;
                for (const Sample &p : m_samples)
                    if (std::abs(p.d - (a + b * (p.x - cx) / 64 + c * (p.y - cy) / 64)) <= spread)
                    {
                        const double u = (p.x - mx) / 64, v = (p.y - my) / 64;
                        xx += u * u, xy += u * v, yy += v * v, xd += u * p.d, yd += v * p.d, sd += p.d;
                        nlo = std::min<int>(nlo, p.d), nhi = std::max<int>(nhi, p.d);
                    }
                const double ridge = 0.02 * sw;
                xx += ridge, yy += ridge;
                const double det = xx * yy - xy * xy;
                a = sd / sw, cx = mx, cy = my;
                b = det > 1e-9 ? (xd * yy - yd * xy) / det : 0;
                c = det > 1e-9 ? (yd * xx - xd * xy) / det : 0;
                lo = nlo, hi = nhi;
            }
            auto surface = [&](int x, int y) {
                const double d = a + b * (x - cx) / 64 + c * (y - cy) / 64;
                return std::max(lo, std::min(hi, static_cast<int>(d < 0 ? d - 0.5 : d + 0.5)));
            };
            for (const int i : m_area)
                left[i] = static_cast<int16_t>(surface(i % kWidth, i / kWidth));
            // Its edge (the 3 pixels its own shade goes on past the flat part,
            // where their windows see what's beside it): those off the surface
            // join it (a band of another color around every fill otherwise).
            size_t from = 0, to = m_area.size();
            for (int ring = 0; ring < 3; ++ring)
            {
                for (size_t k = from; k < to; ++k)
                {
                    const int i = m_area[k], x = i % kWidth, y = i / kWidth;
                    const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
                    for (int j = 0; j < 4; ++j)
                    {
                        if (nx[j] < 0 || nx[j] >= kWidth || ny[j] < 0 || ny[j] >= kHeight)
                            continue;
                        const int ni = ny[j] * kWidth + nx[j];
                        if (m_flatLabel[ni] != -1 || shades[0][ni] != shade || !want[0][ni])
                            continue;
                        m_flatLabel[ni] = -3; // (seen)
                        m_area.push_back(ni);
                        const int d = surface(nx[j], ny[j]);
                        if (left[ni] == kUnknown || std::abs(left[ni] - d) > 2)
                            left[ni] = static_cast<int16_t>(d);
                    }
                }
                from = to, to = m_area.size();
            }
            for (size_t k = m_area.size(); k-- > 0 && m_flatLabel[m_area[k]] == -3;)
                m_flatLabel[m_area[k]] = -1;
        }
    }

    // The right picture: each pixel the disparity of the left pixel it shows
    // (in front wins where two land on one); the rest, their row's nearest
    // such pixel (the same shade within 6 pixels, any within 12), else their
    // block's in the left picture.
    std::vector<int16_t> &right = m_disparity[1];
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x)
        {
            const size_t i = static_cast<size_t>(y) * kWidth + x;
            const int d = m_disparity[0][i];
            if (d == kUnknown)
                continue;
            const int xr = x + d;
            if (xr < 0 || xr >= kWidth)
                continue;
            const size_t j = static_cast<size_t>(y) * kWidth + xr;
            if (shades[1][j] == shades[0][i] && (right[j] == kUnknown || d < right[j]))
                right[j] = static_cast<int16_t>(d);
        }
    std::vector<int16_t> mapped(kWidth);
    for (int y = 0; y < kHeight; ++y)
    {
        const size_t row = static_cast<size_t>(y) * kWidth;
        std::copy(right.begin() + row, right.begin() + row + kWidth, mapped.begin());
        for (int x = 0; x < kWidth; ++x)
        {
            if (!want[1][row + x] || mapped[x] != kUnknown)
                continue;
            const uint8_t shade = shades[1][row + x];
            int found = kUnknown;
            for (int r = 1; r <= 12 && found == kUnknown; ++r)
                for (const int xx : {x - r, x + r})
                    if (xx >= 0 && xx < kWidth && mapped[xx] != kUnknown && (r > 6 || shades[1][row + xx] == shade))
                    {
                        found = mapped[xx];
                        break;
                    }
            if (found == kUnknown && !m_state.best.empty())
                found = m_state.best[(y >> 3) * kBlocksX + (x >> 3)] - kMaxDisparity;
            right[row + x] = static_cast<int16_t>(found == kUnknown ? 0 : found);
        }
    }

    for (int e = 0; e < 2; ++e)
    {
        m_lastShades[e].assign(shades[e], shades[e] + n);
        m_lastWant[e].assign(want[e], want[e] + n);
    }
    m_lastDisparity = m_disparity[0];
    m_haveLast = true;
}

void DepthColors::Estimate(const uint8_t *const shades[2], const uint8_t *const want[2])
{
    State &state = m_state;

    // 1. Per disparity and block: how many of the left picture's pixels
    // the right one shows in the same shade (matches), and how many have
    // their spot on screen at all (inside).
    m_matches.assign(static_cast<size_t>(kDisparities) * kBlocks, 0);
    m_inside.assign(static_cast<size_t>(kDisparities) * kBlocks, 0);
    // (inside: all but the left edge's word for d < 0, the right edge's for d > 0)
    uint64_t edgeMask[kDisparities];
    for (int di = 0; di < kDisparities; ++di)
    {
        const int d = di - kMaxDisparity;
        edgeMask[di] = d < 0 ? ~0ull << -d : d > 0 ? ~0ull >> d : ~0ull;
    }
    for (int by = 0; by < kBlocksY; ++by)
    {
        uint64_t acc[kDisparities][kRowWords] = {};
        uint64_t total[kRowWords] = {}, edge[kDisparities] = {};
        for (int y = by * 8; y < by * 8 + 8; ++y)
        {
            const uint64_t *l1 = m_bits[0][0][y].data(), *l2 = m_bits[0][1][y].data(), *l3 = m_bits[0][2][y].data();
            const uint64_t *r1 = m_bits[1][0][y].data(), *r2 = m_bits[1][1][y].data(), *r3 = m_bits[1][2][y].data();
            for (int k = 0; k < kRowWords; ++k)
            {
                const uint64_t w1 = l1[k + 1], w2 = l2[k + 1], w3 = l3[k + 1], lw = w1 | w2 | w3;
                if (!lw)
                    continue;
                total[k] += ByteCounts(lw);
                for (int di = 0; di < kDisparities; ++di)
                {
                    const int d = di - kMaxDisparity;
                    acc[di][k] += ByteCounts((w1 & Shifted(r1, k, d)) | (w2 & Shifted(r2, k, d)) | (w3 & Shifted(r3, k, d)));
                }
            }
            const uint64_t first = l1[1] | l2[1] | l3[1], last = l1[kRowWords] | l2[kRowWords] | l3[kRowWords];
            for (int di = 0; di < kDisparities; ++di)
            {
                const int d = di - kMaxDisparity;
                if (d < 0)
                    edge[di] += ByteCounts(first & edgeMask[di]);
                else if (d > 0)
                    edge[di] += ByteCounts(last & edgeMask[di]);
            }
        }
        for (int di = 0; di < kDisparities; ++di)
        {
            const int d = di - kMaxDisparity;
            uint8_t *m = &m_matches[static_cast<size_t>(di) * kBlocks + by * kBlocksX];
            uint8_t *in = &m_inside[static_cast<size_t>(di) * kBlocks + by * kBlocksX];
            for (int k = 0; k < kRowWords; ++k)
            {
                const uint64_t t = (d < 0 && k == 0) || (d > 0 && k == kRowWords - 1) ? edge[di] : total[k];
                for (int j = 0; j < 8; ++j)
                {
                    m[k * 8 + j] = static_cast<uint8_t>(acc[di][k] >> (8 * j));
                    in[k * 8 + j] = static_cast<uint8_t>(t >> (8 * j));
                }
            }
        }
    }

    // 2. Costs per block and disparity: how badly the two pictures line up
    // in the 3x3 blocks around it and in the 11-block strip through it (as
    // far as their pixels have their spot on screen - the rest counts
    // kNeutral). Worked out a disparity at a time (disparity-major), then
    // turned block-major for the smoothing.
    static const std::vector<int32_t> kPerMille = [] { // 1000 * 65536 / n: n-ths without dividing
        std::vector<int32_t> t(1024, 0);
        for (int i = 1; i < 1024; ++i)
            t[i] = 1000 * 65536 / i;
        return t;
    }();
    constexpr size_t kVolume = static_cast<size_t>(kBlocks) * kDisparities;
    const bool hadLast = state.estimated && state.cost.size() == kVolume;
    m_costByDisparity.resize(kVolume);
    m_blockPixels.assign(kBlocks, 0);
    int32_t *blockPixels = m_blockPixels.data();
    int16_t sums[4][kBlocks]; // the 3-wide and 11-wide sums of matches and countable pixels, along the rows of blocks
    auto rowSums = [&](int di) {
        // (running sums over the row padded with kWideBlocks empty blocks either side)
        constexpr int kPadded = kBlocksX + 2 * kWideBlocks;
        const uint8_t *M = &m_matches[static_cast<size_t>(di) * kBlocks], *N = &m_inside[static_cast<size_t>(di) * kBlocks];
        for (int by = 0; by < kBlocksY; ++by)
        {
            int16_t pm[kPadded + 1], pn[kPadded + 1];
            for (int i = 0; i <= kWideBlocks; ++i)
                pm[i] = pn[i] = 0;
            for (int bx = 0; bx < kBlocksX; ++bx)
            {
                pm[kWideBlocks + bx + 1] = static_cast<int16_t>(pm[kWideBlocks + bx] + M[by * kBlocksX + bx]);
                pn[kWideBlocks + bx + 1] = static_cast<int16_t>(pn[kWideBlocks + bx] + N[by * kBlocksX + bx]);
            }
            for (int i = kWideBlocks + kBlocksX + 1; i <= kPadded; ++i)
                pm[i] = pm[i - 1], pn[i] = pn[i - 1];
            int16_t *s0 = &sums[0][by * kBlocksX], *s1 = &sums[1][by * kBlocksX], *s2 = &sums[2][by * kBlocksX], *s3 = &sums[3][by * kBlocksX];
            for (int bx = 0; bx < kBlocksX; ++bx)
            {
                s0[bx] = static_cast<int16_t>(pm[bx + kWideBlocks + 2] - pm[bx + kWideBlocks - 1]);
                s1[bx] = static_cast<int16_t>(pn[bx + kWideBlocks + 2] - pn[bx + kWideBlocks - 1]);
                s2[bx] = static_cast<int16_t>(pm[bx + 2 * kWideBlocks + 1] - pm[bx]);
                s3[bx] = static_cast<int16_t>(pn[bx + 2 * kWideBlocks + 1] - pn[bx]);
            }
        }
    };
    // (what each block's 3x3 window holds at all: everything has its spot on screen at disparity 0)
    rowSums(kMaxDisparity);
    for (int by = 0; by < kBlocksY; ++by)
        for (int bx = 0; bx < kBlocksX; ++bx)
        {
            const int b = by * kBlocksX + bx;
            blockPixels[b] = sums[1][b] + (by > 0 ? sums[1][b - kBlocksX] : 0) + (by + 1 < kBlocksY ? sums[1][b + kBlocksX] : 0);
        }
    for (int di = 0; di < kDisparities; ++di)
    {
        rowSums(di);
        const size_t row = static_cast<size_t>(di) * kBlocks;
        const int16_t *last = hadLast ? &state.cost[row] : nullptr;
        int16_t *out = &m_costByDisparity[row];
        for (int by = 0; by < kBlocksY; ++by)
            for (int bx = 0; bx < kBlocksX; ++bx)
            {
                const int b = by * kBlocksX + bx;
                int c = 0;
                if (blockPixels[b])
                {
                    int32_t m = sums[0][b], in = sums[1][b];
                    if (by > 0)
                        m += sums[0][b - kBlocksX], in += sums[1][b - kBlocksX];
                    if (by + 1 < kBlocksY)
                        m += sums[0][b + kBlocksX], in += sums[1][b + kBlocksX];
                    const int w = std::min<int>(in, kSmallFull);
                    const int small = in ? (w * (1000 - ((m * kPerMille[in]) >> 16)) + (kSmallFull - w) * kNeutral) >> 5 : kNeutral;
                    const int32_t wm = sums[2][b], wn = sums[3][b];
                    const int ww = std::min<int>(wn, kWideFull);
                    const int wide = wn ? (ww * (1000 - ((wm * kPerMille[wn]) >> 16)) + (kWideFull - ww) * kNeutral) >> 6 : kNeutral;
                    c = (small + wide) >> 1;
                }
                // (and a little for every pixel away from the screen's plane: where several
                // shifts line up as well - a dithered field, a repeating pattern - the
                // smallest wins, not the first tried)
                c += kPrior * std::abs(di - kMaxDisparity);
                // 3. ... and partly the last frame's (things move a little at a time).
                if (last)
                    c = (c * kKeepNow + last[b] * kKeepLast) >> 8;
                out[b] = static_cast<int16_t>(c);
            }
    }
    state.cost = m_costByDisparity;
    state.estimated = true;
    static_assert(kSmallFull == 32 && kWideFull == 64, "(the shifts above)");

    // 4. The frame's main depths: the most common cheapest disparities of
    // blocks with something in them (weighted by how much), apart by 3 or
    // more - each pulls the blocks near it a little.
    {
        int16_t low[kBlocks];
        uint8_t at[kBlocks];
        for (int b = 0; b < kBlocks; ++b)
            low[b] = m_costByDisparity[b], at[b] = 0;
        for (int di = 1; di < kDisparities; ++di)
        {
            const int16_t *c = &m_costByDisparity[static_cast<size_t>(di) * kBlocks];
            for (int b = 0; b < kBlocks; ++b)
            {
                const bool lower = c[b] < low[b];
                low[b] = lower ? c[b] : low[b];
                at[b] = lower ? static_cast<uint8_t>(di) : at[b];
            }
        }
        int32_t histogram[kDisparities] = {};
        for (int b = 0; b < kBlocks; ++b)
            if (blockPixels[b])
                histogram[at[b]] += std::min(blockPixels[b], 64);
        bool pulled[kDisparities] = {};
        for (int p = 0; p < 4; ++p)
        {
            const int i = static_cast<int>(std::max_element(histogram, histogram + kDisparities) - histogram);
            if (histogram[i] <= 0)
            {
                state.planes[p] = -1;
                continue;
            }
            state.planes[p] = i;
            for (int k = std::max(0, i - 2); k <= std::min(kDisparities - 1, i + 2); ++k)
                histogram[k] = 0;
            for (int k = std::max(0, i - 1); k <= std::min(kDisparities - 1, i + 1); ++k)
                pulled[k] = true;
        }
        for (int di = 0; di < kDisparities; ++di)
            if (pulled[di])
            {
                int16_t *c = &m_costByDisparity[static_cast<size_t>(di) * kBlocks];
                for (int b = 0; b < kBlocks; ++b)
                    if (blockPixels[b])
                        c[b] = static_cast<int16_t>(c[b] - kPlaneBonus);
            }
    }
    // (block-major for the smoothing - transposed in tiles)
    m_cost.resize(kVolume);
    for (int b0 = 0; b0 < kBlocks; b0 += 16)
        for (int di = 0; di < kDisparities; ++di)
        {
            const int16_t *src = &m_costByDisparity[static_cast<size_t>(di) * kBlocks];
            for (int b = b0; b < std::min(b0 + 16, kBlocks); ++b)
                m_cost[static_cast<size_t>(b) * kDisparities + di] = src[b];
        }

    // 5. Smoothed over the block grid: along each row and column of blocks,
    // both ways, a block's cost at a disparity plus the cheapest way to get
    // there from its neighbour's (the same disparity free, one step kStep,
    // any jump kJump) - summed over the four ways, the cheapest wins.
    m_sum.assign(m_cost.size(), 0);
    constexpr int16_t kWall = 16000; // (beyond the disparities: never the cheapest)
    auto pass = [&](int first, int step, int count, int lines, int lineStep) {
        int16_t rows[2][kDisparities + 2];
        for (auto &r : rows)
            r[0] = r[kDisparities + 1] = kWall;
        for (int line = 0; line < lines; ++line)
        {
            int16_t *prev = nullptr;
            for (int k = 0; k < count; ++k)
            {
                const int b = first + line * lineStep + k * step;
                const int16_t *c = &m_cost[static_cast<size_t>(b) * kDisparities];
                int16_t *sum = &m_sum[static_cast<size_t>(b) * kDisparities];
                int16_t *out = rows[k & 1] + 1;
                if (!prev)
                {
                    for (int di = 0; di < kDisparities; ++di)
                        out[di] = c[di];
                }
                else
                {
                    int16_t low = kWall;
                    for (int di = 0; di < kDisparities; ++di)
                        low = std::min(low, prev[di]);
                    const int16_t jump = static_cast<int16_t>(low + kJump);
                    for (int di = 0; di < kDisparities; ++di)
                    {
                        const int16_t step1 = static_cast<int16_t>(std::min(prev[di - 1], prev[di + 1]) + kStep);
                        const int16_t v = std::min(std::min(prev[di], step1), jump);
                        out[di] = static_cast<int16_t>(c[di] + v - low);
                    }
                }
                for (int di = 0; di < kDisparities; ++di)
                    sum[di] = static_cast<int16_t>(sum[di] + out[di]);
                prev = out;
            }
        }
    };
    pass(0, 1, kBlocksX, kBlocksY, kBlocksX);                          // left to right
    pass(kBlocksX - 1, -1, kBlocksX, kBlocksY, kBlocksX);              // right to left
    pass(0, kBlocksX, kBlocksY, kBlocksX, 1);                          // top to bottom
    pass((kBlocksY - 1) * kBlocksX, -kBlocksX, kBlocksY, kBlocksX, 1); // bottom to top
    state.best.resize(kBlocks);
    for (int b = 0; b < kBlocks; ++b)
    {
        const int16_t *sum = &m_sum[static_cast<size_t>(b) * kDisparities];
        state.best[b] = static_cast<int16_t>(std::min_element(sum, sum + kDisparities) - sum);
    }

    // 6. Per wanted left pixel: its block's disparity, unless a neighbour
    // block's or a main depth lines up clearly better in the 7x7 window
    // around it (its own shade's pixels); a pixel that hasn't changed (nor
    // the right picture where it was shown) keeps its last disparity while
    // that still lines up about as well.
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x)
        {
            const size_t i = static_cast<size_t>(y) * kWidth + x;
            if (!want[0][i] || !shades[0][i])
                continue;
            const unsigned s = shades[0][i] - 1u;
            const int bx = x >> 3, by = y >> 3;
            int candidates[13], count = 0;
            auto add = [&](int di) {
                if (di < 0)
                    return;
                for (int k = 0; k < count; ++k)
                    if (candidates[k] == di)
                        return;
                candidates[count++] = di;
            };
            add(state.best[by * kBlocksX + bx]);
            for (int oy = -1; oy <= 1; ++oy)
                for (int ox = -1; ox <= 1; ++ox)
                {
                    const int nx = bx + ox, ny = by + oy;
                    if ((ox || oy) && nx >= 0 && nx < kBlocksX && ny >= 0 && ny < kBlocksY)
                        add(state.best[ny * kBlocksX + nx]);
                }
            for (const int p : state.planes)
                add(p);
            int d = candidates[0] - kMaxDisparity, bestVotes = -1;
            unsigned left[7];
            bool haveLeft = false;
            if (count > 1)
            {
                LeftWindow(s, x, y, left);
                haveLeft = true;
                const int own = Votes(s, left, x, y, d);
                bestVotes = own;
                for (int k = 1; k < count; ++k)
                {
                    const int v = Votes(s, left, x, y, candidates[k] - kMaxDisparity);
                    if (v > own + kSwitch && v > bestVotes)
                    {
                        bestVotes = v;
                        d = candidates[k] - kMaxDisparity;
                    }
                }
            }
            if (m_haveLast && m_lastShades[0][i] == shades[0][i] && m_lastDisparity[i] != kUnknown && m_lastDisparity[i] != d)
            {
                const int last = m_lastDisparity[i], xr = x + last;
                if (xr >= 0 && xr < kWidth && m_lastShades[1][static_cast<size_t>(y) * kWidth + xr] == shades[1][static_cast<size_t>(y) * kWidth + xr])
                {
                    if (!haveLeft)
                        LeftWindow(s, x, y, left);
                    if (bestVotes < 0)
                        bestVotes = Votes(s, left, x, y, d);
                    if (Votes(s, left, x, y, last) >= bestVotes - 1)
                        d = last;
                }
            }
            m_disparity[0][i] = static_cast<int16_t>(d);
        }
}
