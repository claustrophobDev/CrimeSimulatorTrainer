#pragma once

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <string>

// Общая память между лаунчером и hook DLL.
// Лаунчер создаёт mapping, DLL его открывает. Пишем только через Interlocked,
// потому что читают и пишут разные потоки в разных процессах.

namespace crime_trainer {

inline constexpr std::uint32_t kSharedMagic = 0x4354524Eu; // 'CTRN'
inline constexpr std::uint32_t kSharedVersion = 8;

inline constexpr wchar_t kGameProcessName[] = L"Crime Simulator.exe";
inline constexpr wchar_t kHookDllName[] = L"CrimeTrainerHook.dll";

inline constexpr int kFeatureSlots = 10;

// Если одна сторона не обновляла свой тик дольше этого - считаем её мёртвой.
inline constexpr DWORD kHeartbeatTimeoutMs = 4000;

// Доступна ли функция в той сборке игры, которая сейчас запущена.
enum FeatureSupport : LONG {
    kSupportUnknown = 0,
    kSupportReady = 1,
    kSupportWaiting = 2,     // нужен загруженный мир / игрок
    kSupportUnsupported = 3, // в этой сборке ничего подходящего не нашлось
};

struct FeatureState final {
    volatile LONG requested;
    volatile LONG applied;
    volatile LONG support;
};

struct SharedState final {
    std::uint32_t magic;
    std::uint32_t version;

    volatile LONG launcherAlive;
    volatile LONG hookReady;
    volatile LONG unloadRequested;
    volatile LONG unloadComplete;

    FeatureState freezeTime;
    FeatureState ignorePolice;
    FeatureState ignoreCivilians;
    FeatureState ignoreCameras;
    FeatureState ignoreSensors;
    FeatureState infiniteAmmo;
    FeatureState unbreakableItems;
    FeatureState silentSteps;
    FeatureState infiniteStamina;
    FeatureState noDamage;

    volatile LONG hour;
    volatile LONG minute;

    volatile DWORD launcherHeartbeat;
    volatile DWORD hookHeartbeat;

    // Сколько method patch'ей реально удалось разрешить. Если после патча игры
    // тут 0 - сразу видно, что сборка сменилась.
    volatile LONG resolvedPatches;

    wchar_t status[192];
};

static_assert(sizeof(FeatureState) == 12);
static_assert(offsetof(SharedState, freezeTime) == 24);
static_assert(offsetof(SharedState, noDamage) == 132);
static_assert(offsetof(SharedState, status) == 164);
static_assert(sizeof(SharedState) == 548);

inline std::wstring MappingName(DWORD pid) {
    return L"Local\\CrimeTrainer_v8_" + std::to_wstring(pid);
}

// GetTickCount переполняется примерно раз в 49 дней. Вычитание в беззнаковом
// типе переживает переполнение, поэтому живой сосед не выглядит мёртвым.
inline bool HeartbeatExpired(DWORD beat, DWORD now) {
    return static_cast<DWORD>(now - beat) > kHeartbeatTimeoutMs;
}

} // namespace crime_trainer
