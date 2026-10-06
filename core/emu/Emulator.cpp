#include "emu/Emulator.h"
#include "emu/vbgo_tiletrack.h"
#include "io/Settings.h"

#include <libretro.h>

#include <stb_image.h> // decoder for color-pack paintings (implementation in VulkanRenderer.cpp)

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO // only the in-memory writer is used (platform file I/O does the rest)
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace
{
    // libretro's C API has no per-instance context pointer on its callbacks, so
    // there's nowhere to stash a `this` - this whole block is necessarily
    // global/singleton state, same as the core itself only supports one loaded
    // game at a time. Fine here since the app only ever has one Emulator.
    constexpr uint32_t kFbWidth = 384 * 2 + 256; // matches libretro.cpp's FB_WIDTH
    constexpr uint32_t kFbHeight = 224 * 2;      // matches libretro.cpp's FB_HEIGHT

    // Set by the video_cb callback each retro_run() call. The pointer is always
    // the core's own persistent surf.pixels buffer (allocated once in
    // retro_load_game, kFbWidth*kFbHeight*4 bytes) - never null once a ROM is
    // loaded, so it's safe to memcpy the whole fixed-size buffer every time
    // regardless of what the current DisplayRect sub-region actually is.
    const void *g_pendingFrame = nullptr;
    unsigned g_pendingWidth = 0;
    unsigned g_pendingHeight = 0;
    bool g_frameReady = false;

    // Set by Emulator::SetGameplayInput each app frame, read back by
    // RetroInputState - see VBButtonBit in Emulator.h for what each bit means.
    uint32_t g_joypadBitmask = 0;

    // Set by Emulator::Initialize to &m_audioOutput - same "necessarily global"
    // reasoning as the rest of this block; RetroAudioSampleBatch forwards the
    // core's samples through it. Null (and RetroAudioSampleBatch a no-op) until
    // then, and whenever AudioOutput::Initialize() itself failed (e.g. no
    // audio device on this machine) - PushSamples nonetheless already no-ops
    // when uninitialized, but the null check here also covers Emulator not
    // having called Initialize() at all yet.
    AudioOutput *g_audioOutput = nullptr;
    // While recording (see Emulator::ToggleRecording): the game's audio goes there too.
    FrameRecorder *g_recorder = nullptr;

    // The core has at least one call site (SettingChanged's "3D mode changed"
    // log line) that calls log_cb unconditionally with no null check, unlike
    // every other log_cb use in libretro.cpp - so GET_LOG_INTERFACE can't just
    // return false like the rest of the environment calls this pass doesn't
    // otherwise care about. Without this, that log line dereferences a null
    // function pointer the moment the core changes 3D mode during
    // retro_load_game, crashing (found via a real access violation there).
    // A log line: logcat on Android (tag VBoyColor), stderr elsewhere.
    void LogLine(const char *fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
#ifdef __ANDROID__
        __android_log_vprint(ANDROID_LOG_INFO, "VBoyColor", fmt, args);
#else
        std::vfprintf(stderr, fmt, args);
        std::fputc('\n', stderr);
#endif
        va_end(args);
    }

    void RetroLogPrintf(retro_log_level, const char *fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        std::vfprintf(stderr, fmt, args);
        va_end(args);
    }

    bool RetroEnvironment(unsigned cmd, void *data)
    {
        switch (cmd)
        {
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        {
            auto *cb = static_cast<retro_log_callback *>(data);
            if (!cb)
                return false;
            cb->log = RetroLogPrintf;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        {
            // Only XRGB8888 is supported here - UiRenderer's streaming texture
            // is created as VK_FORMAT_B8G8R8A8_SRGB, which is exactly this
            // format's in-memory byte order (Rshift16/Gshift8/Bshift0/Ashift24
            // on little-endian == bytes B,G,R,A) - no CPU-side conversion. The
            // _SRGB (not _UNORM) tag matters too: the core's output is
            // gamma-encoded (see CaptureScreenshotGrayscale's linearize step),
            // and ui_image.frag/screen_pattern.frag assume their source
            // texture auto-linearizes on sample, matching every other _SRGB
            // image in this renderer - a _UNORM tag here would read those
            // bytes as already-linear, and the sRGB swapchain would then
            // gamma-encode them a second time on write.
            const auto *fmt = static_cast<const retro_pixel_format *>(data);
            return fmt && *fmt == RETRO_PIXEL_FORMAT_XRGB8888;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE:
        {
            auto *var = static_cast<retro_variable *>(data);
            if (!var || !var->key)
                return false;
            // Force side-by-side stereo - see Emulator.h's class comment. Every
            // other variable this core asks about just falls back to its own
            // built-in default (returning false here is safe - libretro.cpp
            // guards every other GET_VARIABLE call with `&& var.value`).
            if (std::strcmp(var->key, "vb_3dmode") == 0)
            {
                var->value = "side-by-side";
                return true;
            }
            return false;
        }
        // Silently accept/ignore anything else this core probes for (performance
        // interface, input descriptors, geometry updates, overscan, variable-
        // update polling, ...) - none of it is needed for video-only playback
        // this pass.
        default:
            return false;
        }
    }

    void RetroVideoRefresh(const void *data, unsigned width, unsigned height, size_t /*pitch*/)
    {
        if (!data)
            return; // core is signalling "same as last frame" (e.g. hardware-render path) - not used by this core

        g_pendingFrame = data;
        g_pendingWidth = width;
        g_pendingHeight = height;
        g_frameReady = true;
    }

    // The core only ever calls the batch variant below (see libretro.cpp's
    // audio_batch_cb usage) - this one's wired up solely because
    // retro_set_audio_sample still requires a non-null callback.
    void RetroAudioSampleNoop(int16_t, int16_t) {}

    size_t RetroAudioSampleBatch(const int16_t *data, size_t frames)
    {
        if (g_audioOutput)
            g_audioOutput->PushSamples(data, frames);
        if (g_recorder)
            g_recorder->AddAudio(data, frames);
        return frames;
    }

    // No-op: g_joypadBitmask is refreshed once per app frame by
    // Emulator::SetGameplayInput (called before RunFrame's retro_run() calls),
    // not by polling a device here.
    void RetroInputPollNoop() {}

    int16_t RetroInputState(unsigned port, unsigned device, unsigned /*index*/, unsigned id)
    {
        if (port != 0 || device != RETRO_DEVICE_JOYPAD || id > 31)
            return 0;
        return (g_joypadBitmask & (1u << id)) ? 1 : 0;
    }
} // namespace

void Emulator::Initialize(UiRenderer &ui, Platform &platform)
{
    m_ui = &ui;
    m_platform = &platform;

    retro_set_environment(RetroEnvironment);
    retro_set_video_refresh(RetroVideoRefresh);
    retro_set_audio_sample(RetroAudioSampleNoop);
    retro_set_audio_sample_batch(RetroAudioSampleBatch);
    retro_set_input_poll(RetroInputPollNoop);
    retro_set_input_state(RetroInputState);
    retro_init();
    m_coreInitialized = true;

    // Failure (e.g. no audio device on this machine) isn't fatal - g_audioOutput
    // stays set either way, PushSamples itself no-ops while uninitialized.
    m_audioOutput.Initialize();
    g_audioOutput = &m_audioOutput;

    // Fixed max-size streaming texture (see UiRenderer::CreateStreamingImage)
    // - big enough for any 3D mode the core supports, even though we only
    // ever force side-by-side. DrawScreen UV-crops to whatever the current
    // frame's actual valid region is.
    m_screenTexture = ui.CreateStreamingImage(kFbWidth, kFbHeight, VK_FORMAT_B8G8R8A8_SRGB);
    m_frameBufferRgba.resize(static_cast<size_t>(kFbWidth) * kFbHeight * 4);
    m_rawFrame.resize(m_frameBufferRgba.size());
    m_records.resize(VBGO_TT_EYE_PIXELS);
}

bool Emulator::LoadRom(const std::string &romPath, const std::string &displayName)
{
    if (!m_coreInitialized)
        return false;

    const std::vector<uint8_t> romBytes = m_platform->ReadRomFile(romPath);
    if (romBytes.empty())
        return false;

    if (m_romLoaded)
    {
        // Uncolored objects collected so far belong to the outgoing ROM -
        // save them as its sheets, then keep collecting for the new one.
        if (m_collecting)
        {
            std::fprintf(stderr, "[Emulator] %s\n", SetCollectingUncolored(false).c_str());
            m_collecting = true;
        }
        m_collector.Reset();
        // Must flush the outgoing ROM's SRAM before unloading - the core's
        // SRAM pointer isn't valid once the game is unloaded.
        SaveRam();
        retro_unload_game();
        m_romLoaded = false;
    }

    retro_game_info info{};
    info.path = romPath.c_str(); // need_fullpath is false, but the core prefers a real path over null
    info.data = romBytes.data();
    info.size = romBytes.size();

    m_romLoaded = retro_load_game(&info);
    m_frameAccumulator = 0.0f;
    std::fprintf(stderr, "[Emulator] LoadRom(\"%s\"): %zu bytes read, retro_load_game -> %s\n", romPath.c_str(),
                 romBytes.size(), m_romLoaded ? "success" : "FAILED");

    if (m_romLoaded)
    {
        m_romBaseName = displayName.empty() ? "rom" : displayName;
        m_romCrc = TileColorPack::Crc32(romBytes.data(), romBytes.size());
        m_romSize = static_cast<uint32_t>(romBytes.size());
        LoadRam();
        std::fprintf(stderr, "[Emulator] %s\n", ReloadColorPack().c_str());
    }

    return m_romLoaded;
}

void Emulator::SetGameplayInput(uint32_t joypadBitmask) { g_joypadBitmask = joypadBitmask; }

bool Emulator::ResetGame()
{
    if (!m_romLoaded)
        return false;
    g_joypadBitmask = 0;
    m_frameAccumulator = 0.0f;
    g_frameReady = false;
    retro_reset();
    return true;
}

void Emulator::RunFrame(float deltaSeconds)
{
    if (!m_romLoaded)
        return;

    const float framePeriod = 1.0f / kCoreFps;
    m_frameAccumulator += deltaSeconds;

    // Cap catch-up so a debugger pause/hitch doesn't spin retro_run() an
    // enormous number of times on the next real frame.
    const float kMaxCatchUp = framePeriod * 4.0f;
    if (m_frameAccumulator > kMaxCatchUp)
        m_frameAccumulator = kMaxCatchUp;

    bool ranAny = false;
    int runs = 0;
    while (m_frameAccumulator >= framePeriod)
    {
        const auto runStart = std::chrono::steady_clock::now();
        retro_run();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - runStart).count();
        m_emulationMs += ms;
        m_emulationMaxMs = std::max(m_emulationMaxMs, ms);
        ++m_timedFrames;
        m_frameAccumulator -= framePeriod;
        ranAny = true;
        ++runs;
        // Recording: every emulated frame, not just the last one an app
        // frame shows.
        if (m_recorder.Active() && g_frameReady && m_ui)
        {
            PresentFrame();
            RecordFrame();
        }
    }
    if (runs > 1)
        ++m_catchUps;

    if (ranAny && g_frameReady && m_ui)
        PresentFrame();

    // Frame times, logged every few seconds - to check them on the headset
    // (logcat: VBoyColor). Both run on the render thread, once per
    // emulated frame (50 a second); "behind" counts app frames that had to
    // run the core more than once to catch up (the app missed frames).
    if (m_timedFrames >= 250)
    {
        // (an unoptimized build's times say little about an optimized one's)
#if defined(__OPTIMIZE__) || (defined(_MSC_VER) && defined(NDEBUG))
        const char *build = "";
#else
        const char *build = " - unoptimized build: build Release to measure";
#endif
        char coloring[96] = "no coloring";
        if (m_coloringFrames > 0)
            std::snprintf(coloring, sizeof(coloring), "coloring both eyes %.2f ms (%.2f at most)",
                          m_coloringMs / m_coloringFrames, m_coloringMaxMs);
        LogLine("[Emulator] A frame: emulation %.2f ms on average (%.2f at most), %s - last %d frames, behind %d times%s",
                m_emulationMs / m_timedFrames, m_emulationMaxMs, coloring, m_timedFrames, m_catchUps, build);
        m_emulationMs = m_emulationMaxMs = m_coloringMs = m_coloringMaxMs = 0.0;
        m_timedFrames = m_coloringFrames = m_catchUps = 0;
    }
}

void Emulator::PresentFrame()
{
    std::memcpy(m_rawFrame.data(), g_pendingFrame, m_rawFrame.size());
    m_hasFrame = true;
    if (m_collecting && vbgo_tiletrack_is_enabled() && vbgo_tiletrack_records(0, m_records.data(), nullptr, false))
        m_collector.AddFrame(m_records.data(), m_rawFrame.data(), kFbWidth, m_colorPack, CapturePalette());
    if (m_shadePaletteIndex >= 0 && m_shadeColorizer.IsGradient())
        m_shadeColorizer.Observe(m_rawFrame.data(), std::min<uint32_t>(m_lastFrameWidth, kFbWidth),
                                 std::min<uint32_t>(m_lastFrameHeight, kFbHeight), static_cast<size_t>(kFbWidth) * 4);
    UploadFrame();
    m_lastFrameWidth = g_pendingWidth > 0 ? g_pendingWidth : kSideBySideWidth;
    m_lastFrameHeight = g_pendingHeight > 0 ? g_pendingHeight : kSideBySideHeight;
    g_frameReady = false;
}

std::string Emulator::ToggleRecording()
{
    if (m_recorder.Active())
    {
        g_recorder = nullptr;
        const size_t frames = m_recorder.Stop();
        char line[512];
        std::snprintf(line, sizeof(line),
                      "Recorded %zu frames (%.1f s) into roms/%s/: original/ (red, as the hardware shows it), colored/ "
                      "(as the app colors it), game audio.wav - import each folder as an image sequence at 50 fps",
                      frames, frames / 50.0, m_recorder.Folder().c_str());
        return line;
    }
    if (!m_romLoaded || !m_platform)
        return "Recording: no game running";
    std::string folder;
    for (int n = 1; n < 1000 && folder.empty(); ++n)
    {
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), " %03d", n);
        if (!m_platform->RomsFileExists("recordings/" + m_romBaseName + suffix, false))
            folder = "recordings/" + m_romBaseName + suffix;
    }
    if (folder.empty() || !m_recorder.Start(m_platform, folder, VBGO_TT_WIDTH, VBGO_TT_HEIGHT, AudioOutput::kSampleRate))
        return "Recording: couldn't start";
    // (the folder exists from the first frame on, so the next recording picks the next number)
    g_recorder = &m_recorder;
    return "Recording into roms/" + folder + "/ - press again to stop (stops on its own after a minute)";
}

