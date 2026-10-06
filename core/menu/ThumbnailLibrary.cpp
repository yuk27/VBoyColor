#include "menu/ThumbnailLibrary.h"

#include "emu/ThumbnailRecipes.h"
#include "emu/TileColorPack.h"
#include "io/Settings.h"

#include <stb_image.h>       // (implementation in VulkanRenderer.cpp)
#include <stb_image_write.h> // (implementation in Emulator.cpp)

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace
{
    // Where the thumbnails are kept: one file, so the ROMs folder isn't
    // searched once per game (slow through Android's folder access).
    constexpr const char *kArchiveName = "library.thumbs";
    constexpr const char *kRecentName = "recent.txt";
    constexpr char kArchiveMagic[8] = {'V', 'B', 'C', 'T', 'H', 'U', 'M', '1'};
    // Part of every thumbnail's key: bump it when the thumbnails themselves
    // should change (how they're made or colored), to make them all again.
    constexpr int kThumbnailFormat = 1;

    // At most this many saved thumbnails decoded and uploaded per frame.
    constexpr int kDecodesPerFrame = 2;
    // Unsaved thumbnails are written at the latest after this many frames.
    constexpr int kSaveEveryFrames = 1800;

    // Box art: libretro's thumbnails for the Virtual Boy, by No-Intro name
    // (with &*/:`<>?\| written as _), kept under "box/<name>".
    constexpr const char *kBoxArtUrl =
        "https://raw.githubusercontent.com/libretro-thumbnails/Nintendo_-_Virtual_Boy/master/Named_Boxarts/";
    constexpr const char *kBoxPrefix = "box/";

    std::string BoxArtUrl(const std::string &name)
    {
        static constexpr char kHex[] = "0123456789ABCDEF";
        std::string url = kBoxArtUrl;
        for (unsigned char c : name + ".png")
        {
            if (std::strchr("&*/:`<>?\\|\"", c))
                c = '_';
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || std::strchr("-_.~", c))
                url += static_cast<char>(c);
            else
            {
                url += '%';
                url += kHex[c >> 4];
                url += kHex[c & 15];
            }
        }
        return url;
    }

    // The box art on a card: fitted whole in the middle, over a blurred,
    // darkened copy of itself filling the rest.
    std::vector<uint8_t> ComposeBoxArt(const uint8_t *rgb, int w, int h, uint32_t outW, uint32_t outH)
    {
        auto sample = [&](float x, float y, int c)
        {
            x = std::clamp(x, 0.0f, static_cast<float>(w - 1));
            y = std::clamp(y, 0.0f, static_cast<float>(h - 1));
            const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
            const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
            const float fx = x - x0, fy = y - y0;
            auto at = [&](int px, int py) { return static_cast<float>(rgb[(static_cast<size_t>(py) * w + px) * 3 + c]); };
            return (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
        };
        // The background: the art covering the card, small (so blurring is
        // cheap), box-blurred twice, then stretched back up.
        constexpr uint32_t kSmallW = 48, kSmallH = 28;
        const float cover = std::max(static_cast<float>(outW) / w, static_cast<float>(outH) / h);
        std::vector<float> small(kSmallW * kSmallH * 3), blurred(small.size());
        for (uint32_t y = 0; y < kSmallH; ++y)
            for (uint32_t x = 0; x < kSmallW; ++x)
                for (int c = 0; c < 3; ++c)
                {
                    const float sx = ((x + 0.5f) * outW / kSmallW - outW / 2.0f) / cover + w / 2.0f;
                    const float sy = ((y + 0.5f) * outH / kSmallH - outH / 2.0f) / cover + h / 2.0f;
                    small[(y * kSmallW + x) * 3 + c] = sample(sx, sy, c);
                }
        for (int pass = 0; pass < 2; ++pass)
        {
            constexpr int kRadius = 3;
            for (uint32_t y = 0; y < kSmallH; ++y)
                for (uint32_t x = 0; x < kSmallW; ++x)
                    for (int c = 0; c < 3; ++c)
                    {
                        float sum = 0;
                        int count = 0;
                        for (int dy = -kRadius; dy <= kRadius; ++dy)
                            for (int dx = -kRadius; dx <= kRadius; ++dx)
                            {
                                const int px = static_cast<int>(x) + dx, py = static_cast<int>(y) + dy;
                                if (px < 0 || py < 0 || px >= static_cast<int>(kSmallW) || py >= static_cast<int>(kSmallH))
                                    continue;
                                sum += small[(py * kSmallW + px) * 3 + c];
                                ++count;
                            }
                        blurred[(y * kSmallW + x) * 3 + c] = sum / count;
                    }
            small.swap(blurred);
        }
        std::vector<uint8_t> out(static_cast<size_t>(outW) * outH * 3);
        const float fit = std::min(static_cast<float>(outW) / w, static_cast<float>(outH) / h);
        const float fitW = w * fit, fitH = h * fit;
        const float left = (outW - fitW) / 2.0f, top = (outH - fitH) / 2.0f;
        for (uint32_t y = 0; y < outH; ++y)
            for (uint32_t x = 0; x < outW; ++x)
            {
                uint8_t *dst = &out[(static_cast<size_t>(y) * outW + x) * 3];
                const bool inside = x + 0.5f >= left && x + 0.5f < left + fitW && y + 0.5f >= top && y + 0.5f < top + fitH;
                for (int c = 0; c < 3; ++c)
                {
                    float v;
                    if (inside)
                        v = sample((x + 0.5f - left) / fit - 0.5f, (y + 0.5f - top) / fit - 0.5f, c);
                    else
                    {
                        const float sx = std::clamp((x + 0.5f) * kSmallW / outW - 0.5f, 0.0f, kSmallW - 1.0f);
                        const float sy = std::clamp((y + 0.5f) * kSmallH / outH - 0.5f, 0.0f, kSmallH - 1.0f);
                        const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy);
                        const int x1 = std::min<int>(x0 + 1, kSmallW - 1), y1 = std::min<int>(y0 + 1, kSmallH - 1);
                        const float fx = sx - x0, fy = sy - y0;
                        auto at = [&](int px, int py) { return small[(py * kSmallW + px) * 3 + c]; };
                        v = ((at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy) * 0.45f;
                    }
                    dst[c] = static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
                }
            }
        return out;
    }

    std::vector<uint8_t> EncodePng(const std::vector<uint8_t> &rgb, uint32_t w, uint32_t h)
    {
        std::vector<uint8_t> png;
        stbi_write_png_to_func([](void *ctx, void *data, int size)
                               {
                                   auto *out = static_cast<std::vector<uint8_t> *>(ctx);
                                   out->insert(out->end(), static_cast<uint8_t *>(data), static_cast<uint8_t *>(data) + size);
                               },
                               &png, static_cast<int>(w), static_cast<int>(h), 3, rgb.data(), static_cast<int>(w) * 3);
        return png;
    }

    void SplitName(const std::string &name, std::string &title, std::string &details)
    {
        const size_t at = name.find(" (");
        title = at == std::string::npos ? name : name.substr(0, at);
        details = at == std::string::npos ? std::string() : name.substr(at + 1);
    }

    void AppendLe(std::vector<uint8_t> &out, uint32_t value, int bytes)
    {
        for (int i = 0; i < bytes; ++i)
            out.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }

    bool ReadLe(const std::vector<uint8_t> &in, size_t &at, int bytes, uint32_t &value)
    {
        if (at + bytes > in.size())
            return false;
        value = 0;
        for (int i = 0; i < bytes; ++i)
            value |= static_cast<uint32_t>(in[at + i]) << (i * 8);
        at += bytes;
        return true;
    }

    bool ReadString(const std::vector<uint8_t> &in, size_t &at, std::string &out)
    {
        uint32_t length = 0;
        if (!ReadLe(in, at, 2, length) || at + length > in.size())
            return false;
        out.assign(reinterpret_cast<const char *>(in.data() + at), length);
        at += length;
        return true;
    }

    void AppendString(std::vector<uint8_t> &out, const std::string &text)
    {
        const size_t length = std::min<size_t>(text.size(), 0xffff);
        AppendLe(out, static_cast<uint32_t>(length), 2);
        out.insert(out.end(), text.begin(), text.begin() + static_cast<std::ptrdiff_t>(length));
    }
} // namespace

