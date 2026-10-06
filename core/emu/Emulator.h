#pragma once

#include "emu/AudioOutput.h"
#include "emu/ColorPackRenderer.h"
#include "emu/ShadeColorizer.h"
#include "emu/TileColorPack.h"
#include "emu/UncoloredCollector.h"
#include "gfx/UiRenderer.h"
#include "io/Platform.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Bit positions for Emulator::SetGameplayInput's bitmask - one bit per VB
// button. Values match libretro's RETRO_DEVICE_ID_JOYPAD_* ids exactly (see
// beetle-vb-libretro/libretro.cpp's retro_load_game input descriptor table,
// which is what actually defines which VB button each id means - the VB has
// two D-pads, not one, hence Left*/Right* instead of a single Up/Down/Left/
// Right) so Emulator.cpp's input_state_cb can use id directly as a bit
// index. Kept here (not libretro.h) so frontends (Main.cpp/OpenXrApp) don't
// need to include the core's headers just to feed it input.
namespace VBButtonBit
{
    constexpr uint32_t B = 0;
    constexpr uint32_t Select = 2;
    constexpr uint32_t Start = 3;
    constexpr uint32_t LeftUp = 4;
    constexpr uint32_t LeftDown = 5;
    constexpr uint32_t LeftLeft = 6;
    constexpr uint32_t LeftRight = 7;
    constexpr uint32_t A = 8;
    constexpr uint32_t L = 10;
    constexpr uint32_t R = 11;
    constexpr uint32_t RightUp = 12;
    constexpr uint32_t RightLeft = 13;
    constexpr uint32_t RightDown = 14;
    constexpr uint32_t RightRight = 15;
} // namespace VBButtonBit

// Thin adapter over the real Virtual Boy core (libretro/beetle-vb-libretro,
// vendored as a git submodule at third_party/beetle-vb-libretro - see that
// folder's COPYING for its GPL-2.0 license), statically linked and called
// directly through its standard libretro.h API (retro_load_game/retro_run/
// etc.), not dlopen'd. See RunFrame/the retro_* callback implementations in
// Emulator.cpp for how the core's video/audio output reaches the screen/
// speakers.
//
// The core always renders in "side-by-side" 3D mode (forced via this
// class's environment callback) - both VB eyes packed into one wide
// combined frame - which DrawScreen exposes as a single texture; splitting
// that into two per-eye OpenXR quad layers (via subImage.imageRect crops)
// is OpenXrApp's job, not this class's.
class Emulator
{
public:
    // Screens are shown at this fixed integer upscale (pixel-perfect,
    // nearest-neighbor - see UiRenderer::LoadImage) everywhere the emulator
    // screen is displayed, so PC2D and the headset builds look consistent.
    static constexpr int kScale = 3;

    // Native VB refresh rate (retro_get_system_av_info's timing.fps) -
    // RunFrame accumulates real deltaSeconds against this to decide how many
    // times to call retro_run() per app frame. Public so the display refresh
    // rate can be chosen relative to it (OpenXrApp::RequestMaxDisplayRefreshRate).
    static constexpr float kCoreFps = 50.27f;

    // Fixed side-by-side-mode geometry (384-wide base VB screen * 2 eyes,
    // 224 tall) - used to size the screen swapchain/texture up front, before
    // any ROM is loaded (GetScreenWidth/Height must be valid immediately
    // after Initialize(), well before the user picks a ROM from the menu).
    // This is an assumption about what libretro.cpp's side-by-side geometry
    // actually reports - the core's own base geometry constants
    // (MEDNAFEN_CORE_GEOMETRY_BASE_W/H = 384/224) are fixed regardless of
    // ROM content, so this should hold for any ROM, but hasn't been verified
    // against the real DisplayRect the core reports at runtime yet. If it's
    // off, the visible symptom is a stretched/letterboxed aspect ratio, not
    // a crash - adjust these two constants once confirmed.
    static constexpr uint32_t kSideBySideWidth = 384 * 2;
    static constexpr uint32_t kSideBySideHeight = 224;