void Emulator::RecordFrame()
{
    // The left eye, twice: the original's red (the core's shade brightness in
    // the red channel - the hardware's look, the app's red Tint), and the
    // frame as colored for the screen (BGRA).
    std::vector<uint8_t> original(static_cast<size_t>(VBGO_TT_WIDTH) * VBGO_TT_HEIGHT * 3, 0), colored(original.size());
    for (uint32_t y = 0; y < VBGO_TT_HEIGHT; ++y)
    {
        const uint8_t *raw = &m_rawFrame[static_cast<size_t>(y) * kFbWidth * 4];
        const uint8_t *col = &m_frameBufferRgba[static_cast<size_t>(y) * kFbWidth * 4];
        uint8_t *o = &original[static_cast<size_t>(y) * VBGO_TT_WIDTH * 3];
        uint8_t *c = &colored[static_cast<size_t>(y) * VBGO_TT_WIDTH * 3];
        for (uint32_t x = 0; x < VBGO_TT_WIDTH; ++x)
        {
            o[x * 3] = std::max({raw[x * 4], raw[x * 4 + 1], raw[x * 4 + 2]});
            c[x * 3 + 0] = col[x * 4 + 2];
            c[x * 3 + 1] = col[x * 4 + 1];
            c[x * 3 + 2] = col[x * 4 + 0];
        }
    }
    m_recorder.AddFrame(original.data(), colored.data());
    if (m_recorder.Frames() >= 60 * 50)
        std::fprintf(stderr, "[Emulator] %s\n", ToggleRecording().c_str());
}