void ThumbnailLibrary::Init(UiRenderer &ui, Platform &platform, Emulator &emulator, const AppSettings &settings)
{
    m_ui = &ui;
    m_platform = &platform;
    m_emulator = &emulator;
    m_settings = &settings;
    m_boxArt = settings.downloadBoxArt;
    m_sortRecent = settings.librarySortRecent;
    {
        const std::vector<uint8_t> bytes = platform.ReadRomsFile(kRecentName, true);
        const std::string text(bytes.begin(), bytes.end());
        for (size_t start = 0; start < text.size();)
        {
            size_t end = text.find('\n', start);
            if (end == std::string::npos)
                end = text.size();
            std::string line = text.substr(start, end - start);
            start = end + 1;
            while (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty())
                m_recent.push_back(line);
        }
    }
    LoadArchive();
    Rescan();
}

void ThumbnailLibrary::Sort()
{
    auto recentRank = [this](const std::string &name)
    {
        const auto it = std::find(m_recent.begin(), m_recent.end(), name);
        return it == m_recent.end() ? static_cast<int>(m_recent.size()) : static_cast<int>(it - m_recent.begin());
    };
    std::stable_sort(m_games.begin(), m_games.end(), [&](const Game &a, const Game &b)
                     {
                         if (m_sortRecent)
                         {
                             const int ra = recentRank(a.rom.name), rb = recentRank(b.rom.name);
                             if (ra != rb)
                                 return ra < rb;
                         }
                         return a.order < b.order;
                     });
    ++m_version;
}

