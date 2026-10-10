#include "desktop/DesktopPlatform.h"

#if defined(_WIN32)
// Pulls in <windows.h> - without this, its min/max macros mangle every
// std::min/std::max call in this file (see AudioOutput.cpp's identical fix
// for the same issue with miniaudio's WASAPI backend).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

namespace
{
    std::string ToLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        return s;
    }

#if defined(_DEBUG)
    // This repo's checked-in sample-ROM / writable-data folder
    // (<repo>/sd/roms), for zero-setup local testing. Derived from this
    // file's own path (three parents up from
    // <repo>/platform/desktop/DesktopPlatform.cpp) so it follows the
    // checkout instead of a hardcoded absolute path.
    std::string DebugSdRomsDir()
    {
        const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
        return (repoRoot / "sd" / "roms").string();
    }
#endif

    std::string DefaultGamesFolder()
    {
#if defined(_DEBUG)
        return DebugSdRomsDir(); // repo's checked-in sample ROMs
#else
        return "roms"; // next to the exe (the working directory - see the constructor)
#endif
    }

    // UTF-8 <-> paths (Windows' narrow strings aren't UTF-8).
    std::filesystem::path U8(const std::string &s)
    {
#if defined(__cpp_char8_t)
        return std::filesystem::path(reinterpret_cast<const char8_t *>(s.c_str()));
#else
        return std::filesystem::u8path(s);
#endif
    }
    std::string ToU8(const std::filesystem::path &p)
    {
        const auto u = p.u8string();
        return std::string(u.begin(), u.end());
    }

    constexpr const char *kFoldersFile = "folders.txt";
    constexpr const char *kFolderKeys[kDataKindCount] = {"games", "saves", "states", "settings"};
} // namespace

DesktopPlatform::DesktopPlatform(const char *settingsFileName) : m_settingsFileName(settingsFileName)
{
#if defined(_WIN32)
    wchar_t exe[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(sizeof(exe) / sizeof(exe[0])));
    if (length > 0 && length < sizeof(exe) / sizeof(exe[0]))
    {
        std::error_code ec;
        std::filesystem::current_path(std::filesystem::path(exe).parent_path(), ec);
    }
#elif defined(__linux__)
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec)
        std::filesystem::current_path(exe.parent_path(), ec);
#endif
    std::error_code cwdEc;
    m_appFolder = ToU8(std::filesystem::current_path(cwdEc));
    LoadFolders();
}

std::vector<RomEntry> DesktopPlatform::ScanRoms()
{
    std::vector<RomEntry> roms;

    const std::filesystem::path dir = U8(FolderOf(DataKind::Games));
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec)
        return roms;

    for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
    {
        if (ec)
            break;
        if (!entry.is_regular_file())
            continue;

        const std::filesystem::path &path = entry.path();
        if (ToLower(path.extension().string()) != ".vb")
            continue;

        roms.push_back({ToU8(path.stem()), ToU8(path)});
    }

    std::sort(roms.begin(), roms.end(),
              [](const RomEntry &a, const RomEntry &b)
              { return ToLower(a.name) < ToLower(b.name); });

    return roms;
}