void Emulator::SetShadePalette(int paletteIndex)
{
    if (paletteIndex != kAutoColors && (paletteIndex < 0 || paletteIndex >= kShadePaletteCount))
        paletteIndex = -1;
    if (paletteIndex == m_shadePaletteIndex)
        return;

    m_shadePaletteIndex = paletteIndex;
    m_packRenderer.SetAutoColors(paletteIndex == kAutoColors);
    if (paletteIndex == kAutoColors)
    {
        // Automatic colors are painted from the tile tracker's tags (see
        // ColorPackRenderer); the base palette only shows where nothing
        // tracked is drawn, and is what they fade toward.
        std::array<ShadeRgb, 4> palette;
        for (int i = 0; i < 4; ++i)
            palette[i] = ShadeRgb{AutoColors::kBase[i][0] / 255.0f, AutoColors::kBase[i][1] / 255.0f, AutoColors::kBase[i][2] / 255.0f};
        m_shadeColorizer.SetPalette(palette);
        m_shadeBackground = m_shadeColorizer.Palette()[0];
    }
    else if (paletteIndex >= 0)
    {
        const int pattern = GradientOfShadePalette(paletteIndex);
        if (pattern >= 0)
        {
            // All 5 stops, each shade where the game's brightness for it
            // falls on the gradient (see ShadeColorizer::SetGradient) -
            // relative to the color pack's reference brightness if there's
            // one, else the brightest the game has shown.
            std::array<ShadeRgb, 5> stops;
            for (int i = 0; i < 5; ++i)
            {
                const XrColor4f &c = kScreenPatterns[pattern][i];
                stops[i] = ShadeRgb{c.r, c.g, c.b};
            }
            m_shadeColorizer.SetGradient(stops, m_colorPack.Empty() ? -1 : m_colorPack.ReferenceLevel());
            if (m_hasFrame)
                m_shadeColorizer.Observe(m_rawFrame.data(), std::min<uint32_t>(m_lastFrameWidth, kFbWidth),
                                         std::min<uint32_t>(m_lastFrameHeight, kFbHeight), static_cast<size_t>(kFbWidth) * 4);
        }
        else
        {
            std::array<ShadeRgb, 4> palette;
            for (int i = 0; i < 4; ++i)
            {
                const XrColor4f &c = kShadePalettes[paletteIndex][i];
                palette[i] = ShadeRgb{c.r, c.g, c.b};
            }
            m_shadeColorizer.SetPalette(palette);
        }
        m_shadeBackground = m_shadeColorizer.Palette()[0];
    }
    UpdateTileTracking();

    // Emulation is paused while the menu is open, so RunFrame won't upload
    // anything new until it closes - re-color the frame already on screen
    // now so palette changes show up immediately.
    if (m_hasFrame && m_ui)
        UploadFrame();
}

