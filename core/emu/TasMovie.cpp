#include "emu/TasMovie.h"

#include <stb_image.h> // (stbi_zlib_decode_noheader_malloc - zip's deflate; implementation in VulkanRenderer.cpp)

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>

namespace
{
    uint32_t Le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
    uint32_t Le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

    // One file out of a zip, by name: stored or deflated. Empty if missing.
    bool ZipFile(const std::vector<uint8_t> &zip, const std::string &name, std::string &out)
    {
        const size_t n = zip.size();
        if (n < 22)
            return false;
        // End of central directory: the last "PK\5\6" (a comment may follow it).
        size_t end = std::string::npos;
        for (size_t i = n - 22 + 1; i-- > 0 && i + 65557 >= n;)
            if (Le32(&zip[i]) == 0x06054b50)
            {
                end = i;
                break;
            }
        if (end == std::string::npos)
            return false;
        const uint32_t entries = Le16(&zip[end + 10]);
        size_t at = Le32(&zip[end + 16]);
        for (uint32_t e = 0; e < entries; ++e)
        {
            if (at + 46 > n || Le32(&zip[at]) != 0x02014b50)
                return false;
            const uint32_t method = Le16(&zip[at + 10]);
            const uint32_t packed = Le32(&zip[at + 20]);
            const uint32_t size = Le32(&zip[at + 24]);
            const uint32_t nameLen = Le16(&zip[at + 28]), extraLen = Le16(&zip[at + 30]), commentLen = Le16(&zip[at + 32]);
            const uint32_t local = Le32(&zip[at + 42]);
            if (at + 46 + nameLen > n)
                return false;
            const std::string entryName(reinterpret_cast<const char *>(&zip[at + 46]), nameLen);
            at += 46 + nameLen + extraLen + commentLen;
            if (entryName != name)
                continue;
            if (local + 30 > n || Le32(&zip[local]) != 0x04034b50)
                return false;
            const size_t data = local + 30 + Le16(&zip[local + 26]) + Le16(&zip[local + 28]);
            if (data + packed > n)
                return false;
            if (method == 0)
            {
                out.assign(reinterpret_cast<const char *>(&zip[data]), packed);
                return true;
            }
            if (method != 8)
                return false;
            int length = 0;
            char *raw = stbi_zlib_decode_noheader_malloc(reinterpret_cast<const char *>(&zip[data]), static_cast<int>(packed), &length);
            if (!raw)
                return false;
            out.assign(raw, static_cast<size_t>(length));
            std::free(raw);
            return size == 0 || out.size() == size;
        }
        return false;
    }

    std::string Trim(std::string s)
    {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
            s.pop_back();
        size_t i = 0;
        while (i < s.size() && s[i] == ' ')
            ++i;
        return s.substr(i);
    }
} // namespace

bool TasMovie::Parse(const std::vector<uint8_t> &bk2, TasMovie &out, std::string &error)
{
    out = TasMovie{};
    std::string header, log;
    if (!ZipFile(bk2, "Header.txt", header) || !ZipFile(bk2, "Input Log.txt", log))
    {
        error = "not a BizHawk movie (.bk2)";
        return false;
    }
    std::istringstream headerLines(header);
    std::string platform;
    for (std::string line; std::getline(headerLines, line);)
    {
        line = Trim(line);
        const size_t space = line.find(' ');
        if (space == std::string::npos)
            continue;
        const std::string key = line.substr(0, space), value = Trim(line.substr(space + 1));
        if (key == "GameName")
            out.gameName = value;
        else if (key == "SHA1")
        {
            out.romSha1 = value;
            for (char &c : out.romSha1)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        else if (key == "Core")
            out.core = value;
        else if (key == "Platform")
            platform = value;
    }
    if (!platform.empty() && platform != "VB")
    {
        error = "a movie for another system (" + platform + ")";
        return false;
    }

    // The columns, from LogKey ("#L_Up|L_Down|...|Power|"): each frame line
    // has one character per button, '.' when it isn't pressed.
    static const std::map<std::string, int> kButtons = {
        {"L_Up", 4}, {"L_Down", 5}, {"L_Left", 6}, {"L_Right", 7},     // RETRO_DEVICE_ID_JOYPAD_UP/DOWN/LEFT/RIGHT
        {"R_Up", 12}, {"R_Down", 14}, {"R_Left", 13}, {"R_Right", 15}, // L2/L3/R2/R3 (Beetle VB's right pad)
        {"B", 0}, {"A", 8}, {"L", 10}, {"R", 11}, {"Select", 2}, {"Start", 3}};
    std::vector<int> columns;
    std::istringstream logLines(log);
    for (std::string line; std::getline(logLines, line);)
    {
        line = Trim(line);
        if (line.rfind("LogKey:", 0) == 0)
        {
            columns.clear();
            std::string keys = line.substr(7);
            if (!keys.empty() && keys[0] == '#')
                keys.erase(0, 1);
            std::istringstream names(keys);
            // (newer BizHawk: "#Power|Reset|#P1 L_Up|...": a '#' starts a
            // group, a player's buttons are named "P1 ...")
            for (std::string name; std::getline(names, name, '|');)
            {
                while (!name.empty() && name[0] == '#')
                    name.erase(0, 1);
                if (name.empty())
                    continue;
                if (name.rfind("P1 ", 0) == 0)
                    name.erase(0, 3);
                const auto it = kButtons.find(name);
                columns.push_back(it == kButtons.end() ? -1 : it->second);
            }
            continue;
        }
        if (line.size() < 2 || line[0] != '|')
            continue;
        if (columns.empty()) // (no LogKey: BizHawk's Virtual Boy order)
            columns = {4, 5, 6, 7, 12, 14, 13, 15, 0, 8, 10, 11, 2, 3, -1};
        uint32_t bits = 0;
        size_t column = 0;
        for (size_t i = 1; i < line.size() && column < columns.size(); ++i)
        {
            if (line[i] == '|')
                continue;
            if (line[i] != '.' && line[i] != ' ' && columns[column] >= 0)
                bits |= 1u << columns[column];
            ++column;
        }
        out.frames.push_back(bits);
    }
    if (out.frames.empty())
    {
        error = "the movie has no frames";
        return false;
    }
    return true;
}

// SHA-1 (FIPS 180-1), small and plain.
std::string TasMovie::Sha1(const uint8_t *data, size_t size)
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    auto rol = [](uint32_t v, int s) { return (v << s) | (v >> (32 - s)); };
    auto block = [&](const uint8_t *p)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) | p[i * 4 + 3];
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i)
        {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    };
    size_t i = 0;
    for (; i + 64 <= size; i += 64)
        block(data + i);
    uint8_t tail[128] = {};
    const size_t rest = size - i;
    std::memcpy(tail, data + i, rest);
    tail[rest] = 0x80;
    const size_t blocks = rest + 9 <= 64 ? 1 : 2;
    const uint64_t bits = static_cast<uint64_t>(size) * 8;
    for (int b = 0; b < 8; ++b)
        tail[blocks * 64 - 1 - b] = static_cast<uint8_t>(bits >> (b * 8));
    for (size_t b = 0; b < blocks; ++b)
        block(tail + b * 64);
    char hex[41];
    for (int k = 0; k < 5; ++k)
        std::snprintf(hex + k * 8, 9, "%08x", h[k]);
    return std::string(hex, 40);
}