std::vector<uint8_t> DesktopPlatform::ReadRomFile(const std::string &path)
{
    // (ScanRoms' paths are UTF-8; one named on the command line or dropped
    // on the window may come in the system's own encoding)
    std::ifstream in(U8(path), std::ios::binary);
    if (!in)
        in.open(path, std::ios::binary);
    if (!in)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string DesktopPlatform::FolderOf(DataKind kind) const
{
    const std::string &chosen = m_folders[static_cast<int>(kind)];
    if (!chosen.empty())
        return chosen;
    const std::string &games = m_folders[0].empty() ? DefaultGamesFolder() : m_folders[0];
    const char *sub = DefaultSubfolder(kind);
    return *sub ? games + "/" + sub : games;
}

std::string DesktopPlatform::PathFor(const std::string &fileName, bool inStatesDir) const
{
    return FolderOf(DataKindOf(fileName, inStatesDir)) + "/" + fileName;
}

bool DesktopPlatform::WriteRomsFile(const std::string &fileName, bool inStatesDir, const void *data, size_t size)
{
    const std::filesystem::path path = U8(PathFor(fileName, inStatesDir));
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

std::vector<uint8_t> DesktopPlatform::ReadRomsFile(const std::string &fileName, bool inStatesDir)
{
    std::ifstream in(U8(PathFor(fileName, inStatesDir)), std::ios::binary);
    if (!in)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool DesktopPlatform::RomsFileExists(const std::string &fileName, bool inStatesDir) const
{
    std::error_code ec;
    return std::filesystem::exists(U8(PathFor(fileName, inStatesDir)), ec) && !ec;
}

std::vector<std::string> DesktopPlatform::ListRomsSubfolder(const std::string &subfolder) const
{
    std::vector<std::string> names;
    std::error_code ec;
    const std::filesystem::path dir = U8(FolderOf(DataKind::Games)) / U8(subfolder);
    if (!std::filesystem::is_directory(dir, ec) || ec)
        return names;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
    {
        if (ec)
            break;
        if (entry.is_regular_file(ec))
            names.push_back(ToU8(entry.path().filename()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

// ---------------------------------------------------------------------------
// Settings > Folders

void DesktopPlatform::LoadFolders()
{
    std::ifstream in(U8(m_appFolder) / kFoldersFile);
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        for (int i = 0; i < kDataKindCount; ++i)
            if (key == kFolderKeys[i])
                m_folders[i] = value;
    }
}

bool DesktopPlatform::SaveFolders() const
{
    std::string text = "# VBoy Color's folders (Settings > Folders) - empty: the default place\n";
    for (int i = 0; i < kDataKindCount; ++i)
        text += std::string(kFolderKeys[i]) + "=" + m_folders[i] + "\n";
    std::ofstream out(U8(m_appFolder) / kFoldersFile, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

std::string DesktopPlatform::DataFolderLabel(DataKind kind) const
{
    if (kind == DataKind::Games)
        return m_folders[0].empty() ? "roms" : m_folders[0];
    return m_folders[static_cast<int>(kind)];
}

std::string DesktopPlatform::DataFolderPath(DataKind kind) const
{
    std::error_code ec;
    std::filesystem::path path = U8(FolderOf(kind));
    if (path.is_relative())
        path = U8(m_appFolder) / path;
    const std::filesystem::path normal = std::filesystem::weakly_canonical(path, ec);
    return ToU8(ec ? path.lexically_normal() : normal);
}

std::vector<std::pair<std::string, std::string>> DesktopPlatform::FolderPlaces() const
{
    std::vector<std::pair<std::string, std::string>> places;
#if defined(_WIN32)
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
        if (drives & (1u << i))
        {
            const std::string root = std::string(1, static_cast<char>('A' + i)) + ":\\";
            places.push_back({"Drive " + root.substr(0, 2), root});
        }
    if (const wchar_t *home = _wgetenv(L"USERPROFILE"))
        places.push_back({"Your user folder", ToU8(std::filesystem::path(home))});
#else
    places.push_back({"The whole computer (/)", "/"});
    if (const char *home = std::getenv("HOME"))
        places.push_back({"Your home folder", home});
#endif
    places.push_back({"VBoy Color's folder", m_appFolder});
    return places;
}

std::vector<std::string> DesktopPlatform::ListFolders(const std::string &path) const
{
    std::vector<std::string> names;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(U8(path), std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec))
    {
        std::error_code typeEc;
        if (!it->is_directory(typeEc) || typeEc)
            continue;
        const std::string name = ToU8(it->path().filename());
        if (name.empty() || name[0] == '.' || name[0] == '$') // (hidden, and Windows' $Recycle.Bin and such)
            continue;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) { return ToLower(a) < ToLower(b); });
    return names;
}

namespace
{
    // Copies from's files of a kind (and only those - States and Settings
    // share the games folder's "States" by default) to to: those to doesn't
    // have, and those newer than to's. Returns how many, -1 if to can't be
    // written.
    int CopyKind(const std::filesystem::path &from, const std::filesystem::path &to, DataKind kind)
    {
        std::error_code ec;
        std::filesystem::create_directories(to, ec);
        if (!std::filesystem::is_directory(to, ec))
            return -1;
        {
            // (can it be written?)
            const std::filesystem::path probe = to / ".vboycolor-write-test";
            std::ofstream out(probe, std::ios::binary);
            if (!out)
                return -1;
            out.close();
            std::filesystem::remove(probe, ec);
        }
        if (std::filesystem::equivalent(from, to, ec) && !ec)
            return 0;
        const bool inStatesDir = kind == DataKind::States || kind == DataKind::Settings;
        int copied = 0;
        ec.clear();
        for (std::filesystem::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec))
        {
            std::error_code fileEc;
            if (!it->is_regular_file(fileEc))
                continue;
            const std::string name = ToU8(it->path().filename());
            if (DataKindOf(name, inStatesDir) != kind)
                continue;
            const std::filesystem::path target = to / it->path().filename();
            if (std::filesystem::exists(target, fileEc))
            {
                // Both have it: the newer one wins - the one it replaces is
                // kept beside it as "<name>.bak".
                const auto source = std::filesystem::last_write_time(it->path(), fileEc);
                const auto existing = std::filesystem::last_write_time(target, fileEc);
                if (fileEc || !(source > existing))
                    continue;
                std::filesystem::path backup = target;
                backup += ".bak";
                std::filesystem::remove(backup, fileEc);
                std::filesystem::rename(target, backup, fileEc);
                if (fileEc)
                    continue;
            }
            if (std::filesystem::copy_file(it->path(), target, fileEc))
            {
                // (with the original's time, so the copy isn't "newer" than it)
                std::error_code timeEc;
                std::filesystem::last_write_time(target, std::filesystem::last_write_time(it->path(), timeEc), timeEc);
                ++copied;
            }
        }
        return copied;
    }

    std::string Copied(int count, DataKind kind)
    {
        if (count <= 0)
            return std::string(DataKindName(kind)) + " folder changed";
        return std::string(DataKindName(kind)) + ": copied " + std::to_string(count) + (count == 1 ? " file" : " files") +
               " - the originals stay";
    }
} // namespace

std::string DesktopPlatform::SetDataFolder(DataKind kind, const std::string &path)
{
    const int k = static_cast<int>(kind);
    if (m_folders[k] == path)
        return "";
    auto absolute = [this](const std::string &folder)
    {
        std::filesystem::path p = U8(folder);
        return p.is_relative() ? U8(m_appFolder) / p : p;
    };

    // Where each kind lives now - and after the change.
    std::string before[kDataKindCount];
    for (int i = 0; i < kDataKindCount; ++i)
        before[i] = FolderOf(static_cast<DataKind>(i));
    const std::string old = m_folders[k];
    m_folders[k] = path;
    std::string after[kDataKindCount];
    for (int i = 0; i < kDataKindCount; ++i)
        after[i] = FolderOf(static_cast<DataKind>(i));

    if (kind == DataKind::Games)
    {
        std::error_code ec;
        std::filesystem::create_directories(absolute(after[0]), ec); // (back to the default "roms")
        if (!std::filesystem::is_directory(absolute(after[0]), ec))
        {
            m_folders[k] = old;
            return "That folder doesn't exist - nothing changed";
        }
    }

    // The kind's files go along - and when the games folder moves, so do
    // those of the kinds that live in it by default (not the ROMs: the new
    // games folder is where the player keeps them).
    int copied = 0;
    for (int i = 1; i < kDataKindCount; ++i)
    {
        if (i != k && kind != DataKind::Games)
            continue;
        if (before[i] == after[i])
            continue;
        const int n = CopyKind(absolute(before[i]), absolute(after[i]), static_cast<DataKind>(i));
        if (n < 0)
        {
            m_folders[k] = old;
            return "Can't write to that folder - nothing changed";
        }
        copied += n;
    }
    if (!SaveFolders())
    {
        m_folders[k] = old;
        return "Couldn't save the choice next to VBoy Color - nothing changed";
    }
    return Copied(copied, kind);
}

#if defined(_WIN32)
std::vector<uint8_t> DesktopPlatform::LoadAssetBytes(const std::string &name)
{
    // Embedded into the .exe as RCDATA resources (see
    // platform/desktop/DesktopAssets.rc.in / CMakeLists.txt) rather than
    // read from loose files next to it.
    struct Entry
    {
        const char *name;
        LPCWSTR resourceId;
    };
    static const Entry kEmbedded[] = {
        {"logo/vboycolor_header.png", L"LOGO_HEADER"},
        {"fonts/Roboto-Regular.ttf", L"FONT_ROBOTO_REGULAR"},
        {"fonts/Roboto-Bold.ttf", L"FONT_ROBOTO_BOLD"},
        {"icons/icons_atlas_10.png", L"ICON_ATLAS_10"},
        {"icons/icons_atlas_20.png", L"ICON_ATLAS_20"},
        {"icons/icons_atlas_30.png", L"ICON_ATLAS_30"},
        {"icons/icons_atlas_40.png", L"ICON_ATLAS_40"},
        {"icons/icons_atlas_50.png", L"ICON_ATLAS_50"},
        {"icons/icons_atlas_60.png", L"ICON_ATLAS_60"},
    };

    for (const Entry &entry : kEmbedded)
    {
        if (name != entry.name)
            continue;

        const HMODULE module = GetModuleHandleW(nullptr);
        // MAKEINTRESOURCEW(10), not RT_RCDATA - that macro expands via the
        // ambient UNICODE define, which isn't set for this target, so it
        // resolves to the ANSI (LPSTR) form and won't compile against the
        // wide FindResourceW below.
        const HRSRC resInfo = FindResourceW(module, entry.resourceId, MAKEINTRESOURCEW(10));
        if (!resInfo)
            return {};
        const HGLOBAL resHandle = LoadResource(module, resInfo);
        if (!resHandle)
            return {};
        const auto *data = static_cast<const uint8_t *>(LockResource(resHandle));
        const DWORD size = SizeofResource(module, resInfo);
        if (!data || size == 0)
            return {};
        return std::vector<uint8_t>(data, data + size);
    }
    // Anything else (the built-in color packs, colorpacks/): a file next to
    // the exe (the working directory - see the constructor).
    std::ifstream in(name, std::ios::binary);
    if (!in)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
#else
std::vector<uint8_t> DesktopPlatform::LoadAssetBytes(const std::string &name)
{
    // Non-Windows desktop: fonts/, icons/, logo/ and colorpacks/ next to the
    // executable (the working directory - see the constructor; the build
    // copies them there).
    std::ifstream in(name, std::ios::binary);
    if (!in)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
#endif

// ---------------------------------------------------------------------------
// Downloads (the library's optional box art)

namespace
{
    constexpr size_t kMaxDownloadBytes = 16 * 1024 * 1024;

#if defined(_WIN32)
    bool HttpGet(const std::string &url, std::vector<uint8_t> &out)
    {
        const std::wstring wideUrl(url.begin(), url.end()); // (ASCII - callers percent-encode)
        wchar_t host[256] = {}, path[2048] = {};
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.lpszHostName = host;
        parts.dwHostNameLength = static_cast<DWORD>(sizeof(host) / sizeof(host[0]));
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
        if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts))
            return false;

        HINTERNET session = WinHttpOpen(L"VBoyColor", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
            return false;
        WinHttpSetTimeouts(session, 10000, 10000, 20000, 20000);
        HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
        HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                            parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                       : nullptr;
        bool ok = request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                  WinHttpReceiveResponse(request, nullptr);
        if (ok)
        {
            DWORD status = 0, size = sizeof(status);
            ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                     &status, &size, WINHTTP_NO_HEADER_INDEX) &&
                 status == 200;
        }
        while (ok)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available))
                ok = false;
            if (!ok || available == 0)
                break;
            const size_t at = out.size();
            out.resize(at + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, out.data() + at, available, &read))
                ok = false;
            out.resize(at + read);
            if (out.size() > kMaxDownloadBytes)
                ok = false;
        }
        if (request)
            WinHttpCloseHandle(request);
        if (connection)
            WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return ok && !out.empty();
    }
#else
    // (desktop Linux/macOS are for development - curl does it)
    bool HttpGet(const std::string &url, std::vector<uint8_t> &out)
    {
        if (url.find('\'') != std::string::npos)
            return false;
        const std::string command = "curl -sfL --max-time 30 '" + url + "'";
        FILE *pipe = popen(command.c_str(), "r");
        if (!pipe)
            return false;
        uint8_t buffer[16384];
        size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0 && out.size() <= kMaxDownloadBytes)
            out.insert(out.end(), buffer, buffer + read);
        return pclose(pipe) == 0 && !out.empty() && out.size() <= kMaxDownloadBytes;
    }
#endif
} // namespace