void Emulator::UploadFrame()
{
    // Only the part of the core's (bigger, fixed-size) buffer this frame
    // uses - both eyes side by side - is converted and uploaded.
    const auto coloringStart = std::chrono::steady_clock::now();
    const uint32_t width = std::min<uint32_t>(m_lastFrameWidth, kFbWidth);
    const uint32_t height = std::min<uint32_t>(m_lastFrameHeight, kFbHeight);
    const size_t stride = static_cast<size_t>(kFbWidth) * 4;
    for (uint32_t y = 0; y < height; ++y)
    {
        const uint8_t *src = &m_rawFrame[y * stride];
        uint8_t *dst = &m_frameBufferRgba[y * stride];
        if (m_shadePaletteIndex >= 0)
            m_shadeColorizer.Colorize(src, dst, width);
        else
            // Force alpha to opaque while copying - see m_frameBufferRgba's
            // doc comment for why the core's own buffer can't be uploaded
            // directly. (Its top byte also carries the shade index tag, which
            // this mode doesn't need.)
            for (uint32_t x = 0; x < width * 4; x += 4)
            {
                dst[x + 0] = src[x + 0];
                dst[x + 1] = src[x + 1];
                dst[x + 2] = src[x + 2];
                dst[x + 3] = 0xFF;
            }
    }
    // The color pack's colors (unless F8 hid them) and/or the Auto mode's.
    const bool packShown = m_colorPackEnabled && m_packRenderer.HasPack();
    if (m_shadePaletteIndex >= 0 && (packShown || m_packRenderer.AutoColorsOn()) && vbgo_tiletrack_is_enabled())
    {
        const uint32_t eyeOffset[2] = {0, m_lastFrameWidth - VBGO_TT_WIDTH};
        m_packRenderer.SetPackShown(packShown);
        m_packRenderer.Paint(m_frameBufferRgba.data(), m_rawFrame.data(), kFbWidth, eyeOffset, m_shadeBackground);
        // How long coloring both eyes takes (colorize + paint) - logged with
        // the emulation's time (see RunFrame).
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - coloringStart).count();
        m_coloringMs += ms;
        m_coloringMaxMs = std::max(m_coloringMaxMs, ms);
        ++m_coloringFrames;
    }
    if (m_tileDebugView)
        PaintTileDebugView();
    m_ui->UpdateStreamingImage(m_screenTexture, m_frameBufferRgba.data(), height * stride);
}

namespace
{
    // Stable pseudo-random color per tile hash (murmur3 finalizer), kept
    // away from black so tiles never vanish into the background.
    void TileDebugColor(uint32_t hash, unsigned rawPixel, uint8_t out[3])
    {
        uint32_t h = hash;
        h ^= h >> 16;
        h *= 0x85EBCA6Bu;
        h ^= h >> 13;
        h *= 0xC2B2AE35u;
        h ^= h >> 16;
        const float shade = 0.45f + 0.55f * (rawPixel / 3.0f);
        for (int c = 0; c < 3; ++c)
            out[c] = static_cast<uint8_t>((50.0f + 0.75f * ((h >> (c * 8)) & 0xFF)) * shade);
    }

    void AppendLe32(std::vector<uint8_t> &v, uint32_t x)
    {
        for (int i = 0; i < 4; ++i)
            v.push_back(static_cast<uint8_t>(x >> (i * 8)));
    }
} // namespace

void Emulator::PaintTileDebugView()
{
    if (!vbgo_tiletrack_is_enabled())
        return;
    // Side-by-side frame: left eye at x 0, right eye after it (plus any
    // separation the core was configured with - none by default).
    const uint32_t eyeOffset[2] = {0, m_lastFrameWidth - VBGO_TT_WIDTH};
    for (unsigned eye = 0; eye < 2; ++eye)
    {
        if (!vbgo_tiletrack_records(eye, m_records.data(), nullptr, false))
            continue;
        for (uint32_t y = 0; y < VBGO_TT_HEIGHT; ++y)
        {
            for (uint32_t x = 0; x < VBGO_TT_WIDTH; ++x)
            {
                const uint64_t t = m_records[y * VBGO_TT_WIDTH + x];
                uint8_t *px = &m_frameBufferRgba[(static_cast<size_t>(y) * kFbWidth + eyeOffset[eye] + x) * 4];
                uint8_t rgb[3] = {0, 0, 0};
                if (VBGO_TT_VALID(t))
                    TileDebugColor(VBGO_TT_HASH(t), VBGO_TT_PIXEL(t), rgb);
                px[0] = rgb[2]; // B,G,R,A
                px[1] = rgb[1];
                px[2] = rgb[0];
                px[3] = 0xFF;
            }
        }
    }
}

void Emulator::SetTileTracking(bool enabled)
{
    m_trackingWanted = enabled;
    if (!enabled && m_tileDebugView)
        SetTileDebugView(false);
    UpdateTileTracking();
}

void Emulator::UpdateTileTracking()
{
    const bool colors = m_shadePaletteIndex >= 0 && (!m_colorPack.Empty() || m_packRenderer.AutoColorsOn());
    const bool on = colors || m_trackingWanted || m_tileDebugView || m_collecting;
    if (on != vbgo_tiletrack_is_enabled())
        vbgo_tiletrack_set_enabled(on);
}

bool Emulator::IsTileTracking() const { return vbgo_tiletrack_is_enabled(); }

void Emulator::SetTileDebugView(bool enabled)
{
    m_tileDebugView = enabled;
    UpdateTileTracking();
    if (m_hasFrame && m_ui)
        UploadFrame(); // show/hide it right away, even while paused
}

