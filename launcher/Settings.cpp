#include "Settings.h"

#include <ShlObj.h>
#include <cstdio>
#include <string>
#include <system_error>

#pragma comment(lib, "Shell32.lib")

namespace crime_trainer {
namespace {

constexpr wchar_t kFileName[] = L"CrimeTrainer.ini";
constexpr wchar_t kSection[] = L"trainer";

constexpr const wchar_t* kFeatureKeys[kFeatureSlots] = {
    L"freezeTime", L"ignorePolice", L"ignoreCivilians", L"ignoreCameras",
    L"ignoreSensors", L"infiniteAmmo", L"unbreakableItems",
};

std::filesystem::path ExecutableDirectory() {
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}

bool DirectoryIsWritable(const std::filesystem::path& directory) {
    const std::filesystem::path probe = directory / L"CrimeTrainer.write-test";
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}

std::filesystem::path FallbackDirectory() {
    PWSTR appData = nullptr;
    std::filesystem::path directory;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData)) && appData) {
        directory = std::filesystem::path(appData) / L"CrimeTrainer";
        CoTaskMemFree(appData);
        std::error_code ignored;
        std::filesystem::create_directories(directory, ignored);
    }
    return directory;
}

int ReadInt(const std::filesystem::path& path, const wchar_t* key, int fallback) {
    return static_cast<int>(
        GetPrivateProfileIntW(kSection, key, fallback, path.c_str()));
}

void WriteInt(const std::filesystem::path& path, const wchar_t* key, int value) {
    WritePrivateProfileStringW(kSection, key, std::to_wstring(value).c_str(), path.c_str());
}

} // namespace

std::filesystem::path SettingsPath() {
    // Кэшируем: проверка лезет на диск, а путь спрашивают при каждом сохранении.
    static const std::filesystem::path path = [] {
        const std::filesystem::path local = ExecutableDirectory();
        if (!local.empty() && DirectoryIsWritable(local)) return local / kFileName;
        const std::filesystem::path fallback = FallbackDirectory();
        return fallback.empty() ? (local / kFileName) : (fallback / kFileName);
    }();
    return path;
}

Settings LoadSettings() {
    Settings settings;
    const std::filesystem::path path = SettingsPath();

    std::error_code ignored;
    if (!std::filesystem::exists(path, ignored)) return settings;

    for (int i = 0; i < kFeatureSlots; ++i) {
        settings.features[static_cast<std::size_t>(i)] = ReadInt(path, kFeatureKeys[i], 0) != 0;
    }
    settings.windowX = ReadInt(path, L"windowX", INT_MIN);
    settings.windowY = ReadInt(path, L"windowY", INT_MIN);
    settings.globalHotkeys = ReadInt(path, L"globalHotkeys", 1) != 0;
    return settings;
}

void SaveSettings(const Settings& settings) {
    const std::filesystem::path path = SettingsPath();
    for (int i = 0; i < kFeatureSlots; ++i) {
        WriteInt(path, kFeatureKeys[i], settings.features[static_cast<std::size_t>(i)] ? 1 : 0);
    }
    if (settings.HasSavedPosition()) {
        WriteInt(path, L"windowX", settings.windowX);
        WriteInt(path, L"windowY", settings.windowY);
    }
    WriteInt(path, L"globalHotkeys", settings.globalHotkeys ? 1 : 0);

    // Windows кэширует запись в ini и сбрасывает её лениво. Если трейнер убить
    // через диспетчер задач, последние переключения потеряются. Три nullptr
    // заставляют сбросить кэш на диск прямо сейчас.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
}

} // namespace crime_trainer