    void Initialize(UiRenderer &ui, Platform &platform);

    // Reads romPath's bytes (via Platform::ReadRomFile) and hands them to
    // the core via retro_load_game. Safe to call more than once (unloads
    // whatever ROM was previously loaded first). Returns false if the file
    // couldn't be read or the core rejected it.
    //
    // displayName (RomEntry::name) is used for save-state/SRAM naming -
    // romPath itself may be a content:// URI (Android), not a usable
    // filename stem.
    bool LoadRom(const std::string &romPath, const std::string &displayName = "");

    // Cold-resets the currently loaded game through the libretro core.
    // Returns false when no ROM is loaded.
    bool ResetGame();

    // True once Initialize() has set up the streaming screen texture -
    // *not* tied to whether a ROM is loaded, so the screen quad layer/
    // texture exists (showing black) from app start, and DrawScreen doesn't
    // need special-casing for the "no ROM loaded yet" state.
    bool HasScreen() const { return m_screenTexture.IsValid(); }
    uint32_t GetScreenWidth() const { return kSideBySideWidth; }
    uint32_t GetScreenHeight() const { return kSideBySideHeight; }

    // Fixed-timestep accumulator against the VB's native ~50.27Hz refresh -
    // call once per app frame with the same deltaSeconds already computed
    // for AppMenu::Update. Runs retro_run() zero or more times to catch up,
    // then re-uploads the latest video frame to the streaming texture if at
    // least one retro_run() happened. A no-op before any ROM is loaded.
    void RunFrame(float deltaSeconds);

    // Sets the VB gamepad state RunFrame's next retro_run() call(s) will
    // read - bits per VBButtonBit, 1 = held. Call once per app frame (before
    // RunFrame) with whatever the current frontend's input maps to; pass 0
    // while the menu is open so gameplay input doesn't leak through it.
    void SetGameplayInput(uint32_t joypadBitmask);

    // Which half of the combined side-by-side frame DrawScreen shows -
    // Both is the real stereo image (what OpenXrApp uses, splitting it into
    // per-eye quad layers itself); Left/Right are for flat/mono display
    // (pc2d's debug window) where there's no second eye to show the other
    // half to. TODO: expose Left vs Right as a user-facing setting instead
    // of pc2d hardcoding Left - filed as a known follow-up, not implemented
    // yet.
    enum class Eye
    {
        Both,
        Left,
        Right
    };

    // Stretches the current screen into the given destination rect (in
    // target pixels) - callers typically size that rect to
    // GetScreenWidth/Height() * kScale for a pixel-perfect look (Both only -
    // Left/Right are half that width, see Eye). Draws whatever the last
    // RunFrame produced (all-black before any ROM loads). tint multiplies
    // the drawn pixels (white = no-op) - the VB color palette/custom RGB
    // tint (AppSettings::colorR/G/B) is applied this way, since the core
    // itself has no palette concept and always outputs pre-colored frames
    // (per-shade palettes are the exception - see SetShadePalette - and
    // callers pass AppSettings::ScreenTint()/ScreenPattern(), which go
    // neutral while one is active).
    // patternIndex (0-5, see kScreenPatterns in Settings.h) draws through
    // screen_pattern.frag's multi-hue gradient instead, ignoring tint
    // entirely - the flat single-color multiply path above is otherwise
    // completely unchanged. -1 (default) keeps today's tint-only behavior.
    void DrawScreen(UiRenderer &ui, float x, float y, float w, float h, Eye eye = Eye::Both,
                    const XrColor4f &tint = XrColor4f{1.0f, 1.0f, 1.0f, 1.0f}, int patternIndex = -1) const;

