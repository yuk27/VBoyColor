#pragma once

#include "io/Platform.h"

#include <jni.h>

struct AAssetManager;

// Platform implementation for Android - JNI bridge to MainActivity.java's
// Storage Access Framework ROMs folder (SAF instead of a plain /sdcard path:
// at target SDK 34 that needs MANAGE_EXTERNAL_STORAGE, which kills and
// restarts the process the moment it's granted) plus misc device queries
// (battery).
class AndroidPlatform : public Platform
{
public:
    // vm/activityClazz/assetManager all come from android_app::activity -
    // called once at startup, before anything else here.
    AndroidPlatform(JavaVM *vm, jobject activityClazz, AAssetManager *assetManager);

    bool HasRomsFolder() const override;
    bool SupportsChangeRomsFolder() const override { return true; }
    void RequestChangeRomsFolder() override;

    std::vector<RomEntry> ScanRoms() override;
    std::vector<uint8_t> ReadRomFile(const std::string &path) override;

    bool WriteRomsFile(const std::string &fileName, bool inStatesDir, const void *data, size_t size) override;
    std::vector<uint8_t> ReadRomsFile(const std::string &fileName, bool inStatesDir) override;
    bool RomsFileExists(const std::string &fileName, bool inStatesDir) const override;

    // Settings > Folders, with the system's folder picker (MainActivity's
    // pickFolder: right away on a phone, at the next start on a headset).
    bool SupportsDataFolders() const override { return true; }
    std::string DataFolderLabel(DataKind kind) const override;
    std::string SetDataFolder(DataKind kind, const std::string &path) override; // ("" only: back to the default)
    std::string PickDataFolder(DataKind kind) override;
    std::string TakeDataFolderStatus() override;

    int GetBatteryPercent() const override;

    // MainActivity's download thread (startDownload/pollDownload/takeDownload).
    bool StartDownload(const std::string &url) override;
    int PollDownload(std::vector<uint8_t> &bytes) override;

    std::vector<uint8_t> LoadAssetBytes(const std::string &name) override;

private:
    // Calls a MainActivity method taking (int kind) and returning a String.
    std::string CallKindString(const char *method, DataKind kind) const;
    // Shared by ReadRomFile/ReadRomsFile - fd < 0 returns empty.
    static std::vector<uint8_t> ReadAllFromFd(int fd);

    JavaVM *m_vm = nullptr;
    jobject m_activity = nullptr; // global ref, set once by the constructor
    AAssetManager *m_assetManager = nullptr;
};