void ThumbnailLibrary::SetSortRecent(bool recent)
{
    if (m_sortRecent == recent)
        return;
    m_sortRecent = recent;
    Sort();
}

void ThumbnailLibrary::NotePlayed(const std::string &name)
{
    m_recent.erase(std::remove(m_recent.begin(), m_recent.end(), name), m_recent.end());
    m_recent.insert(m_recent.begin(), name);
    if (m_recent.size() > 200)
        m_recent.resize(200);
    std::string text;
    for (const std::string &line : m_recent)
        text += line + "\n";
    m_platform->WriteRomsFile(kRecentName, true, text.data(), text.size());
    if (m_sortRecent)
        Sort();
}

ThumbnailLibrary::Game *ThumbnailLibrary::Find(const std::string &name)
{
    for (Game &game : m_games)
        if (game.rom.name == name)
            return &game;
    return nullptr;
}

int ThumbnailLibrary::IndexOf(const std::string &name) const
{
    for (size_t i = 0; i < m_games.size(); ++i)
        if (m_games[i].rom.name == name)
            return static_cast<int>(i);
    return -1;
}

void ThumbnailLibrary::Rescan()
{
    std::vector<Game> previous = std::move(m_games);
    m_games.clear();
    m_decodeQueue.clear();
    m_checkQueue.clear();
    if (!m_platform->HasRomsFolder())
        return;
    for (const RomEntry &rom : m_platform->ScanRoms())
    {
        Game game;
        game.rom = rom;
        game.order = static_cast<int>(m_games.size());
        SplitName(rom.name, game.title, game.details);
        // Keep what's already showing (and its texture) for games still here.
        for (Game &old : previous)
            if (old.rom.name == rom.name)
            {
                game.hasPack = old.hasPack;
                game.ready = old.ready;
                game.failed = old.failed;
                game.showingBox = old.showingBox;
                game.texture = old.texture;
                old.texture = UiImageHandle{};
                break;
            }
        m_games.push_back(std::move(game));
    }
    for (const Game &game : m_games)
    {
        m_decodeQueue.push_back(game.rom.name);
        m_checkQueue.push_back(game.rom.name);
    }
    Sort();
    // Textures of games no longer in the folder are kept for new ones (the
    // UI's descriptor pool can't free them).
    for (Game &old : previous)
        if (old.texture.IsValid())
            m_spareTextures.push_back(old.texture);
}

