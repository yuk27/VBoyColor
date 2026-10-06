#include "io/Settings.h"
#include "io/Platform.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr const char *kSettingsFileName = "settings.dat";

    // Versions 12 and 13 only appended fields (selectedShadePalette, then
    // passthrough), so an older file's AppSettings bytes are exactly a
    // prefix of today's layout - copying just that much keeps everything the
    // user had set up (button mapping, screen placement, palette) and leaves
    // the newer fields at their defaults.
    size_t PrefixCompatibleSize(int version)
    {
        switch (version)
        {
        case 11:
            return offsetof(AppSettings, selectedShadePalette);
        case 12:
            return offsetof(AppSettings, passthrough);
        default:
            return 0;
        }
    }

    // Shared by both platform Load paths.
    bool ApplyLoadedSettings(AppSettings &self, int version, const AppSettings &loaded)
    {
        if (version != AppSettings::kVersion)
            return false; // stale layout - leave self untouched, defaults stand

        self = loaded;
        return true;
    }
} // namespace

void AppSettings::Save(Platform &platform) const
{
    std::vector<uint8_t> bytes(sizeof(int) + sizeof(AppSettings));
    const int version = kVersion;
    std::memcpy(bytes.data(), &version, sizeof(version));
    std::memcpy(bytes.data() + sizeof(version), this, sizeof(AppSettings));
    platform.WriteRomsFile(kSettingsFileName, true, bytes.data(), bytes.size());
}

bool AppSettings::Load(Platform &platform)
{
    const std::vector<uint8_t> bytes = platform.ReadRomsFile(kSettingsFileName, true);
    if (bytes.size() < sizeof(int))
        return false;

    int version = 0;
    std::memcpy(&version, bytes.data(), sizeof(version));

    if (const size_t prefix = PrefixCompatibleSize(version); prefix && bytes.size() >= sizeof(int) + prefix)
    {
        AppSettings migrated; // fields past the prefix keep their defaults
        std::memcpy(&migrated, bytes.data() + sizeof(version), prefix);
        *this = migrated;
        return true;
    }

    if (bytes.size() < sizeof(int) + sizeof(AppSettings))
        return false;
    AppSettings loaded;
    std::memcpy(&loaded, bytes.data() + sizeof(version), sizeof(AppSettings));
    return ApplyLoadedSettings(*this, version, loaded);
}