std::string Emulator::ReloadColorPack()
{
    m_colorPack.Clear();
    if (!m_romLoaded)
        return "No ROM loaded";

    const std::string folder = "colorpacks/" + m_romBaseName;
    const std::vector<std::string> files = m_platform->ListRomsSubfolder(folder);
    TileColorPack::ImportStats stats;
    for (const std::string &name : files)
    {
        if (name.size() < 5 || name.compare(name.size() - 4, 4, ".png") != 0)
            continue;
        const std::string base = name.substr(0, name.size() - 4);
        if (std::find(files.begin(), files.end(), base + ".tiles") == files.end())
        {
            ++stats.rejected;
            stats.lastError = "\"" + name + "\" has no matching .tiles file";
            continue;
        }
        const std::vector<uint8_t> png = m_platform->ReadRomsFile(folder + "/" + name, false);
        const std::vector<uint8_t> sidecar = m_platform->ReadRomsFile(folder + "/" + base + ".tiles", false);
        int w = 0, h = 0, channels = 0;
        stbi_uc *pixels = png.empty() ? nullptr
                                      : stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &channels, 3);
        if (!m_colorPack.AddPainting(pixels, w, h, 3, sidecar, stats))
            stats.lastError = "\"" + name + "\": " + stats.lastError;
        if (pixels)
            stbi_image_free(pixels);
    }

    char summary[512];
    if (stats.paintings + stats.sheets > 0)
    {
        m_colorPack.FinishImport(stats);
        m_colorPack.SetRom(m_romCrc, m_romSize); // so the game finds it under any file name
        const std::vector<uint8_t> bytes = m_colorPack.Serialize();
        m_platform->WriteRomsFile(m_romBaseName + ".vbcp", false, bytes.data(), bytes.size());
        std::snprintf(summary, sizeof(summary),
                      "Color pack: imported %d painting(s) + %d tile sheet(s) -> %zu tiles (%zu tile pixels, %zu only "
                      "from tile sheets, %zu left uncolored (magenta); own colors: %zu per palette, %zu per layer, %zu "
                      "per map cell, %zu fills, %zu shared tiles per object (%zu objects); %zu painted differently in "
                      "different places, majority used; %zu stray shades merged)%s%s, saved %s.vbcp",
                      stats.paintings, stats.sheets, m_colorPack.TileCount(), stats.tilePixels, stats.fromSheets,
                      stats.erased, stats.palettePixels, stats.layerPixels, stats.cellPixels, stats.fillPixels,
                      stats.contextTiles, stats.contextGroups, stats.inconsistent, stats.mergedColors,
                      stats.rejected ? "; skipped: " : "",
                      stats.rejected ? stats.lastError.c_str() : "", m_romBaseName.c_str());
    }
    else if (std::vector<uint8_t> bytes = m_platform->ReadRomsFile(m_romBaseName + ".vbcp", false); m_colorPack.Deserialize(bytes))
    {
        // Made before packs recorded their ROM: record this one, so the game
        // finds it under any file name (and the Android build's index of
        // bundled packs lists it - see android/app/build.gradle).
        const bool stamped = !m_colorPack.RomCrc();
        if (stamped)
        {
            bytes = TileColorPack::WithRom(std::move(bytes), m_romCrc, m_romSize);
            m_platform->WriteRomsFile(m_romBaseName + ".vbcp", false, bytes.data(), bytes.size());
            m_colorPack.SetRom(m_romCrc, m_romSize);
        }
        std::snprintf(summary, sizeof(summary), "Color pack: loaded %s.vbcp (%zu tiles)%s", m_romBaseName.c_str(),
                      m_colorPack.TileCount(), stamped ? " - recorded its ROM (CRC) in it" : "");
    }
    // Built into the app (Android: the .vbcp files android/app/build.gradle
    // bundles - see colorpacks.dir there); one in the ROMs folder wins.
    else if (m_colorPack.Deserialize(m_platform->LoadAssetBytes("colorpacks/" + m_romBaseName + ".vbcp")))
        std::snprintf(summary, sizeof(summary), "Color pack: built-in %s.vbcp (%zu tiles)", m_romBaseName.c_str(),
                      m_colorPack.TileCount());
    // A pack made for this very ROM under another name (the ROM's file was
    // renamed, or comes from another set).
    else if (std::string from; FindPackForRom(from))
        std::snprintf(summary, sizeof(summary), "Color pack: %s (made for this ROM, CRC %08X; %zu tiles)", from.c_str(),
                      m_romCrc, m_colorPack.TileCount());
    else
        std::snprintf(summary, sizeof(summary), "Color pack: none for \"%s\" (CRC %08X)%s%s", m_romBaseName.c_str(), m_romCrc,
                      stats.rejected ? " - skipped: " : "", stats.rejected ? stats.lastError.c_str() : "");

    m_packRenderer.SetPack(&m_colorPack);
    UpdateTileTracking();
    UpdateFillTracking();
    // A gradient palette learns each game's brightness anew (and takes the
    // new pack's reference brightness).
    if (m_shadePaletteIndex >= 0 && GradientOfShadePalette(m_shadePaletteIndex) >= 0)
    {
        const int index = m_shadePaletteIndex;
        m_shadePaletteIndex = -1;
        SetShadePalette(index);
    }
    if (m_hasFrame && m_ui)
        UploadFrame();
    return summary;
}

bool Emulator::FindPackForRom(std::string &from)
{
    // Built in: android/app/build.gradle lists the bundled packs' ROMs in
    // colorpacks/index.txt ("<CRC-32 in hex> <ROM size> <pack file>" a line).
    const std::vector<uint8_t> index = m_platform->LoadAssetBytes("colorpacks/index.txt");
    const std::string text(index.begin(), index.end());
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        unsigned crc = 0, size = 0;
        int nameAt = 0;
        if (std::sscanf(line.c_str(), "%x %u %n", &crc, &size, &nameAt) < 2 || nameAt <= 0 || crc != m_romCrc ||
            (size && size != m_romSize))
            continue;
        std::string name = line.substr(static_cast<size_t>(nameAt));
        while (!name.empty() && (name.back() == '\r' || name.back() == ' '))
            name.pop_back();
        if (m_colorPack.Deserialize(m_platform->LoadAssetBytes("colorpacks/" + name)))
        {
            from = "built-in " + name;
            return true;
        }
    }
    // The ROMs folder's packs (where the platform can list it - not Android).
    for (const std::string &name : m_platform->ListRomsSubfolder(""))
    {
        if (name.size() < 6 || name.compare(name.size() - 5, 5, ".vbcp") != 0)
            continue;
        const std::vector<uint8_t> bytes = m_platform->ReadRomsFile(name, false);
        uint32_t crc = 0, size = 0;
        if (TileColorPack::RomOf(bytes, crc, size) && crc == m_romCrc && (!size || size == m_romSize) &&
            m_colorPack.Deserialize(bytes))
        {
            from = name;
            return true;
        }
    }
    m_colorPack.Clear();
    return false;
}

void Emulator::SetAuthoring(bool enabled)
{
    m_authoring = enabled;
    UpdateFillTracking();
}

void Emulator::UpdateFillTracking()
{
    // Captures need every fill the painter could paint; otherwise only the
    // map cells the pack has fills for are worth tagging.
    const std::vector<uint8_t> &cells = m_packRenderer.FillCells();
    vbgo_tiletrack_set_fill_cells(cells.empty() ? nullptr : cells.data());
    vbgo_tiletrack_set_fill_mode(m_authoring ? VBGO_TT_FILLS_ALL : cells.empty() ? VBGO_TT_FILLS_NONE : VBGO_TT_FILLS_PACK);
}

void Emulator::SetColorPackEnabled(bool enabled)
{
    m_colorPackEnabled = enabled;
    if (m_hasFrame && m_ui)
        UploadFrame();
}

std::string Emulator::NextCaptureName(const char *infix) const
{
    for (int n = 1; n < 1000; ++n)
    {
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), " %03d", n);
        const std::string base = m_romBaseName + infix + suffix;
        if (!m_platform->RomsFileExists("captures/" + base + ".tiles", false))
            return base;
    }
    return m_romBaseName + infix + " 999";
}

