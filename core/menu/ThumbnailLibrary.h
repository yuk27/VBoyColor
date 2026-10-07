#pragma once

#include "emu/Emulator.h"
#include "gfx/UiRenderer.h"
#include "io/Platform.h"

#include <cstdint>
#include <deque>
#include <future>
#include <string>
#include <unordered_map>
#include <vector>

struct AppSettings;

// The library's games and their thumbnails - each game's title screen in
// the colors it starts with, made on this device by running the game (see
// Emulator::GiveThumbnailInput and ThumbnailRecipes.h), so nothing of any
// game ships with the app.
//
// Made once and kept in the ROMs folder (States/library.thumbs). Each one
// is filed with what it was made from - its recipe, its colors, its color
// pack - and made again when any of that changes (a new pack, the game's
// colors changed in Settings, a new version of the app's recipes).
//
// By default (Settings > Download box art) the cards show each game's box
// art instead, downloaded once from libretro's thumbnail collection
// (github.com/libretro-thumbnails) and kept in the same file - and only a
// game without one (or with no internet) gets its title screen made, which
// takes a lot more work than a download.
//
// Downloads go on while a game is played (one at a time, each made into a
// card on a thread of its own); everything else waits for the menu: making
// title screens, showing new cards, writing the file.
//
// Everything else here runs on the render thread (Android's file access
// needs it); the emulation itself runs on the emulator's own thread.
class ThumbnailLibrary
{
public:
    static constexpr uint32_t kWidth = Emulator::kPreviewWidth;
    static constexpr uint32_t kHeight = Emulator::kPreviewHeight;

    struct Game
    {
        RomEntry rom;
        std::string title;   // the name up to its first " (" - "Mario Clash"
        std::string details; // the rest - "(Japan, USA)"
        bool hasPack = false; // made with a color pack (known once its thumbnail is)
        bool ready = false;   // `texture` shows its thumbnail (or box art)
        bool showingBox = false; // ...its box art
        int order = 0;           // its place A-Z (the ROMs folder's order)
        bool failed = false;  // it couldn't be made
        UiImageHandle texture;
    };

    void Init(UiRenderer &ui, Platform &platform, Emulator &emulator, const AppSettings &settings);
    // Reads the ROMs folder again. Saved thumbnails show over the next
    // frames; missing or outdated ones get made while the menu is open.
    void Rescan();
    // Once per app frame, outside rendering (it uploads textures). allowed:
    // the menu is open - thumbnails are only made (and shown, and saved)
    // then; box art downloads either way.
    void Update(bool allowed);

    const std::vector<Game> &Games() const { return m_games; }
    int IndexOf(const std::string &name) const;
    // Checks a game's thumbnail again - e.g. its colors changed - and makes
    // it again if it's outdated.
    void Recheck(const std::string &name);
    // Makes every thumbnail again.
    void RebuildAll();
    // The games A-Z, or the last played first (then the rest A-Z) - the
    // order Games() lists them in.
    void SetSortRecent(bool recent);
    bool IsSortRecent() const { return m_sortRecent; }
    // Bumped whenever Games() changes (rescan, order) - for anything that
    // keeps per-game data by index.
    int Version() const { return m_version; }
    // Box art instead of title screens (downloaded as needed).
    void SetBoxArt(bool boxArt);
    // Box art is being downloaded right now.
    bool Downloading() const { return !m_downloading.empty() || m_card.valid(); }
    // A thumbnail is being made right now.
    bool Making() const { return !m_making.empty(); }
    // Games still to check or make (0: all done) - not counting those
    // waiting for their box art.
    int Remaining() const;

private:
    struct Saved
    {
        std::string key; // what it was made from (see Check)
        bool hasPack = false;
        std::vector<uint8_t> png;
    };
    void LoadArchive();
    void SaveArchive();
    // Checks the next game waiting; gives the emulator its input when its
    // thumbnail needs making.
    void CheckNext();
    void Upload(Game &game, const std::vector<uint8_t> &rgb, bool box);
    Game *Find(const std::string &name);
    // What a game's card should show: its box art (box mode, and it has
    // one), else its title screen - nullptr if neither is here yet.
    const Saved *ShownFor(const std::string &name, bool &box) const;
    // Box art: the next game's download, and a finished one (made into a
    // card off the render thread - see m_card).
    void DownloadNext();
    void FinishCard();
    // A game's card should show its box art: box art's on, and it has (or
    // may yet get) one. Waiting: ...and it hasn't come yet - its title
    // screen isn't made unless it turns out there's none.
    bool WantsBoxArt(const std::string &name) const;
    bool WaitingForBoxArt(const std::string &name) const;
    // Box art still downloading or to download.
    bool BoxArtToCome() const;

    UiRenderer *m_ui = nullptr;
    Platform *m_platform = nullptr;
    Emulator *m_emulator = nullptr;
    const AppSettings *m_settings = nullptr;

    std::vector<Game> m_games;
    std::vector<UiImageHandle> m_spareTextures; // of games no longer in the folder
    std::unordered_map<std::string, Saved> m_saved; // by RomEntry::name
    std::deque<std::string> m_decodeQueue;          // saved thumbnails still to show
    std::deque<std::string> m_checkQueue;           // games still to check
    std::string m_making, m_makingKey;              // the one being made
    bool m_makingHasPack = false;
    bool m_archiveDirty = false;
    int m_framesSinceSave = 0;

    // Last played first (States/recent.txt, a name per line) - noticed in
    // Update whenever the emulator's game changes.
    void Sort();
    void NotePlayed(const std::string &name);
    std::vector<std::string> m_recent;
    std::string m_lastPlayed;
    bool m_sortRecent = false;
    int m_version = 0;

    bool m_boxArt = false;
    std::string m_downloading;            // the game whose box art is downloading
    std::vector<std::string> m_noBoxArt;  // games with none to get (this session)
    bool m_downloadsUnavailable = false;  // (the platform can't download)
    // A downloaded picture being made into a card (decoded, fitted, encoded
    // as PNG) on a thread of its own, so a game being played never stutters.
    struct Card
    {
        std::string name;
        std::vector<uint8_t> png; // empty: it wasn't a picture
    };
    std::future<Card> m_card;
};
