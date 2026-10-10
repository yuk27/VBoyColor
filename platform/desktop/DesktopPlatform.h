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
    // settingsFileName: see Platform::SettingsFileName (the VR app passes
    // "settings-vr.dat").
    explicit DesktopPlatform(const char *settingsFileName = "settings.dat");

    const char *SettingsFileName() const override { return m_settingsFileName; }

    bool HasRomsFolder() const override { return true; } // no picker - always "has" the fixed folder
    bool SupportsChangeRomsFolder() const override { return false; }
    void RequestChangeRomsFolder() override {}

    std::vector<RomEntry> ScanRoms() override;
    std::vector<uint8_t> ReadRomFile(const std::string &path) override;

    bool WriteRomsFile(const std::string &fileName, bool inStatesDir, const void *data, size_t size) override;
    std::vector<uint8_t> ReadRomsFile(const std::string &fileName, bool inStatesDir) override;
    bool RomsFileExists(const std::string &fileName, bool inStatesDir) const override;
    std::vector<std::string> ListRomsSubfolder(const std::string &subfolder) const override;

    // Settings > Folders: in the app's own folder browser; the choices are
    // kept in folders.txt next to the exe (shared by the flat and VR apps,
    // and outside the folders themselves, so the settings folder can move).
    bool SupportsDataFolders() const override { return true; }
    std::string DataFolderLabel(DataKind kind) const override;
    bool BrowsesDataFolders() const override { return true; }
    std::string DataFolderPath(DataKind kind) const override;
    std::vector<std::pair<std::string, std::string>> FolderPlaces() const override;
    std::vector<std::string> ListFolders(const std::string &path) const override;
    std::string SetDataFolder(DataKind kind, const std::string &path) override;

    int GetBatteryPercent() const override { return -1; } // no real battery to read off Android

    // On a thread of its own: WinHTTP on Windows, curl elsewhere.
    bool StartDownload(const std::string &url) override;
    int PollDownload(std::vector<uint8_t> &bytes) override;

    std::vector<uint8_t> LoadAssetBytes(const std::string &name) override;

private:
    // A kind's folder (UTF-8; relative ones are to the exe's folder).
    std::string FolderOf(DataKind kind) const;
    std::string PathFor(const std::string &fileName, bool inStatesDir) const;
    void LoadFolders();
    bool SaveFolders() const;

    const char *m_settingsFileName;
    std::string m_appFolder;                 // the exe's (UTF-8)
    std::string m_folders[kDataKindCount];   // chosen ones, "" = default
    struct Download;
    std::shared_ptr<Download> m_download; // (shared with its thread)
};