bool Emulator::WriteCapture(const std::string &base, const uint8_t *rgb, const uint64_t *tiles, uint32_t width,
                            uint32_t height, const uint16_t *chr, const uint32_t *cells,
                            const std::array<std::array<uint8_t, 3>, 4> *shown, uint8_t brightnessLevel,
                            const uint8_t *rightOwn)
{
    // 3x nearest-neighbor upscale, so single pixels are easy to hit with a brush.
    constexpr uint32_t kUp = 3;
    const uint32_t w = width * kUp, h = height * kUp;
    std::vector<uint8_t> up(static_cast<size_t>(w) * h * 3);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            std::memcpy(&up[(static_cast<size_t>(y) * w + x) * 3], &rgb[(static_cast<size_t>(y / kUp) * width + x / kUp) * 3], 3);
    std::vector<uint8_t> png;
    stbi_write_png_to_func([](void *ctx, void *data, int size)
                           {
                               auto *out = static_cast<std::vector<uint8_t> *>(ctx);
                               out->insert(out->end(), static_cast<uint8_t *>(data), static_cast<uint8_t *>(data) + size);
                           },
                           &png, static_cast<int>(w), static_cast<int>(h), 3, up.data(), static_cast<int>(w) * 3);

    // Sidecar: "VBGOTIL2", width, height, one 64-bit tile record per pixel
    // (see vbgo_tiletrack.h for the fields), one 32-bit map cell per pixel,
    // then every tile used: count, (hash, 8 rows of 2bpp pixels) each.
    std::vector<uint8_t> sidecar = {'V', 'B', 'G', 'O', 'T', 'I', 'L', '2'};
    AppendLe32(sidecar, width);
    AppendLe32(sidecar, height);
    const size_t count = static_cast<size_t>(width) * height;
    std::unordered_map<uint32_t, uint32_t> used; // hash -> character slot
    sidecar.reserve(sidecar.size() + count * 12);
    for (size_t i = 0; i < count; ++i)
    {
        for (int b = 0; b < 8; ++b)
            sidecar.push_back(static_cast<uint8_t>(tiles[i] >> (b * 8)));
        if (VBGO_TT_VALID(tiles[i]))
            used[VBGO_TT_HASH(tiles[i])] = VBGO_TT_CHAR(tiles[i]);
    }
    for (size_t i = 0; i < count; ++i)
        AppendLe32(sidecar, cells ? cells[i] : 0);
    std::vector<uint8_t> dictionary;
    uint32_t tileCount = 0;
    for (const auto &entry : used)
    {
        if (!chr)
            break;
        AppendLe32(dictionary, entry.first);
        for (int row = 0; row < 8; ++row)
        {
            dictionary.push_back(static_cast<uint8_t>(chr[entry.second * 8 + row]));
            dictionary.push_back(static_cast<uint8_t>(chr[entry.second * 8 + row] >> 8));
        }
        ++tileCount;
    }
    AppendLe32(sidecar, tileCount);
    sidecar.insert(sidecar.end(), dictionary.begin(), dictionary.end());
    // The colors this capture shows unpainted pixels in, so the importer can
    // tell what was left unpainted (and, for a screen, the game's brightness).
    TileColorPack::AppendSidecarPalette(sidecar, shown ? *shown : CapturePalette(), shown ? brightnessLevel : 255);
    // And exactly what each pixel showed: pixels left showing the pack's
    // colors (or anything else) don't count as painted.
    TileColorPack::AppendSidecarShown(sidecar, rgb, count);
    // A right-eye capture: where the right eye shows a picture of its own.
    if (rightOwn)
        TileColorPack::AppendSidecarRightPicture(sidecar, rightOwn, count);

    return !png.empty() && m_platform->WriteRomsFile("captures/" + base + ".png", false, png.data(), png.size()) &&
           m_platform->WriteRomsFile("captures/" + base + ".tiles", false, sidecar.data(), sidecar.size());
}

std::array<std::array<uint8_t, 3>, 4> Emulator::CapturePalette() const
{
    std::array<std::array<uint8_t, 3>, 4> palette{};
    for (int i = 0; i < 4; ++i)
    {
        if (m_shadePaletteIndex >= 0)
        {
            const ShadeRgb c = m_shadeColorizer.Palette()[i]; // what each shade shows at full brightness
            palette[i] = {static_cast<uint8_t>(c.r * 255.0f + 0.5f), static_cast<uint8_t>(c.g * 255.0f + 0.5f),
                          static_cast<uint8_t>(c.b * 255.0f + 0.5f)};
        }
        else
        {
            const uint8_t gray = static_cast<uint8_t>(i * 85);
            palette[i] = {gray, gray, gray};
        }
    }
    return palette;
}

std::string Emulator::CaptureTileReference(bool rightEye)
{
    if (!m_romLoaded || !m_hasFrame || !vbgo_tiletrack_is_enabled())
        return "";
    // The right eye: a right-eye painting, for what games draw for that eye
    // only (see ColorPackRenderer) - its own pixels are marked, so the
    // importer keeps what's painted there for that eye.
    const unsigned eye = rightEye ? 1 : 0;
    const uint32_t eyeX = rightEye ? m_lastFrameWidth - VBGO_TT_WIDTH : 0;
    const std::vector<uint8_t> &rightOwn = m_packRenderer.RightPictureOwn();
    if (rightEye && rightOwn.size() != VBGO_TT_EYE_PIXELS)
        return ""; // (nothing drawn for the right eye only)

    // That eye, in the active Multicolor palette (easier to tell objects
    // apart while painting - e.g. Ember), or the core's grayscale otherwise.
    // Only pixel positions matter when reading a painted copy back, so the
    // palette is purely a painting aid. What's on screen - including
    // color-pack colors, so a capture can be painted further from where the
    // pack left off - except the tile debug view's colors.
    std::vector<uint8_t> source(m_rawFrame.size());
    if (!m_tileDebugView)
        source = m_frameBufferRgba;
    else if (m_shadePaletteIndex >= 0)
        m_shadeColorizer.Colorize(m_rawFrame.data(), source.data(), m_rawFrame.size() / 4);
    else
        source = m_rawFrame;
    std::vector<uint8_t> rgb(static_cast<size_t>(VBGO_TT_EYE_PIXELS) * 3);
    for (int y = 0; y < VBGO_TT_HEIGHT; ++y)
        for (int x = 0; x < VBGO_TT_WIDTH; ++x)
        {
            const uint8_t *src = &source[(static_cast<size_t>(y) * kFbWidth + eyeX + x) * 4];
            uint8_t *dst = &rgb[(static_cast<size_t>(y) * VBGO_TT_WIDTH + x) * 3];
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
        }

    // Every pixel's tile record and map cell, fills (transparent background
    // pixels the painter can paint over) included - but only visible
    // pixels: the game also draws tiles in a shade it has switched off
    // (shown as background), and those must not take the painted background
    // color.
    vbgo_tt_eye_view view;
    std::vector<uint64_t> tiles(VBGO_TT_EYE_PIXELS);
    std::vector<uint32_t> cells(VBGO_TT_EYE_PIXELS);
    if (!vbgo_tiletrack_eye_view(eye, &view) || !vbgo_tiletrack_records(eye, tiles.data(), cells.data(), true))
        return "";
    for (int y = 0; y < VBGO_TT_HEIGHT; ++y)
        for (int x = 0; x < VBGO_TT_WIDTH; ++x)
        {
            const size_t i = static_cast<size_t>(y) * VBGO_TT_WIDTH + x;
            const uint8_t *raw = &m_rawFrame[(static_cast<size_t>(y) * kFbWidth + eyeX + x) * 4];
            if (VBGO_TT_PIXEL(tiles[i]) && (raw[0] | raw[1] | raw[2]) == 0)
                tiles[i] = 0, cells[i] = 0;
        }
    // The game's brightness right now (every pixel carries it - see
    // cmake/PatchBeetleVip.cmake; the most common value, in case it changed
    // mid-frame) and the colors unpainted pixels show at it.
    uint32_t levels[64] = {};
    for (int y = 0; y < VBGO_TT_HEIGHT; ++y)
        for (int x = 0; x < VBGO_TT_WIDTH; ++x)
            ++levels[m_rawFrame[(static_cast<size_t>(y) * kFbWidth + eyeX + x) * 4 + 3] >> 2];
    const uint8_t level = static_cast<uint8_t>(std::max_element(levels, levels + 64) - levels);
    std::array<std::array<uint8_t, 3>, 4> shown = CapturePalette();
    if (m_shadePaletteIndex >= 0)
    {
        const float fade = m_shadeColorizer.Fade(level);
        for (int i = 1; i < 4; ++i)
            for (int c = 0; c < 3; ++c)
                shown[i][c] = static_cast<uint8_t>(std::lround(shown[0][c] + (shown[i][c] - shown[0][c]) * fade));
    }
    else // grayscale: the core's own output for each shade
        for (int y = 0; y < VBGO_TT_HEIGHT; ++y)
            for (int x = 0; x < VBGO_TT_WIDTH; ++x)
            {
                const uint8_t *raw = &m_rawFrame[(static_cast<size_t>(y) * kFbWidth + eyeX + x) * 4];
                shown[raw[3] & 3] = {raw[2], raw[1], raw[0]};
            }
    const std::string base = NextCaptureName(rightEye ? " right" : "");
    if (!WriteCapture(base, rgb.data(), tiles.data(), VBGO_TT_WIDTH, VBGO_TT_HEIGHT, view.chr, cells.data(), &shown, level,
                      rightEye ? rightOwn.data() : nullptr))
        return "";
    std::fprintf(stderr, "[Emulator] Captured tile reference \"%s\"\n", base.c_str());
    return base;
}

