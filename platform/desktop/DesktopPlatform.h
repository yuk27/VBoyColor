#pragma once

#include "io/Platform.h"

#include <memory>

// Platform implementation for Windows/desktop (both platform/pc/'s OpenXR
// build and platform/pc2d's flat debug window) - a fixed relative ROMs
// folder on disk instead of Android's SAF grant, so no per-instance JNI
// state to hold.
class DesktopPlatform : public Platform
{
public:
    // Makes the exe's folder the working directory, so the roms folder (and,
    // off Windows, fonts/ and icons/) next to it are found however the app
    // was started (a shortcut, a terminal somewhere else).
    DesktopPlatform();

    bool HasRomsFolder() const override { return true; } // no picker - always "has" the fixed folder
    bool SupportsChangeRomsFolder() const override { return false; }
    void RequestChangeRomsFolder() override {}

    std::vector<RomEntry> ScanRoms() override;
    std::vector<uint8_t> ReadRomFile(const std::string &path) override;

    bool WriteRomsFile(const std::string &fileName, bool inStatesDir, const void *data, size_t size) override;
    std::vector<uint8_t> ReadRomsFile(const std::string &fileName, bool inStatesDir) override;
    bool RomsFileExists(const std::string &fileName, bool inStatesDir) const override;
    std::vector<std::string> ListRomsSubfolder(const std::string &subfolder) const override;

    int GetBatteryPercent() const override { return -1; } // no real battery to read off Android

    // On a thread of its own: WinHTTP on Windows, curl elsewhere.
    bool StartDownload(const std::string &url) override;
    int PollDownload(std::vector<uint8_t> &bytes) override;

    std::vector<uint8_t> LoadAssetBytes(const std::string &name) override;

private:
    struct Download;
    std::shared_ptr<Download> m_download; // (shared with its thread)
};