void ThumbnailLibrary::Recheck(const std::string &name)
{
    if (name.empty() || name == m_making)
        return;
    m_checkQueue.erase(std::remove(m_checkQueue.begin(), m_checkQueue.end(), name), m_checkQueue.end());
    m_checkQueue.push_front(name);
}

void ThumbnailLibrary::RebuildAll()
{
    // (box art stays - it doesn't change)
    for (auto it = m_saved.begin(); it != m_saved.end();)
        it = it->first.compare(0, std::strlen(kBoxPrefix), kBoxPrefix) == 0 ? std::next(it) : m_saved.erase(it);
    m_decodeQueue.clear();
    m_checkQueue.clear();
    for (const Game &game : m_games)
        m_checkQueue.push_back(game.rom.name);
    m_archiveDirty = true;
}

void ThumbnailLibrary::SetBoxArt(bool boxArt)
{
    if (m_boxArt == boxArt)
        return;
    m_boxArt = boxArt;
    m_noBoxArt.clear();
    for (const Game &game : m_games) // (each shows what it should now)
        m_decodeQueue.push_back(game.rom.name);
}

const ThumbnailLibrary::Saved *ThumbnailLibrary::ShownFor(const std::string &name, bool &box) const
{
    if (m_boxArt)
    {
        const auto art = m_saved.find(kBoxPrefix + name);
        if (art != m_saved.end())
        {
            box = true;
            return &art->second;
        }
    }
    box = false;
    const auto title = m_saved.find(name);
    return title == m_saved.end() ? nullptr : &title->second;
}

void ThumbnailLibrary::DownloadNext()
{
    for (const Game &game : m_games)
    {
        const std::string &name = game.rom.name;
        if (m_saved.count(kBoxPrefix + name) ||
            std::find(m_noBoxArt.begin(), m_noBoxArt.end(), name) != m_noBoxArt.end())
            continue;
        if (!m_platform->StartDownload(BoxArtUrl(name)))
        {
            m_downloadsUnavailable = true; // (this platform can't - title screens it is)
            return;
        }
        m_downloading = name;
        return;
    }
}

void ThumbnailLibrary::FinishDownload(const std::vector<uint8_t> &bytes)
{
    const std::string name = m_downloading;
    m_downloading.clear();
    int w = 0, h = 0, channels = 0;
    stbi_uc *pixels = bytes.empty() ? nullptr
                                    : stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 3);
    if (!pixels || w < 8 || h < 8)
    {
        if (pixels)
            stbi_image_free(pixels);
        m_noBoxArt.push_back(name);
        return;
    }
    const std::vector<uint8_t> card = ComposeBoxArt(pixels, w, h, kWidth, kHeight);
    stbi_image_free(pixels);
    Saved saved;
    saved.key = "box1";
    saved.png = EncodePng(card, kWidth, kHeight);
    m_saved[kBoxPrefix + name] = std::move(saved);
    m_archiveDirty = true;
    if (Game *game = Find(name); game && m_boxArt)
        Upload(*game, card, true);
}