std::string Emulator::CaptureTileSheet()
{
    const uint16_t *chr = vbgo_tiletrack_chr_ram();
    if (!m_romLoaded || !m_hasFrame || !vbgo_tiletrack_is_enabled() || !chr)
        return "";

    // All 2048 tiles, 32 per row, in memory order - pieces the game stores
    // together (a big sprite, a block) mostly stay together.
    constexpr uint32_t kColumns = 32, kWidth = kColumns * 8, kHeight = 2048 / kColumns * 8;
    const std::array<std::array<uint8_t, 3>, 4> palette = CapturePalette();
    std::vector<uint8_t> rgb(static_cast<size_t>(kWidth) * kHeight * 3);
    std::vector<uint64_t> tiles(static_cast<size_t>(kWidth) * kHeight, 0);
    for (uint32_t slot = 0; slot < 2048; ++slot)
    {
        const uint16_t *rows = &chr[slot * 8];
        const uint32_t hash = vbgo_tiletrack_hash_rows(rows);
        const TileColorPack::Tile *tile = m_colorPack.Find(hash);
        for (uint32_t sy = 0; sy < 8; ++sy)
            for (uint32_t sx = 0; sx < 8; ++sx)
            {
                const size_t i = static_cast<size_t>((slot / kColumns) * 8 + sy) * kWidth + (slot % kColumns) * 8 + sx;
                const unsigned value = (rows[sy] >> (sx * 2)) & 3;
                const unsigned index = sy * 8 + sx;
                const uint8_t *color = palette[value].data(); // the raw value as a shade - the usual palette setup
                if (value && tile && (tile->mask >> index & 1))
                    color = tile->rgb[index];
                std::memcpy(&rgb[i * 3], color, 3);
                if (value) // value 0 is transparent - not part of the tile's picture
                    tiles[i] = hash | static_cast<uint64_t>(sx) << 32 | static_cast<uint64_t>(sy) << 35 |
                               static_cast<uint64_t>(value) << 41 | static_cast<uint64_t>(slot) << 48 | 1ull << 63;
            }
    }
    const std::string base = NextCaptureName(" tiles");
    if (!WriteCapture(base, rgb.data(), tiles.data(), kWidth, kHeight, chr))
        return "";
    std::fprintf(stderr, "[Emulator] Saved tile sheet \"%s\"\n", base.c_str());
    return base;
}

std::string Emulator::SetCollectingUncolored(bool enabled)
{
    if (enabled == m_collecting)
        return m_collecting ? "Collecting uncolored objects" : "Not collecting";
    m_collecting = enabled;
    UpdateTileTracking();
    if (enabled)
        return "Collecting uncolored objects - play, then switch it off to save paint sheets";
    if (!m_romLoaded)
        return "Stopped collecting";
    const std::vector<UncoloredCollector::Sheet> sheets = m_collector.TakeSheets(CapturePalette()[0]);
    std::string first, last;
    int written = 0;
    for (const UncoloredCollector::Sheet &sheet : sheets)
    {
        const std::string base = NextCaptureName(" todo");
        if (!WriteCapture(base, sheet.rgb.data(), sheet.tiles.data()))
            break;
        (written ? last : first) = base;
        ++written;
    }
    char summary[512];
    if (written == 0)
        std::snprintf(summary, sizeof(summary), "Stopped collecting - nothing new without colors%s",
                      sheets.empty() ? "" : " (couldn't write captures/)");
    else
        std::snprintf(summary, sizeof(summary), "Saved %d paint sheet(s) to roms/captures/: \"%s\"%s%s", written,
                      first.c_str(), written > 1 ? " to \"" : "", written > 1 ? (last + "\"").c_str() : "");
    return summary;
}

