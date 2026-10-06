// Python-drivable (ctypes) wrapper around the patched core + the app's own
// ShadeColorizer / TileColorPack / ColorPackRenderer / UncoloredCollector,
// for exploring games and rendering frames exactly as the app does.
#include <libretro.h>
#include "vbgo_tiletrack.h"
#include "emu/ColorPackRenderer.h"
#include "emu/ShadeColorizer.h"
#include "emu/TileColorPack.h"
#include "emu/UncoloredCollector.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace
{
    const uint8_t *g_frame = nullptr;
    unsigned g_w = 0, g_h = 0;
    size_t g_pitch = 0;
    uint32_t g_buttons = 0;
    std::vector<uint8_t> g_rom;
    TileColorPack g_pack;
    ColorPackRenderer g_renderer;
    ShadeColorizer g_colorizer;
    ShadeRgb g_bg{};
    std::array<std::array<uint8_t, 3>, 4> g_pal8{};
    std::vector<uint8_t> g_blob;
    UncoloredCollector g_collector;
    std::vector<UncoloredCollector::Sheet> g_sheets;
    TileColorPack g_import;
    TileColorPack::ImportStats g_importStats;

    void Log(retro_log_level, const char *, ...) {}
    bool Env(unsigned cmd, void *data)
    {
        switch (cmd)
        {
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            static_cast<retro_log_callback *>(data)->log = Log;
            return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            return *static_cast<const retro_pixel_format *>(data) == RETRO_PIXEL_FORMAT_XRGB8888;
        case RETRO_ENVIRONMENT_GET_VARIABLE:
        {
            auto *var = static_cast<retro_variable *>(data);
            if (std::strcmp(var->key, "vb_3dmode") == 0)
            {
                var->value = "side-by-side";
                return true;
            }
            return false;
        }
        default:
            return false;
        }
    }
    void Video(const void *d, unsigned w, unsigned h, size_t pitch)
    {
        if (!d)
            return;
        g_frame = static_cast<const uint8_t *>(d);
        g_w = w;
        g_h = h;
        g_pitch = pitch;
    }
    void Audio(int16_t, int16_t) {}
    size_t AudioBatch(const int16_t *, size_t f) { return f; }
    void Poll() {}
    int16_t Input(unsigned port, unsigned device, unsigned, unsigned id)
    {
        if (port != 0 || device != RETRO_DEVICE_JOYPAD)
            return 0;
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
            return static_cast<int16_t>(g_buttons & 0xFFFF);
        return id < 32 && (g_buttons >> id) & 1;
    }
    void SetPackForRendering()
    {
        g_renderer.SetPack(g_pack.Empty() ? nullptr : &g_pack);
        const std::vector<uint8_t> &cells = g_renderer.FillCells();
        vbgo_tiletrack_set_fill_cells(cells.empty() ? nullptr : cells.data());
        vbgo_tiletrack_set_fill_mode(cells.empty() ? VBGO_TT_FILLS_NONE : VBGO_TT_FILLS_PACK);
    }
} // namespace

