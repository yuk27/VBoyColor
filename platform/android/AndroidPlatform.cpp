#include "android/AndroidPlatform.h"

#include <android/asset_manager.h>

#include <algorithm>
#include <cstdio>
#include <unistd.h>

AndroidPlatform::AndroidPlatform(JavaVM *vm, jobject activityClazz, AAssetManager *assetManager)
    : m_vm(vm), m_assetManager(assetManager)
{
    JNIEnv *env = nullptr;
    vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    m_activity = env->NewGlobalRef(activityClazz);
}

bool AndroidPlatform::HasRomsFolder() const
{
    JNIEnv *env = nullptr;
    m_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);

    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID getUri = env->GetMethodID(activityClass, "getRomsTreeUriString", "()Ljava/lang/String;");
    auto uri = static_cast<jstring>(env->CallObjectMethod(m_activity, getUri));
    const bool has = uri != nullptr;
    env->DeleteLocalRef(activityClass);
    if (uri)
        env->DeleteLocalRef(uri);
    return has;
}

void AndroidPlatform::RequestChangeRomsFolder()
{
    JNIEnv *env = nullptr;
    m_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);

    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID request = env->GetMethodID(activityClass, "requestChangeRomsFolder", "()V");
    env->CallVoidMethod(m_activity, request);
    env->DeleteLocalRef(activityClass);
}

std::vector<RomEntry> AndroidPlatform::ScanRoms()
{
    std::vector<RomEntry> result;
    JNIEnv *env = nullptr;
    m_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);

    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID listMethod = env->GetMethodID(activityClass, "listRomFiles", "()[Ljava/lang/String;");
    auto array = static_cast<jobjectArray>(env->CallObjectMethod(m_activity, listMethod));
    env->DeleteLocalRef(activityClass);
    if (!array)
        return result;

    const jsize count = env->GetArrayLength(array);
    for (jsize i = 0; i + 1 < count; i += 2)
    {
        auto nameStr = static_cast<jstring>(env->GetObjectArrayElement(array, i));
        auto uriStr = static_cast<jstring>(env->GetObjectArrayElement(array, i + 1));

        const char *nameChars = env->GetStringUTFChars(nameStr, nullptr);
        const char *uriChars = env->GetStringUTFChars(uriStr, nullptr);
        result.push_back({nameChars, uriChars});
        env->ReleaseStringUTFChars(nameStr, nameChars);
        env->ReleaseStringUTFChars(uriStr, uriChars);

        env->DeleteLocalRef(nameStr);
        env->DeleteLocalRef(uriStr);
    }
    env->DeleteLocalRef(array);

    // Already alphabetical from MainActivity.listRomFiles's own sort, but
    // ScanRoms() promises case-insensitive sorted order to every caller
    // regardless of platform - cheap enough not to special-case away.
    std::sort(result.begin(), result.end(), [](const RomEntry &a, const RomEntry &b)
              { return a.name < b.name; });
    return result;
}

std::vector<uint8_t> AndroidPlatform::ReadAllFromFd(int fd)
{
    std::vector<uint8_t> bytes;
    if (fd < 0)
        return bytes;

    FILE *f = fdopen(fd, "rb");
    if (!f)
    {
        close(fd);
        return bytes;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0)
    {
        bytes.resize(static_cast<size_t>(size));
        const size_t bytesRead = fread(bytes.data(), 1, bytes.size(), f);
        bytes.resize(bytesRead);
    }
    fclose(f); // also closes the underlying fd
    return bytes;
}

std::vector<uint8_t> AndroidPlatform::ReadRomFile(const std::string &path)
{
    JNIEnv *env = nullptr;
    m_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);

    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID openMethod = env->GetMethodID(activityClass, "openRomFileDescriptor", "(Ljava/lang/String;)I");
    jstring uriJString = env->NewStringUTF(path.c_str());
    const jint fd = env->CallIntMethod(m_activity, openMethod, uriJString);
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(uriJString);

    return ReadAllFromFd(fd);
}

bool AndroidPlatform::WriteRomsFile(const std::string &fileName, bool inStatesDir, const void *data, size_t size)
{
    JNIEnv *env = nullptr;
    m_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);

    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID openMethod = env->GetMethodID(activityClass, "openDataFileForWrite", "(ILjava/lang/String;)I");
    jstring nameJString = env->NewStringUTF(fileName.c_str());
    const jint fd = env->CallIntMethod(m_activity, openMethod, static_cast<jint>(DataKindOf(fileName, inStatesDir)), nameJString);
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(nameJString);

    if (fd < 0)
        return false;

    FILE *f = fdopen(fd, "wb");
    if (!f)
    {
        close(fd);
        return false;
    }
    const size_t written = fwrite(data, 1, size, f);
    fclose(f);
    return written == size;
}