void ThumbnailLibrary::Update(bool allowed)
{
    m_emulator->SetThumbnailsAllowed(allowed);

    // A game started (from the library, a dropped file, ...): it's the most recent.
    if (m_emulator->RomName() != m_lastPlayed)
    {
        m_lastPlayed = m_emulator->RomName();
        if (!m_lastPlayed.empty())
            NotePlayed(m_lastPlayed);
    }

    // A finished one: show it, file it.
    std::string name;
    std::vector<uint8_t> rgb;
    while (m_emulator->TakeThumbnail(name, rgb))
    {
        Game *game = Find(name);
        const bool wasMaking = name == m_making;
        if (wasMaking)
            m_making.clear();
        if (rgb.size() != static_cast<size_t>(kWidth) * kHeight * 3)
        {
            if (game)
                game->failed = !game->ready;
            continue;
        }
        Saved saved;
        saved.key = wasMaking ? m_makingKey : std::string();
        saved.hasPack = wasMaking && m_makingHasPack;
        saved.png = EncodePng(rgb, kWidth, kHeight);
        m_saved[name] = std::move(saved);
        m_archiveDirty = true;
        if (game)
        {
            game->hasPack = wasMaking && m_makingHasPack;
            bool box = false;
            ShownFor(name, box);
            if (!box) // (its box art stays)
                Upload(*game, rgb, false);
        }
    }

    // Saved ones, a couple a frame.
    for (int decoded = 0; decoded < kDecodesPerFrame && !m_decodeQueue.empty();)
    {
        const std::string next = m_decodeQueue.front();
        m_decodeQueue.pop_front();
        Game *game = Find(next);
        bool box = false;
        const Saved *saved = ShownFor(next, box);
        if (!game || !saved || (game->ready && game->showingBox == box))
            continue;
        if (const auto title = m_saved.find(next); title != m_saved.end())
            game->hasPack = title->second.hasPack;
        int w = 0, h = 0, channels = 0;
        stbi_uc *pixels = stbi_load_from_memory(saved->png.data(), static_cast<int>(saved->png.size()), &w, &h, &channels, 3);
        if (pixels && w == static_cast<int>(kWidth) && h == static_cast<int>(kHeight))
            Upload(*game, std::vector<uint8_t>(pixels, pixels + static_cast<size_t>(w) * h * 3), box);
        if (pixels)
            stbi_image_free(pixels);
        ++decoded;
    }

    // Box art, one download at a time (while the menu's open).
    if (!m_downloading.empty())
    {
        std::vector<uint8_t> bytes;
        const int state = m_platform->PollDownload(bytes);
        if (state != 0)
            FinishDownload(state == 1 ? bytes : std::vector<uint8_t>());
    }
    else if (allowed && m_boxArt && !m_downloadsUnavailable)
        DownloadNext();

    if (allowed && m_making.empty() && !m_checkQueue.empty() && m_emulator->WantsThumbnailInput())
        CheckNext();

    // Kept: once everything's done, or now and then while making them.
    ++m_framesSinceSave;
    if (m_archiveDirty && ((Remaining() == 0 && m_downloading.empty()) || m_framesSinceSave >= kSaveEveryFrames))
        SaveArchive();
}