extern "C" uint16_t VIP_Read16(int32_t timestamp, uint32_t A);
extern "C"
{
    // The 32 world attribute blocks (16 halfwords each) from VIP memory.
    void vbp_worlds(uint16_t *out)
    {
        for (int w = 0; w < 32; ++w)
            for (int i = 0; i < 16; ++i)
                out[w * 16 + i] = VIP_Read16(0, 0x3D800 + w * 32 + i * 2);
    }

    int vbp_init(const char *path)
    {
        std::ifstream f(path, std::ios::binary);
        g_rom.assign(std::istreambuf_iterator<char>(f), {});
        if (g_rom.empty())
            return 0;
        retro_set_environment(Env);
        retro_set_video_refresh(Video);
        retro_set_audio_sample(Audio);
        retro_set_audio_sample_batch(AudioBatch);
        retro_set_input_poll(Poll);
        retro_set_input_state(Input);
        retro_init();
        retro_game_info info{};
        info.path = path;
        info.data = g_rom.data();
        info.size = g_rom.size();
        return retro_load_game(&info) ? 1 : 0;
    }

    void vbp_track(int on) { vbgo_tiletrack_set_enabled(on != 0); }
    // 0 = none, 1 = pack cells, 2 = all (authoring: captures)
    void vbp_fill_mode(int mode) { vbgo_tiletrack_set_fill_mode(mode == 2 ? VBGO_TT_FILLS_ALL : mode == 1 ? VBGO_TT_FILLS_PACK : VBGO_TT_FILLS_NONE); }

    void vbp_run(uint32_t buttons, int n)
    {
        g_buttons = buttons;
        for (int i = 0; i < n; ++i)
            retro_run();
    }

    double vbp_time(uint32_t buttons, int n)
    {
        g_buttons = buttons;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i)
            retro_run();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }

    int vbp_width() { return static_cast<int>(g_w); }

    void vbp_raw(uint8_t *out)
    {
        for (unsigned y = 0; y < g_h; ++y)
            std::memcpy(out + static_cast<size_t>(y) * g_w * 4, g_frame + y * g_pitch, g_w * 4);
    }

    // Tile records, both eyes (2 * 384 * 224), as the app's captures get them.
    void vbp_tiles(uint64_t *out)
    {
        if (!vbgo_tiletrack_records(0, out, nullptr, false))
            std::memset(out, 0, VBGO_TT_EYE_PIXELS * 8);
        if (!vbgo_tiletrack_records(1, out + VBGO_TT_EYE_PIXELS, nullptr, false))
            std::memset(out + VBGO_TT_EYE_PIXELS, 0, VBGO_TT_EYE_PIXELS * 8);
    }

    // Raw eye-view tags, both eyes (2 * 224 * 384, row-major); 0 where not drawn this frame.
    int vbp_eyetags(uint64_t *out)
    {
        for (unsigned eye = 0; eye < 2; ++eye)
        {
            vbgo_tt_eye_view v;
            if (!vbgo_tiletrack_eye_view(eye, &v))
                return 0;
            for (int x = 0; x < VBGO_TT_WIDTH; ++x)
                for (int y = 0; y < VBGO_TT_HEIGHT; ++y)
                {
                    const uint64_t t = v.columns[x] ? v.columns[x][y] : 0;
                    out[eye * VBGO_TT_EYE_PIXELS + y * VBGO_TT_WIDTH + x] = v.columns[x] && (t >> 48) == v.stamp ? t : 0;
                }
        }
        return 1;
    }

    void vbp_palette(const float *p);
    // Automatic colors on/off (with the Auto base palette as the Multicolor one); clears the pack if asked.
    void vbp_auto(int on, int clearPack)
    {
        if (clearPack)
        {
            g_pack.Clear();
            SetPackForRendering();
        }
        g_renderer.SetAutoColors(on != 0);
        if (on)
        {
            float p[12];
            for (int i = 0; i < 4; ++i)
                for (int c = 0; c < 3; ++c)
                    p[i * 3 + c] = AutoColors::kBase[i][c] / 255.0f;
            vbp_palette(p);
        }
    }

    // How the renderer saw the last frame's worlds: per world (32 x 4 int16):
    // eyes (bit 0 L, bit 1 R), type, partner (-1), the world whose colors it takes;
    // then per world (32 x 28): a pair's right world's disparity per band (0x7FFF unknown).
    void vbp_worldinfo(int16_t *out)
    {
        const auto &w = g_renderer.Worlds();
        for (int i = 0; i < 32; ++i)
        {
            out[i * 4] = w[i].eyes;
            out[i * 4 + 1] = w[i].type;
            out[i * 4 + 2] = w[i].partner;
            out[i * 4 + 3] = w[i].colors;
            for (int b = 0; b < VBGO_TT_HEIGHT / 8; ++b)
                out[128 + i * 28 + b] = static_cast<int16_t>(g_renderer.PairDisparity(i, b));
        }
    }

    // A pair's per-block disparities (28 x 48 int16, 0x7FFF unknown).
    void vbp_block_disparity(unsigned rightWorld, int sprites, int16_t *out)
    {
        for (unsigned by = 0; by < VBGO_TT_HEIGHT / 8; ++by)
            for (unsigned bx = 0; bx < VBGO_TT_WIDTH / 8; ++bx)
                out[by * (VBGO_TT_WIDTH / 8) + bx] = static_cast<int16_t>(g_renderer.PairBlockDisparity(rightWorld, bx, by, sprites != 0));
    }

    // The last painted frame's right eye: 1 where a right picture showed a
    // tile of its own (what a right-eye capture marks). 0 if no pairs.
    int vbp_right_own(uint8_t *out)
    {
        const auto &own = g_renderer.RightPictureOwn();
        if (own.size() != VBGO_TT_EYE_PIXELS)
            return 0;
        std::memcpy(out, own.data(), own.size());
        return 1;
    }

    // Milliseconds for coloring the current frame like the app's UploadFrame:
    // Colorize the whole frame + the renderer's Paint, best of reps runs
    // (each on a fresh copy, after a warm-up - so caches are what they'd be).
    double vbp_time_paint(int reps)
    {
        // As the app lays the frame out (see Emulator::UploadFrame): the
        // core's buffer and the colored one 1024 pixels a row, the eyes side
        // by side - two big buffers, so (allocated alike) the same offset
        // into a 4 KB page (VBP_SKEW: shift the colored one by that many bytes).
        constexpr uint32_t kWidth = 1024;
        const size_t n = static_cast<size_t>(kWidth) * g_h;
        static std::vector<uint8_t> raw, bgraBuffer;
        static const size_t skew = getenv("VBP_SKEW") ? strtoul(getenv("VBP_SKEW"), nullptr, 0) : 0;
        raw.assign(n * 4, 0);
        bgraBuffer.resize(n * 4 + 4096);
        uint8_t *bgra = bgraBuffer.data() + ((reinterpret_cast<uintptr_t>(raw.data()) - reinterpret_cast<uintptr_t>(bgraBuffer.data()) + skew) & 4095);
        {
            std::vector<uint8_t> frame(static_cast<size_t>(g_w) * g_h * 4);
            vbp_raw(frame.data());
            for (unsigned y = 0; y < g_h; ++y)
                std::memcpy(&raw[static_cast<size_t>(y) * kWidth * 4], &frame[static_cast<size_t>(y) * g_w * 4], g_w * 4);
        }
        const uint32_t eyeOffset[2] = {0, g_w - VBGO_TT_WIDTH};
        double best = 1e9;
        for (int k = 0; k < reps; ++k)
        {
            auto t0 = std::chrono::steady_clock::now();
            for (unsigned y = 0; y < g_h; ++y)
                g_colorizer.Colorize(&raw[static_cast<size_t>(y) * kWidth * 4], &bgra[static_cast<size_t>(y) * kWidth * 4], g_w);
            if (g_renderer.Active())
                g_renderer.Paint(bgra, raw.data(), kWidth, eyeOffset, g_bg);
            best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        return best;
    }

    // Rows of the tile with that hash, from the current character memory.
    int vbp_tile_rows(uint32_t hash, uint16_t *rows)
    {
        const uint16_t *c = vbgo_tiletrack_chr_ram();
        if (!c)
            return 0;
        for (int s = 0; s < 2048; ++s)
            if (vbgo_tiletrack_hash_rows(&c[s * 8]) == hash)
            {
                std::memcpy(rows, &c[s * 8], 16);
                return 1;
            }
        return 0;
    }

    int vbp_chr(uint16_t *out)
    {
        const uint16_t *c = vbgo_tiletrack_chr_ram();
        if (!c)
            return 0;
        std::memcpy(out, c, 2048 * 8 * 2);
        return 1;
    }

    size_t vbp_state_size() { return retro_serialize_size(); }
    int vbp_save(uint8_t *buf, size_t n) { return retro_serialize(buf, n) ? 1 : 0; }
    int vbp_load(const uint8_t *buf, size_t n) { return retro_unserialize(buf, n) ? 1 : 0; }

    int vbp_pack_load(const uint8_t *bytes, size_t n)
    {
        g_pack.Clear();
        if (!g_pack.Deserialize(std::vector<uint8_t>(bytes, bytes + n)))
            return -1;
        SetPackForRendering();
        return static_cast<int>(g_pack.TileCount());
    }

    void vbp_palette(const float *p)
    {
        std::array<ShadeRgb, 4> pal;
        for (int i = 0; i < 4; ++i)
            pal[i] = ShadeRgb{p[i * 3], p[i * 3 + 1], p[i * 3 + 2]};
        g_colorizer.SetPalette(pal);
        g_bg = pal[0];
        for (int i = 0; i < 4; ++i)
            for (int c = 0; c < 3; ++c)
                g_pal8[i][c] = static_cast<uint8_t>(p[i * 3 + c] * 255.0f + 0.5f);
    }

    // Left eye as the app shows it in Multicolor mode: Colorize + the pack
    // through ColorPackRenderer. out: 384x224 RGB. painted (optional, 384x224):
    // 1 where the pack changed the pixel.
    void vbp_render(uint8_t *out, int usePack, uint8_t *painted)
    {
        const size_t n = static_cast<size_t>(g_w) * g_h;
        std::vector<uint8_t> raw(n * 4), bgra(n * 4), plain;
        vbp_raw(raw.data());
        g_colorizer.Colorize(raw.data(), bgra.data(), n);
        if (painted)
            plain = bgra;
        if (usePack && g_renderer.Active())
        {
            const uint32_t eyeOffset[2] = {0, g_w - VBGO_TT_WIDTH};
            g_renderer.Paint(bgra.data(), raw.data(), g_w, eyeOffset, g_bg);
        }
        for (int y = 0; y < VBGO_TT_HEIGHT; ++y)
            for (int x = 0; x < VBGO_TT_WIDTH; ++x)
            {
                const size_t i = (static_cast<size_t>(y) * g_w + x) * 4;
                uint8_t *o = &out[(y * VBGO_TT_WIDTH + x) * 3];
                o[0] = bgra[i + 2];
                o[1] = bgra[i + 1];
                o[2] = bgra[i + 0];
                if (painted)
                    painted[y * VBGO_TT_WIDTH + x] = std::memcmp(&bgra[i], &plain[i], 3) != 0;
            }
    }

    // Both eyes (g_w x g_h RGB, side by side) as the app shows them, plus the
    // right eye's offset.
    int vbp_render_full(uint8_t *out, int usePack)
    {
        const size_t n = static_cast<size_t>(g_w) * g_h;
        std::vector<uint8_t> raw(n * 4), bgra(n * 4);
        vbp_raw(raw.data());
        g_colorizer.Colorize(raw.data(), bgra.data(), n);
        if (usePack && g_renderer.Active())
        {
            const uint32_t eyeOffset[2] = {0, g_w - VBGO_TT_WIDTH};
            g_renderer.Paint(bgra.data(), raw.data(), g_w, eyeOffset, g_bg);
        }
        for (size_t i = 0; i < n; ++i)
        {
            out[i * 3 + 0] = bgra[i * 4 + 2];
            out[i * 3 + 1] = bgra[i * 4 + 1];
            out[i * 3 + 2] = bgra[i * 4 + 0];
        }
        return static_cast<int>(g_w - VBGO_TT_WIDTH);
    }

    void vbp_collect_reset() { g_collector.Reset(); }
    int vbp_collect_frame()
    {
        std::vector<uint8_t> raw(static_cast<size_t>(g_w) * g_h * 4);
        vbp_raw(raw.data());
        std::vector<uint64_t> rec(VBGO_TT_EYE_PIXELS);
        if (!vbgo_tiletrack_records(0, rec.data(), nullptr, false))
            return 0;
        return g_collector.AddFrame(rec.data(), raw.data(), g_w, g_pack, g_pal8);
    }
    long vbp_collect_stats(int which) { return which ? static_cast<long>(g_collector.CollectedTilePixels()) : static_cast<long>(g_collector.PendingCrops()); }
    int vbp_collect_take() { g_sheets = g_collector.TakeSheets(g_pal8[0]); return static_cast<int>(g_sheets.size()); }
    void vbp_sheet(int i, uint8_t *rgb, uint64_t *tiles)
    {
        std::memcpy(rgb, g_sheets[i].rgb.data(), g_sheets[i].rgb.size());
        std::memcpy(tiles, g_sheets[i].tiles.data(), g_sheets[i].tiles.size() * 8);
    }

    void vbp_import_begin() { g_import.Clear(); g_importStats = {}; }
    int vbp_import_add(const uint8_t *rgb, int w, int h, const uint8_t *sidecar, size_t n)
    {
        return g_import.AddPainting(rgb, w, h, 3, std::vector<uint8_t>(sidecar, sidecar + n), g_importStats) ? 1 : 0;
    }
    int vbp_import_finish()
    {
        g_import.FinishImport(g_importStats);
        g_pack = g_import;
        SetPackForRendering();
        return static_cast<int>(g_pack.TileCount());
    }
    size_t vbp_paint_bytes(uint8_t *out)
    {
        g_blob = g_pack.Serialize();
        if (out)
            std::memcpy(out, g_blob.data(), g_blob.size());
        return g_blob.size();
    }
    // Like the app's F10 (CaptureTileReference): left-eye records with fills
    // and map cells, invisible pixels dropped. Returns the game's brightness
    // level (most common), or -1. (Fill mode must be ALL while drawing.)
    int vbp_capture_eye(unsigned eye, uint64_t *records, uint32_t *cells)
    {
        if (!vbgo_tiletrack_records(eye, records, cells, true))
            return -1;
        const unsigned off = eye ? g_w - VBGO_TT_WIDTH : 0;
        uint32_t levels[64] = {};
        for (int y = 0; y < VBGO_TT_HEIGHT; ++y)
            for (int x = 0; x < VBGO_TT_WIDTH; ++x)
            {
                const size_t i = static_cast<size_t>(y) * VBGO_TT_WIDTH + x;
                const uint8_t *raw = g_frame + y * g_pitch + (off + x) * 4;
                if (VBGO_TT_PIXEL(records[i]) && (raw[0] | raw[1] | raw[2]) == 0)
                    records[i] = 0, cells[i] = 0;
                ++levels[raw[3] >> 2];
            }
        int best = 0;
        for (int i = 1; i < 64; ++i)
            if (levels[i] > levels[best])
                best = i;
        return best;
    }

    int vbp_capture(uint64_t *records, uint32_t *cells) { return vbp_capture_eye(0, records, cells); }

    // The pack's color for one tile pixel, looked up like ColorPackRenderer
    // (without map cells): palette variant, else layer variant, else the
    // tile's own. 1 = colored (rgb), 0 = unpainted, -1 = left uncolored.
    int vbp_lookup(uint32_t hash, unsigned world, unsigned palette, unsigned index, uint8_t *rgb)
    {
        const TileColorPack::Tile *base = g_pack.Find(hash);
        const TileColorPack::Tile *tile = g_pack.FindPalette(hash, palette);
        if (!tile)
            tile = base;
        if (tile == base)
            if (const TileColorPack::Tile *layer = g_pack.FindLayer(hash, world))
                tile = layer;
        if (tile && (tile->mask >> index & 1))
        {
            std::memcpy(rgb, tile->rgb[index], 3);
            return 1;
        }
        return g_pack.IsLeftUncolored(hash, index) ? -1 : 0;
    }

    void vbp_import_stats(uint64_t *out)
    {
        const auto &s = g_importStats;
        out[0] = s.paintings; out[1] = s.sheets; out[2] = s.tilePixels; out[3] = s.fromSheets; out[4] = s.erased;
        out[5] = s.layerPixels; out[6] = s.inconsistent; out[7] = s.mergedColors;
        out[8] = s.palettePixels; out[9] = s.cellPixels; out[10] = s.fillPixels;
        out[11] = s.contextTiles; out[12] = s.contextGroups; out[13] = s.rightPixels;
    }

    uint8_t *vbp_wram() { return static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM)); }

    uint8_t *vbp_sram() { return static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM)); }
    size_t vbp_sram_size() { return retro_get_memory_size(RETRO_MEMORY_SAVE_RAM); }
}