    // Per-shade colorization (AppSettings::selectedShadePalette): -1 = off,
    // the core's grayscale goes to the screen texture as-is; 0+ = index into
    // kShadePalettes, every pixel recolored by which VB shade it is (see
    // ShadeColorizer); kAutoColors = the Auto mode, colored by what drew
    // each pixel (see AutoColors.h - turns tile tracking on). Under a color
    // pack either way. Unlike the tint/pattern, this is baked into the
    // uploaded frame rather than applied at draw time, so on a change the
    // last frame is immediately re-colored and re-uploaded - the screen
    // updates live while the menu has emulation paused. Cheap when
    // unchanged; call once per app frame with the current setting.
    void SetShadePalette(int paletteIndex);

    // --- Experimental tile colorization groundwork (see vbgo_tiletrack.h) ---
    //
    // Tracking makes the core record, per displayed pixel, which 8x8 tile and
    // which pixel of it was drawn there. It costs the core time on every drawn
    // pixel, so it only runs while something uses it: the Auto mode, a
    // Multicolor palette with a color pack, the debug view, the collector -
    // or this (captures need it on).
    void SetTileTracking(bool enabled);
    bool IsTileTracking() const;

    // Replaces the screen with one random color per distinct tile (shaded
    // by the tile's raw pixel value; black where no tile was drawn) - a
    // visual check that tiles are identified correctly. Enables tracking.
    void SetTileDebugView(bool enabled);
    bool IsTileDebugView() const { return m_tileDebugView; }

    // Saves the current frame's left eye as a paint-ready reference into the
    // ROMs folder's captures/ subfolder: "<rom> NNN.png" (3x nearest-neighbor
    // upscale, in the active Multicolor palette or else the core's
    // grayscale) plus "<rom> NNN.tiles" (which tile
    // and in-tile pixel every screen pixel is, for reading a painted copy
    // back later). Needs tracking on for at least one emulated frame first.
    // Returns the base name used ("<rom> NNN"), or "" on failure.
    // rightEye: the right eye instead ("<rom> right NNN"), with the pixels
    // where it shows a picture of its own marked (see ColorPackRenderer) -
    // paint those to set what that eye shows there; "" if nothing is drawn
    // for the right eye only.
    std::string CaptureTileReference(bool rightEye = false);

    // Saves everything in the game's tile memory right now - loaded for the
    // current area, on screen or not - as a paint-ready tile sheet:
    // captures/"<rom> tiles NNN.png" (32 tiles wide, like a tile viewer, 3x;
    // tiles the pack already colors in its colors, the rest in the active
    // Multicolor palette) plus its .tiles file. A painted sheet in the pack
    // folder fills in whatever the screen paintings don't color. Returns the
    // base name used, or "" on failure.
    std::string CaptureTileSheet();

    // --- Experimental per-game color packs (see TileColorPack.h) ---------
    //
    // (Re)builds the loaded ROM's pack: if the ROMs folder has
    // colorpacks/<rom>/ with paintings (each "<name>.png" beside the
    // "<name>.tiles" its reference was captured with - desktop only, needs
    // folder listing), imports them and saves the result as <rom>.vbcp next
    // to the ROM; otherwise loads an existing <rom>.vbcp (any platform), or
    // else one built into the app (asset "colorpacks/<rom>.vbcp" - Android
    // bundles them at build time, see android/app/build.gradle).
    // Called by LoadRom; call again to pick up edited paintings. Returns a
    // one-line summary.
    std::string ReloadColorPack();
    bool HasColorPack() const { return !m_colorPack.Empty(); }
    // Painted tiles are drawn while a Multicolor palette is active (the
    // palette then colors everything the pack doesn't cover); this switches
    // the pack off/on for comparison. On by default.
    void SetColorPackEnabled(bool enabled);
    bool IsColorPackEnabled() const { return m_colorPackEnabled; }

    // Desktop authoring tools (F10 captures, F7) need to know every
    // transparent background pixel a painter could paint over ("fills" - see
    // vbgo_tiletrack.h); otherwise only those the pack has colors for are
    // tracked, which is cheaper. Off by default.
    void SetAuthoring(bool enabled);