std::vector<uint8_t> AndroidPlatform::ReadRomsFile(const std::string &fileName, bool inStatesDir)
{
    JNIEnv *env = nullptr;
    m_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);

    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID openMethod = env->GetMethodID(activityClass, "openDataFileForRead", "(ILjava/lang/String;)I");
    jstring nameJString = env->NewStringUTF(fileName.c_str());
    const jint fd = env->CallIntMethod(m_activity, openMethod, static_cast<jint>(DataKindOf(fileName, inStatesDir)), nameJString);
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(nameJString);

    return ReadAllFromFd(fd);
}

bool AndroidPlatform::RomsFileExists(const std::string &fileName, bool inStatesDir) const
{
    JNIEnv *env = nullptr;
    m_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);

    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID existsMethod = env->GetMethodID(activityClass, "dataFileExists", "(ILjava/lang/String;)Z");
    jstring nameJString = env->NewStringUTF(fileName.c_str());
    const bool exists = env->CallBooleanMethod(m_activity, existsMethod, static_cast<jint>(DataKindOf(fileName, inStatesDir)), nameJString);
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(nameJString);
    return exists;
}

std::string AndroidPlatform::CallKindString(const char *method, DataKind kind) const
{
    JNIEnv *env = nullptr;
    m_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID id = env->GetMethodID(activityClass, method, "(I)Ljava/lang/String;");
    env->DeleteLocalRef(activityClass);
    auto result = static_cast<jstring>(env->CallObjectMethod(m_activity, id, static_cast<jint>(kind)));
    std::string text;
    if (result)
    {
        const char *chars = env->GetStringUTFChars(result, nullptr);
        text = chars;
        env->ReleaseStringUTFChars(result, chars);
        env->DeleteLocalRef(result);
    }
    return text;
}

std::string AndroidPlatform::DataFolderLabel(DataKind kind) const
{
    return CallKindString("getFolderLabel", kind);
}

std::string AndroidPlatform::SetDataFolder(DataKind kind, const std::string &path)
{
    // (only back to the default place - other folders come from the picker)
    return path.empty() ? CallKindString("resetFolder", kind) : "";
}

std::string AndroidPlatform::PickDataFolder(DataKind kind)
{
    return CallKindString("pickFolder", kind);
}

std::string AndroidPlatform::TakeDataFolderStatus()
{
    JNIEnv *env = nullptr;
    m_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID id = env->GetMethodID(activityClass, "takeFolderStatus", "()Ljava/lang/String;");
    env->DeleteLocalRef(activityClass);
    auto result = static_cast<jstring>(env->CallObjectMethod(m_activity, id));
    std::string text;
    if (result)
    {
        const char *chars = env->GetStringUTFChars(result, nullptr);
        text = chars;
        env->ReleaseStringUTFChars(result, chars);
        env->DeleteLocalRef(result);
    }
    return text;
}

int AndroidPlatform::GetBatteryPercent() const
{
    JNIEnv *env = nullptr;
    m_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);

    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID getBatteryLevel = env->GetMethodID(activityClass, "getBatteryLevel", "()I");
    const int percent = env->CallIntMethod(m_activity, getBatteryLevel);
    env->DeleteLocalRef(activityClass);
    return percent;
}

bool AndroidPlatform::StartDownload(const std::string &url)
{
    JNIEnv *env = nullptr;
    m_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);

    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID start = env->GetMethodID(activityClass, "startDownload", "(Ljava/lang/String;)Z");
    jstring urlJString = env->NewStringUTF(url.c_str());
    const bool started = env->CallBooleanMethod(m_activity, start, urlJString);
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(urlJString);
    return started;
}

int AndroidPlatform::PollDownload(std::vector<uint8_t> &bytes)
{
    JNIEnv *env = nullptr;
    m_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);

    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID poll = env->GetMethodID(activityClass, "pollDownload", "()I");
    int state = env->CallIntMethod(m_activity, poll);
    if (state == 1)
    {
        jmethodID take = env->GetMethodID(activityClass, "takeDownload", "()[B");
        auto array = static_cast<jbyteArray>(env->CallObjectMethod(m_activity, take));
        if (array)
        {
            const jsize length = env->GetArrayLength(array);
            bytes.resize(static_cast<size_t>(length));
            env->GetByteArrayRegion(array, 0, length, reinterpret_cast<jbyte *>(bytes.data()));
            env->DeleteLocalRef(array);
        }
        else
            state = -1;
    }
    env->DeleteLocalRef(activityClass);
    return state;
}

std::vector<uint8_t> AndroidPlatform::LoadAssetBytes(const std::string &name)
{
    if (!m_assetManager)
        return {};
    AAsset *asset = AAssetManager_open(m_assetManager, name.c_str(), AASSET_MODE_BUFFER);
    if (!asset)
        return {};
    const off_t length = AAsset_getLength(asset);
    std::vector<uint8_t> data(static_cast<size_t>(length));
    AAsset_read(asset, data.data(), data.size());
    AAsset_close(asset);
    return data;
}