void Emulator::DrawScreen(UiRenderer &ui, float x, float y, float w, float h, Eye eye, const XrColor4f &tint,
                          int patternIndex) const
{
    if (!m_screenTexture.IsValid())
        return;
    // UV-crop the fixed-size streaming texture down to just the current
    // frame's valid region, stretched to fill the destination rect - same
    // "stretch to fill" behaviour the old static-image stub had. For a
    // single eye, crop that region's left/right half on top of the same
    // valid-region crop (the combined frame is left-eye-then-right-eye).
    const float fullU1 = static_cast<float>(m_lastFrameWidth) / static_cast<float>(kFbWidth);
    const float v1 = static_cast<float>(m_lastFrameHeight) / static_cast<float>(kFbHeight);

    float u0 = 0.0f;
    float u1 = fullU1;
    if (eye == Eye::Left)
        u1 = fullU1 * 0.5f;
    else if (eye == Eye::Right)
        u0 = fullU1 * 0.5f;

    if (patternIndex >= 0 && patternIndex < kScreenPatternCount)
        ui.DrawImageRegionPattern(m_screenTexture, x, y, w, h, u0, 0.0f, u1, v1, kScreenPatterns[patternIndex]);
    else
        ui.DrawImageRegion(m_screenTexture, x, y, w, h, u0, 0.0f, u1, v1, 1.0f, tint);
}

std::string Emulator::StateFileName(int uiSlot, const char *ext) const
{
    std::string name = m_romBaseName + "." + ext;
    if (uiSlot != 0)
        name += std::to_string(uiSlot);
    return name;
}

void Emulator::CaptureScreenshotGrayscale(std::vector<uint8_t> &outGray) const
{
    outGray.assign(static_cast<size_t>(kPreviewWidth) * kPreviewHeight, 0);
    // Sampled from the raw core frame, not the uploaded (possibly shade-
    // palette colorized) one, so .stateimg always stores true VB luminance
    // regardless of the palette active when the state was saved.
    if (!m_hasFrame)
        return;

    // Left-eye crop of the current side-by-side frame (native VB
    // resolution) - same convention DrawScreen's Eye::Left uses - mapped
    // proportionally onto kPreviewWidth x kPreviewHeight rather than
    // assuming an exact match (m_lastFrameWidth/Height aren't guaranteed to
    // be exactly the assumed side-by-side geometry - see Emulator.h); in
    // practice this is a 1:1 copy since both are 384x224. Luminance = max
    // channel - the core's output is already a true grayscale signal
    // (R==G==B) before any palette tint, so any channel would do; max is
    // just the safest choice if that ever isn't quite true.
    const uint32_t srcEyeWidth = m_lastFrameWidth / 2;
    const uint32_t srcHeight = m_lastFrameHeight;
    if (srcEyeWidth == 0 || srcHeight == 0)
        return;

    for (uint32_t y = 0; y < kPreviewHeight; ++y)
    {
        const uint32_t srcY = y * srcHeight / kPreviewHeight;
        for (uint32_t x = 0; x < kPreviewWidth; ++x)
        {
            const uint32_t srcX = x * srcEyeWidth / kPreviewWidth;
            const size_t srcIndex = (static_cast<size_t>(srcY) * kFbWidth + srcX) * 4;
            const uint8_t r = m_rawFrame[srcIndex + 0];
            const uint8_t g = m_rawFrame[srcIndex + 1];
            const uint8_t b = m_rawFrame[srcIndex + 2];
            // Core output is gamma-encoded (sRGB-like). Linearize before
            // storing so .stateimg holds true luminance - byte-compatible
            // with old save files (pre-fed1a44) which were also linear.
            const uint8_t srgb = std::max({r, g, b});
            outGray[static_cast<size_t>(y) * kPreviewWidth + x] =
                static_cast<uint8_t>(std::round(255.0f * std::pow(srgb / 255.0f, 2.2f)));
        }
    }
}

bool Emulator::SaveState(int uiSlot)
{
    if (!m_romLoaded)
        return false;

    const size_t size = retro_serialize_size();
    if (size == 0)
        return false;

    std::vector<uint8_t> data(size);
    if (!retro_serialize(data.data(), size))
        return false;

    std::vector<uint8_t> preview;
    CaptureScreenshotGrayscale(preview);

    if (!m_platform->WriteRomsFile(StateFileName(uiSlot, "state"), true, data.data(), data.size()))
        return false;
    m_platform->WriteRomsFile(StateFileName(uiSlot, "stateimg"), true, preview.data(), preview.size());

    return true;
}

bool Emulator::LoadState(int uiSlot)
{
    if (!m_romLoaded)
        return false;

    std::vector<uint8_t> data = m_platform->ReadRomsFile(StateFileName(uiSlot, "state"), true);
    // Refuse rather than feed the core a stale/mismatched-size buffer -
    // FrontendGo's own LoadState skipped this check.
    if (data.size() != retro_serialize_size())
        return false;

    return retro_unserialize(data.data(), data.size());
}

bool Emulator::SaveStateExists(int uiSlot) const
{
    return m_platform->RomsFileExists(StateFileName(uiSlot, "state"), true);
}

bool Emulator::LoadStatePreview(int uiSlot, std::vector<uint8_t> &outRgba) const
{
    constexpr size_t kGraySize = static_cast<size_t>(kPreviewWidth) * kPreviewHeight;

    std::vector<uint8_t> gray = m_platform->ReadRomsFile(StateFileName(uiSlot, "stateimg"), true);
    if (gray.size() != kGraySize)
        return false;

    // Expand to RGBA for the caller. .stateimg stores linear luminance;
    // the streaming texture is UNORM and the display chain expects
    // gamma-encoded values, so apply sRGB gamma encode here. This also
    // fixes old (pre-fed1a44) save files which were already linear.
    outRgba.resize(kGraySize * 4);
    for (size_t i = 0; i < kGraySize; ++i)
    {
        const uint8_t lum = static_cast<uint8_t>(
            std::round(255.0f * std::pow(gray[i] / 255.0f, 1.0f / 2.2f)));
        outRgba[i * 4 + 0] = lum;
        outRgba[i * 4 + 1] = lum;
        outRgba[i * 4 + 2] = lum;
        outRgba[i * 4 + 3] = 0xFF;
    }
    return true;
}

void Emulator::SaveRam()
{
    if (!m_romLoaded)
        return;

    const size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    void *data = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (size == 0 || !data)
        return; // this ROM has no battery-backed SRAM

    m_platform->WriteRomsFile(m_romBaseName + ".srm", false, data, size);
}

void Emulator::LoadRam()
{
    if (!m_romLoaded)
        return;

    const size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    void *data = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (size == 0 || !data)
        return;

    const std::vector<uint8_t> bytes = m_platform->ReadRomsFile(m_romBaseName + ".srm", false);
    // Ignore rather than feed the core a stale/mismatched-size buffer.
    if (bytes.size() != size)
        return;
    std::memcpy(data, bytes.data(), size);
}

void Emulator::Shutdown()
{
    if (m_romLoaded && m_collecting) // don't lose what was collected
        std::fprintf(stderr, "[Emulator] %s\n", SetCollectingUncolored(false).c_str());
    if (m_romLoaded)
        SaveRam();
    g_audioOutput = nullptr;
    m_audioOutput.Shutdown();
}