struct DesktopPlatform::Download
{
    std::mutex mutex;
    int state = -1; // see PollDownload
    std::vector<uint8_t> bytes;
};

bool DesktopPlatform::StartDownload(const std::string &url)
{
    if (!m_download)
        m_download = std::make_shared<Download>();
    {
        std::lock_guard<std::mutex> lock(m_download->mutex);
        if (m_download->state == 0)
            return false;
        m_download->state = 0;
        m_download->bytes.clear();
    }
    // Detached: it only touches its own shared Download, so the app can
    // quit while one is still running.
    std::thread([download = m_download, url]()
                {
                    std::vector<uint8_t> bytes;
                    const bool ok = HttpGet(url, bytes);
                    std::lock_guard<std::mutex> lock(download->mutex);
                    download->bytes = std::move(bytes);
                    download->state = ok ? 1 : -1;
                })
        .detach();
    return true;
}

int DesktopPlatform::PollDownload(std::vector<uint8_t> &bytes)
{
    if (!m_download)
        return -1;
    std::lock_guard<std::mutex> lock(m_download->mutex);
    const int state = m_download->state;
    if (state == 1)
    {
        bytes = std::move(m_download->bytes);
        m_download->bytes.clear();
        m_download->state = -1; // (reported once)
    }
    return state;
}