void ThumbnailLibrary::CheckNext()
{
    const std::string name = m_checkQueue.front();
    m_checkQueue.pop_front();
    Game *game = Find(name);
    if (!game)
        return;

    Emulator::ThumbnailInput input;
    input.name = name;
    input.rom = m_platform->ReadRomFile(game->rom.fullPath);
    if (input.rom.empty())
    {
        game->failed = !game->ready;
        return;
    }
    const uint32_t crc = TileColorPack::Crc32(input.rom.data(), input.rom.size());
    const uint32_t size = static_cast<uint32_t>(input.rom.size());
    const ThumbnailRecipe &recipe = ThumbnailRecipeFor(crc);
    input.pack = Emulator::FindPackBytes(*m_platform, name, crc, size);

    // The colors it starts with: its own (saved for it in Settings), its
    // suggestion, or Auto - what LibraryPage applies when it loads.
    AppSettings colors = *m_settings;
    colors.ApplyGameColors(*m_platform, name, crc);
    input.shadePalette = colors.EffectiveShadePalette();
    input.pattern = colors.ScreenPattern();
    const XrColor4f tint = colors.ScreenTint();
    input.tint[0] = tint.r;
    input.tint[1] = tint.g;
    input.tint[2] = tint.b;

    char key[160];
    std::snprintf(key, sizeof(key), "f%d r%08x %u %u c%d %d %.3f %.3f %.3f p%zu %08x", kThumbnailFormat, crc,
                  recipe.lastPress, recipe.frame, input.shadePalette, input.pattern, input.tint[0], input.tint[1],
                  input.tint[2], input.pack.size(),
                  input.pack.empty() ? 0u : TileColorPack::Crc32(input.pack.data(), input.pack.size()));

    const auto saved = m_saved.find(name);
    if (saved != m_saved.end() && saved->second.key == key)
        return; // up to date
    m_making = name;
    m_makingKey = key;
    m_makingHasPack = !input.pack.empty();
    m_emulator->GiveThumbnailInput(std::move(input));
}

void ThumbnailLibrary::Upload(Game &game, const std::vector<uint8_t> &rgb, bool box)
{
    if (!game.texture.IsValid() && !m_spareTextures.empty())
    {
        game.texture = m_spareTextures.back();
        m_spareTextures.pop_back();
    }
    if (!game.texture.IsValid())
        game.texture = m_ui->CreateStreamingImage(kWidth, kHeight, VK_FORMAT_B8G8R8A8_SRGB);
    std::vector<uint8_t> bgra(static_cast<size_t>(kWidth) * kHeight * 4);
    for (size_t i = 0, n = static_cast<size_t>(kWidth) * kHeight; i < n; ++i)
    {
        bgra[i * 4 + 0] = rgb[i * 3 + 2];
        bgra[i * 4 + 1] = rgb[i * 3 + 1];
        bgra[i * 4 + 2] = rgb[i * 3 + 0];
        bgra[i * 4 + 3] = 255;
    }
    m_ui->UpdateStreamingImage(game.texture, bgra.data(), bgra.size());
    game.ready = true;
    game.showingBox = box;
    game.failed = false;
}

void ThumbnailLibrary::LoadArchive()
{
    const std::vector<uint8_t> bytes = m_platform->ReadRomsFile(kArchiveName, true);
    if (bytes.size() < 12 || std::memcmp(bytes.data(), kArchiveMagic, 8) != 0)
        return;
    size_t at = 8;
    uint32_t count = 0;
    if (!ReadLe(bytes, at, 4, count))
        return;
    for (uint32_t i = 0; i < count; ++i)
    {
        std::string name;
        Saved saved;
        uint32_t flags = 0, pngSize = 0;
        if (!ReadString(bytes, at, name) || !ReadString(bytes, at, saved.key) || !ReadLe(bytes, at, 1, flags) ||
            !ReadLe(bytes, at, 4, pngSize) || at + pngSize > bytes.size())
            return;
        saved.hasPack = (flags & 1) != 0;
        saved.png.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(at + pngSize));
        at += pngSize;
        m_saved[name] = std::move(saved);
    }
}

void ThumbnailLibrary::SaveArchive()
{
    std::vector<uint8_t> bytes(kArchiveMagic, kArchiveMagic + 8);
    AppendLe(bytes, static_cast<uint32_t>(m_saved.size()), 4);
    for (const auto &[name, saved] : m_saved)
    {
        AppendString(bytes, name);
        AppendString(bytes, saved.key);
        AppendLe(bytes, saved.hasPack ? 1u : 0u, 1);
        AppendLe(bytes, static_cast<uint32_t>(saved.png.size()), 4);
        bytes.insert(bytes.end(), saved.png.begin(), saved.png.end());
    }
    m_platform->WriteRomsFile(kArchiveName, true, bytes.data(), bytes.size());
    m_archiveDirty = false;
    m_framesSinceSave = 0;
}