    // While on, every frame's not-yet-colored objects (whatever the pack
    // doesn't cover) are collected (see UncoloredCollector.h); turning it off
    // writes them to captures/ as paint-ready sheets, "<rom> todo NNN.png" +
    // ".tiles" - painted copies import like any other painting. Enables
    // tracking. Returns a one-line summary.
    std::string SetCollectingUncolored(bool enabled);
    bool IsCollectingUncolored() const { return m_collecting; }

    // UI slots are 0-9 (10 total, matching FrontendGo's saveStates[10]) -
    // slot 0 is unsuffixed on disk, same as FrontendGo's slot 0 (see
    // StateFilePath). Raw retro_serialize dump, binary-compatible with
    // FrontendGo's .state[N].
    bool SaveState(int uiSlot);
    bool LoadState(int uiSlot);
    bool SaveStateExists(int uiSlot) const;

    // Native VB resolution - matches FrontendGo's .stateimg exactly.
    static constexpr uint32_t kPreviewWidth = 384;
    static constexpr uint32_t kPreviewHeight = 224;

    // Reads the preview from the last SaveState(uiSlot) call, expanded to
    // RGBA (R=G=B=lum, A=255) - false if that slot has never been saved.
    // On-disk format (.stateimg[N]) is raw linear-luminance grayscale,
    // byte-compatible with FrontendGo and pre-fed1a44 saves; gamma encode
    // is applied at load time (LoadStatePreview), not baked in.
    bool LoadStatePreview(int uiSlot, std::vector<uint8_t> &outRgba) const;

    // Flushes cart SRAM for the currently-loaded ROM, if any, and stops
    // audio playback - call once on app exit (LoadRom already flushes SRAM
    // on every ROM switch).
    void Shutdown();

private:
    // <m_romBaseName>.<ext><suffix>; suffix empty for uiSlot==0, else uiSlot.
    // Passed to Platform::WriteRomsFile/ReadRomsFile/RomsFileExists.
    std::string StateFileName(int uiSlot, const char *ext) const;

    void CaptureScreenshotGrayscale(std::vector<uint8_t> &outGray) const;

    // Converts m_rawFrame into m_frameBufferRgba (colorized if a shade
    // palette is active, otherwise a straight copy with opaque alpha) and
    // uploads it to the screen texture.
    void UploadFrame();
    void PaintTileDebugView();
    // Tells the tracker which fills to tag (see SetAuthoring).
    void UpdateFillTracking();
    // Turns the tracker on while anything uses it (see SetTileTracking).
    void UpdateTileTracking();
    // Next free "<rom><infix> NNN" in captures/.
    std::string NextCaptureName(const char *infix) const;
    // Writes captures/<base>.png (3x upscale of rgb, width x height RGB) and
    // captures/<base>.tiles (the tile records and map cells, plus every
    // tile's rows from chr - the character memory the records refer to - and
    // the colors unpainted pixels show: shown + the game's brightness level
    // for a screen, else the palette at full brightness).
    bool WriteCapture(const std::string &base, const uint8_t *rgb, const uint64_t *tiles,
                      uint32_t width = 384, uint32_t height = 224, const uint16_t *chr = nullptr,
                      const uint32_t *cells = nullptr, const std::array<std::array<uint8_t, 3>, 4> *shown = nullptr,
                      uint8_t brightnessLevel = 255, const uint8_t *rightOwn = nullptr);
    // The 4 colors uncolored pixels are shown in (Multicolor palette, else gray).
    std::array<std::array<uint8_t, 3>, 4> CapturePalette() const;

    // Cart battery-save, matches FrontendGo's <romDir>/<stem>.srm. Must run
    // before retro_unload_game() - the SRAM pointer isn't valid after.
    void SaveRam();
    void LoadRam();

