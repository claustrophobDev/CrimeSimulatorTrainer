#pragma once

#include <Windows.h>
#include <array>
#include <filesystem>

#include "../include/SharedState.h"

namespace crime_trainer {

// Сохраняется между запусками: позиция окна и включённые функции.
// Лежит рядом с exe, а если туда нельзя писать - в %LOCALAPPDATA%.
struct Settings final {
    std::array<bool, kFeatureSlots> features{};
    int windowX = INT_MIN;
    int windowY = INT_MIN;
    bool globalHotkeys = true;

    bool HasSavedPosition() const noexcept { return windowX != INT_MIN && windowY != INT_MIN; }
};

std::filesystem::path SettingsPath();
Settings LoadSettings();
void SaveSettings(const Settings& settings);

} // namespace crime_trainer