    UiRenderer *m_ui = nullptr;
    Platform *m_platform = nullptr;
    UiImageHandle m_screenTexture;
    bool m_coreInitialized = false;
    bool m_romLoaded = false;

    // Set by LoadRom - the currently-loaded ROM's display name, used to
    // build save-state/SRAM file names. Empty before any ROM loads, when
    // SaveRam/SaveState no-op.
    std::string m_romBaseName;
    uint32_t m_romCrc = 0, m_romSize = 0; // the loaded ROM's CRC-32 and size (color packs are matched by them too)
    bool FindPackForRom(std::string &from); // a pack made for this ROM (by its CRC), built in or in the ROMs folder

    float m_frameAccumulator = 0.0f; // real time not yet consumed by retro_run() - see kCoreFps

    // Updated by the video_cb callback each retro_run() call - the portion
    // of the fixed-size streaming texture that's actually valid for the
    // current frame (used to UV-crop in DrawScreen). Defaults to the
    // side-by-side geometry assumption above until the first real frame.
    uint32_t m_lastFrameWidth = kSideBySideWidth;
    uint32_t m_lastFrameHeight = kSideBySideHeight;

    // The core's XRGB8888 output has an unused byte in the alpha position
    // (not a real alpha channel - typically 0), but ui_image.frag multiplies
    // its output alpha by whatever the sampled texture's alpha channel is.
    // Every other UiRenderer image source has real alpha (stb_image forces
    // opaque alpha for alpha-less PNGs, icons are authored with real alpha),
    // so this is the first data source where that byte is meaningless - copy
    // each frame through this buffer forcing alpha to 0xFF rather than
    // uploading the core's buffer directly, or the screen renders fully
    // transparent (black, since nothing else is behind it).
    std::vector<uint8_t> m_frameBufferRgba;

    // Untouched copy of the core's last frame - gray RGB plus the shade
    // index tag in each pixel's top byte (cmake/PatchBeetleVip.cmake). Kept
    // so a shade palette change can re-color the frame on screen without
    // running the core (UploadFrame), and so save-state previews stay true
    // VB luminance whatever palette is active (CaptureScreenshotGrayscale).
    // Same size as m_frameBufferRgba; m_hasFrame is false until the first
    // frame lands in it.
    std::vector<uint8_t> m_rawFrame;
    bool m_hasFrame = false;

    int m_shadePaletteIndex = -1; // see SetShadePalette
    ShadeColorizer m_shadeColorizer;
    bool m_tileDebugView = false; // see SetTileDebugView
    TileColorPack m_colorPack;    // see ReloadColorPack
    ColorPackRenderer m_packRenderer;
    bool m_trackingWanted = false; // see SetTileTracking
    // Time per emulated frame (see RunFrame): the core's, and coloring both
    // eyes' (UploadFrame) - summed / worst since the last log line.
    double m_emulationMs = 0.0, m_emulationMaxMs = 0.0, m_coloringMs = 0.0, m_coloringMaxMs = 0.0;
    int m_timedFrames = 0, m_coloringFrames = 0, m_catchUps = 0;
    bool m_colorPackEnabled = true;
    bool m_authoring = false;     // see SetAuthoring
    std::vector<uint64_t> m_records; // one eye's tile records, for the debug view / F7 collector
    UncoloredCollector m_collector; // see SetCollectingUncolored
    bool m_collecting = false;
    ShadeRgb m_shadeBackground{0.0f, 0.0f, 0.0f}; // active Multicolor palette's background - painted tiles fade toward it

    // Plays whatever the audio_sample_batch callback forwards to it - see
    // Emulator.cpp's RetroAudioSampleBatch. Owned here (not a global) since
    // it needs real construction/destruction (the ring buffer, the
    // platform device), unlike the plain data the other retro_* callbacks
    // touch; a global pointer to it is still needed for those free
    // functions to reach it (see Emulator.cpp's g_audioOutput).
    AudioOutput m_audioOutput;
};
