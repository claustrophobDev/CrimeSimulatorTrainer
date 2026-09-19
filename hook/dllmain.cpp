#include <Windows.h>
#include <array>
#include <cwchar>
#include <memory>
#include <string>

#include "../include/SharedState.h"
#include "GameController.h"
#include "MonoApi.h"

using crime_trainer::FeatureState;
using crime_trainer::SharedState;

namespace {

volatile LONG g_running = TRUE;
volatile LONG g_subclassInstalled = FALSE;
volatile LONG g_mainInitialized = FALSE;

constexpr DWORD kWorkerNormalExit = 0;
constexpr DWORD kWorkerUnloadSelf = 1;
constexpr UINT kTrainerTickMessage = WM_APP + 0x531;
constexpr UINT kTrainerCleanupMessage = WM_APP + 0x532;

HMODULE g_module = nullptr;
HWND g_gameWindow = nullptr;
WNDPROC g_originalWndProc = nullptr;
SharedState* g_state = nullptr;
std::unique_ptr<MonoApi> g_mono;
std::unique_ptr<GameController> g_controller;

ULONGLONG g_nextAiTick = 0;
ULONGLONG g_nextItemTick = 0;
ULONGLONG g_nextClockTick = 0;
ULONGLONG g_nextFreezeTick = 0;

// Ставится при отправке тика, снимается, когда главный поток его забрал.
// Иначе при фризе игры тики копились бы в очереди и все разом выполнились
// в момент, когда она отвиснет.
volatile LONG g_tickPending = FALSE;

bool g_prevFreeze = false;
bool g_prevPolice = false;
bool g_prevCivilians = false;
bool g_prevCameras = false;
bool g_prevSensors = false;
bool g_prevAmmo = false;
bool g_prevDurability = false;
bool g_prevSilent = false;
bool g_prevStamina = false;
bool g_prevNoDamage = false;

void SetStatus(SharedState* state, const wchar_t* text) {
    if (!state || !text) return;
    wcsncpy_s(state->status, text, _TRUNCATE);
}

bool Requested(const FeatureState& feature) {
    return InterlockedCompareExchange(
        const_cast<volatile LONG*>(&feature.requested), TRUE, TRUE) != FALSE;
}

void SetApplied(FeatureState& feature, bool value) {
    InterlockedExchange(&feature.applied, value ? TRUE : FALSE);
}

LONG SupportOf(GameController::OpResult result) {
    switch (result) {
    case GameController::OpResult::Applied: return crime_trainer::kSupportReady;
    case GameController::OpResult::Waiting: return crime_trainer::kSupportWaiting;
    case GameController::OpResult::Unsupported: return crime_trainer::kSupportUnsupported;
    }
    return crime_trainer::kSupportUnknown;
}

// Публикует сразу две вещи: работает ли функция прямо сейчас и умеет ли её
// вообще эта сборка игры.
void Publish(FeatureState& feature, bool requested, GameController::OpResult result) {
    const LONG support = SupportOf(result);
    InterlockedExchange(&feature.support, support);
    SetApplied(feature, requested && result == GameController::OpResult::Applied);
}

bool UnloadRequested(SharedState* state) {
    return state &&
        InterlockedCompareExchange(&state->unloadRequested, FALSE, FALSE) != FALSE;
}

std::array<FeatureState*, crime_trainer::kFeatureSlots> AllFeatures(SharedState* state) {
    return { &state->freezeTime,       &state->ignorePolice,    &state->ignoreCivilians,
             &state->ignoreCameras,    &state->ignoreSensors,   &state->infiniteAmmo,
             &state->unbreakableItems, &state->silentSteps,     &state->infiniteStamina,
             &state->noDamage };
}

void ClearRequests(SharedState* state) {
    if (!state) return;
    for (FeatureState* feature : AllFeatures(state)) {
        InterlockedExchange(&feature->requested, FALSE);
    }
}

void ClearApplied(SharedState* state) {
    if (!state) return;
    for (FeatureState* feature : AllFeatures(state)) SetApplied(*feature, false);
}

void ClearSupport(SharedState* state) {
    if (!state) return;
    for (FeatureState* feature : AllFeatures(state)) {
        InterlockedExchange(&feature->support, crime_trainer::kSupportUnknown);
    }
}

// Лаунчер тикает на каждом опросе. Если его убили, а не закрыли, тики
// прекращаются, и hook выгружается сам, а не остаётся в игре с патчами.
bool LauncherGone(SharedState* state) {
    if (!state) return true;
    if (InterlockedCompareExchange(&state->launcherAlive, FALSE, FALSE) == FALSE) return true;
    const DWORD beat = InterlockedCompareExchange(
        reinterpret_cast<volatile LONG*>(&state->launcherHeartbeat), 0, 0);
    return crime_trainer::HeartbeatExpired(beat, GetTickCount());
}

void PublishHookHeartbeat(SharedState* state) {
    if (!state) return;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&state->hookHeartbeat),
        static_cast<LONG>(GetTickCount()));
}

void MarkUnloadReady(SharedState* state) {
    if (!state) return;
    ClearRequests(state);
    ClearApplied(state);
    ClearSupport(state);
    SetStatus(state, L"Состояния восстановлены • hook выгружается...");
    InterlockedExchange(&state->hookReady, FALSE);
    InterlockedExchange(&state->unloadComplete, TRUE);
    MemoryBarrier();
}

std::wstring LogPath() {
    wchar_t path[MAX_PATH]{};
    if (!g_module || !GetModuleFileNameW(g_module, path, MAX_PATH)) {
        return L"CrimeTrainerHook.log";
    }
    std::wstring result(path);
    const std::size_t slash = result.find_last_of(L"\\/");
    if (slash != std::wstring::npos) result.resize(slash + 1);
    else result.clear();
    result += L"CrimeTrainerHook.log";
    return result;
}

void LogLine(const std::wstring& line) {
    const std::wstring path = LogPath();
    HANDLE file = CreateFileW(
        path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t prefix[64]{};
    swprintf_s(prefix, L"[%02u:%02u:%02u.%03u] ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    const std::wstring full = std::wstring(prefix) + line + L"\r\n";

    const int bytesNeeded = WideCharToMultiByte(
        CP_UTF8, 0, full.c_str(), static_cast<int>(full.size()),
        nullptr, 0, nullptr, nullptr);
    if (bytesNeeded > 0) {
        std::string utf8(static_cast<std::size_t>(bytesNeeded), '\0');
        WideCharToMultiByte(CP_UTF8, 0, full.c_str(), static_cast<int>(full.size()),
            utf8.data(), bytesNeeded, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    CloseHandle(file);
}

BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM parameter) {
    auto* result = reinterpret_cast<HWND*>(parameter);
    if (!result || *result) return FALSE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

    wchar_t className[128]{};
    GetClassNameW(hwnd, className, _countof(className));

    // Обычно у Unity-сборки класс окна UnityWndClass. Если в новой версии
    // поменяют - берём первое видимое окно процесса с заголовком.
    if (_wcsicmp(className, L"UnityWndClass") == 0) {
        *result = hwnd;
        return FALSE;
    }

    if (IsWindowVisible(hwnd)) {
        wchar_t title[256]{};
        GetWindowTextW(hwnd, title, _countof(title));
        if (title[0] != L'\0') {
            *result = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

HWND FindGameWindow() {
    HWND result = nullptr;
    EnumWindows(&FindGameWindowProc, reinterpret_cast<LPARAM>(&result));
    return result;
}

void ResetTickState() {
    g_nextAiTick = 0;
    g_nextItemTick = 0;
    g_nextClockTick = 0;
    g_nextFreezeTick = 0;
    g_prevFreeze = false;
    g_prevPolice = false;
    g_prevCivilians = false;
    g_prevCameras = false;
    g_prevSensors = false;
    g_prevAmmo = false;
    g_prevDurability = false;
    g_prevSilent = false;
    g_prevStamina = false;
    g_prevNoDamage = false;
}

void MainThreadTick() {
    SharedState* state = g_state;
    if (!state || !g_mono || !g_mono->IsReady()) return;

    InterlockedExchange(&g_tickPending, FALSE);

    if (!g_controller) {
        g_controller = std::make_unique<GameController>(*g_mono);
        if (!g_controller->Initialize()) {
            SetStatus(state, g_controller->LastError().c_str());
            g_controller.reset();
            ClearApplied(state);
            ClearSupport(state);
            return;
        }

        ResetTickState();
        InterlockedExchange(&g_mainInitialized, TRUE);
        InterlockedExchange(&state->resolvedPatches,
            static_cast<LONG>(g_controller->ResolvedPatchCount()));
        SetStatus(state, L"Подключено • Unity main-thread mode");
        LogLine(L"=== CrimeTrainer v0.5 hook initialized ===");
        if (g_mono->UsesStrongHandles()) {
            LogLine(L"WARN: mono_gchandle_new_weakref missing; snapshots hold strong handles.");
        }
        for (const auto& line : g_controller->PatchDiagnostics()) LogLine(line);
    }

    const ULONGLONG now = GetTickCount64();

    const bool freeze = Requested(state->freezeTime);
    if (freeze != g_prevFreeze || now >= g_nextFreezeTick) {
        Publish(state->freezeTime, freeze, g_controller->SetFreezeTime(freeze));
        g_prevFreeze = freeze;
        g_nextFreezeTick = now + 250;
    }

    const bool police = Requested(state->ignorePolice);
    const bool civilians = Requested(state->ignoreCivilians);
    const bool cameras = Requested(state->ignoreCameras);
    const bool sensors = Requested(state->ignoreSensors);
    const bool aiChanged = police != g_prevPolice || civilians != g_prevCivilians ||
        cameras != g_prevCameras || sensors != g_prevSensors;

    if (aiChanged || now >= g_nextAiTick) {
        Publish(state->ignorePolice, police, g_controller->SetIgnorePolice(police));
        Publish(state->ignoreCivilians, civilians, g_controller->SetIgnoreCivilians(civilians));
        Publish(state->ignoreCameras, cameras, g_controller->SetIgnoreCameras(cameras));
        Publish(state->ignoreSensors, sensors, g_controller->SetIgnoreSensors(sensors));

        g_prevPolice = police;
        g_prevCivilians = civilians;
        g_prevCameras = cameras;
        g_prevSensors = sensors;
        g_nextAiTick = now + 1000;
    }

    // blockAllNoise достаточно выставить один раз - дальше игра сама держит
    // noiseRange в нуле, поэтому он в редком тике, а не в частом.
    const bool silent = Requested(state->silentSteps);
    if (silent != g_prevSilent || now >= g_nextAiTick) {
        Publish(state->silentSteps, silent, g_controller->SetSilentSteps(silent));
        g_prevSilent = silent;
    }

    const bool ammo = Requested(state->infiniteAmmo);
    const bool durability = Requested(state->unbreakableItems);
    const bool stamina = Requested(state->infiniteStamina);
    const bool noDamage = Requested(state->noDamage);
    const bool itemChanged = ammo != g_prevAmmo || durability != g_prevDurability ||
        stamina != g_prevStamina || noDamage != g_prevNoDamage;

    if (itemChanged || now >= g_nextItemTick) {
        Publish(state->infiniteAmmo, ammo, g_controller->SetInfiniteAmmo(ammo));
        Publish(state->unbreakableItems, durability, g_controller->SetUnbreakableItems(durability));
        Publish(state->infiniteStamina, stamina, g_controller->SetInfiniteStamina(stamina));
        Publish(state->noDamage, noDamage, g_controller->SetNoDamage(noDamage));
        g_prevAmmo = ammo;
        g_prevDurability = durability;
        g_prevStamina = stamina;
        g_prevNoDamage = noDamage;
        g_nextItemTick = now + 200;
    }

    if (now >= g_nextClockTick) {
        int hour = -1;
        int minute = -1;
        const bool clockOk = g_controller->GetClock(hour, minute);
        InterlockedExchange(&state->hour, clockOk ? hour : -1);
        InterlockedExchange(&state->minute, clockOk ? minute : -1);
        g_nextClockTick = now + 250;

        // Раньше статус перезаписывался на каждом тике и затирал ошибку,
        // которую функция только что записала. Теперь ошибка в приоритете.
        const std::wstring& error = g_controller->LastError();
        if (!error.empty()) {
            SetStatus(state, error.c_str());
        } else if (clockOk) {
            SetStatus(state, L"Подключено • события блокируются до реакции AI");
        } else {
            SetStatus(state, L"Hook активен • ожидание игрового мира");
        }
    }
}

void MainThreadCleanup() {
    if (g_controller) {
        LogLine(L"Restoring patches and captured game state...");
        g_controller->RestoreAll();
        g_controller.reset();
    }
    ClearApplied(g_state);
    ClearSupport(g_state);
    InterlockedExchange(&g_mainInitialized, FALSE);
    LogLine(L"Main-thread cleanup complete.");
}

LRESULT CALLBACK TrainerWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kTrainerTickMessage) {
        MainThreadTick();
        return 0;
    }

    if (message == kTrainerCleanupMessage) {
        MainThreadCleanup();

        WNDPROC original = g_originalWndProc;
        if (original && hwnd == g_gameWindow) {
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
            InterlockedExchange(&g_subclassInstalled, FALSE);
            g_originalWndProc = nullptr;
        }
        return 0;
    }

    WNDPROC original = g_originalWndProc;
    return original ? CallWindowProcW(original, hwnd, message, wParam, lParam)
                    : DefWindowProcW(hwnd, message, wParam, lParam);
}

bool InstallMainThreadBridge(SharedState* state) {
    HWND window = nullptr;
    for (int i = 0; i < 200 && InterlockedCompareExchange(&g_running, TRUE, TRUE); ++i) {
        window = FindGameWindow();
        if (window) break;
        Sleep(50);
    }
    if (!window) {
        SetStatus(state, L"Не найдено главное окно Crime Simulator");
        return false;
    }

    SetLastError(0);
    const LONG_PTR previous = SetWindowLongPtrW(
        window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&TrainerWndProc));
    if (previous == 0) {
        SetStatus(state, L"Не удалось подключиться к main thread окна игры");
        return false;
    }

    g_gameWindow = window;
    g_originalWndProc = reinterpret_cast<WNDPROC>(previous);
    InterlockedExchange(&g_subclassInstalled, TRUE);
    LogLine(L"Unity window subclass installed; game API calls are now marshalled to this thread.");
    return true;
}

bool CleanupMainThreadBridge(SharedState* state) {
    if (!InterlockedCompareExchange(&g_subclassInstalled, TRUE, TRUE)) {
        return true;
    }

    if (!g_gameWindow || !IsWindow(g_gameWindow)) return true;

    DWORD_PTR result = 0;
    const LRESULT sent = SendMessageTimeoutW(
        g_gameWindow,
        kTrainerCleanupMessage,
        0,
        0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK,
        5000,
        &result);

    if (!sent) {
        SetStatus(state, L"Не удалось безопасно выгрузить hook: main thread игры занят");
        LogLine(L"ERROR: main-thread cleanup timed out; DLL was NOT unloaded.");
        return false;
    }
    return true;
}

DWORD WorkerBody() {
    const DWORD pid = GetCurrentProcessId();
    const std::wstring mappingName = crime_trainer::MappingName(pid);

    HANDLE mapping = nullptr;
    while (InterlockedCompareExchange(&g_running, TRUE, TRUE)) {
        mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName.c_str());
        if (mapping) break;
        Sleep(100);
    }
    if (!mapping) return kWorkerNormalExit;

    auto* state = static_cast<SharedState*>(MapViewOfFile(
        mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!state) {
        CloseHandle(mapping);
        return kWorkerNormalExit;
    }
    g_state = state;

    auto closeIpc = [&]() {
        g_state = nullptr;
        UnmapViewOfFile(state);
        CloseHandle(mapping);
    };

    if (state->magic != crime_trainer::kSharedMagic ||
        state->version != crime_trainer::kSharedVersion) {
        // Специально молча. Всё, что после magic и version, лежит по чужой
        // разметке, и статус мы бы записали поверх непонятно чего. Лаунчер
        // увидит то же несовпадение у себя. Выгружаемся, чтобы DLL можно
        // было заменить.
        LogLine(L"FATAL: shared state version mismatch; unloading.");
        closeIpc();
        return kWorkerUnloadSelf;
    }

    InterlockedExchange(&state->unloadComplete, FALSE);
    SetStatus(state, L"Ожидание Unity Mono...");

    g_mono = std::make_unique<MonoApi>();
    while (InterlockedCompareExchange(&g_running, TRUE, TRUE) && !g_mono->Initialize()) {
        if (UnloadRequested(state) || LauncherGone(state)) {
            MarkUnloadReady(state);
            g_mono.reset();
            closeIpc();
            return kWorkerUnloadSelf;
        }

        // Если модуля mono ещё нет - просто рано, ждём дальше. А вот если он
        // есть, но в нём не хватает экспорта, он там и не появится, так что
        // нет смысла крутиться вечно с надписью "ожидание Unity".
        if (!g_mono->MissingExport().empty()) {
            const std::string& missing = g_mono->MissingExport();
            const std::wstring wide(missing.begin(), missing.end());
            LogLine(L"FATAL: Mono export not found: " + wide);
            SetStatus(state, (L"Mono API несовместим: нет " + wide).c_str());
            InterlockedExchange(&state->hookReady, FALSE);
            // Мы ещё ничего не ставили: ни subclass, ни патчей. Выгружаться
            // безопасно, игра останется как была. Статус переживёт выгрузку,
            // потому что общая память принадлежит лаунчеру.
            g_mono.reset();
            closeIpc();
            return kWorkerUnloadSelf;
        }

        PublishHookHeartbeat(state);
        Sleep(250);
    }

    if (!g_mono->IsReady()) {
        // Сюда попадаем при выгрузке процесса, а не из-за настоящей ошибки.
        SetStatus(state, L"Mono runtime не найден");
        g_mono.reset();
        closeIpc();
        return kWorkerNormalExit;
    }

    SetStatus(state, L"Mono найден • подключение к Unity main thread...");
    if (!InstallMainThreadBridge(state)) {
        // Subclass не встал, значит ни один WndProc на нас не смотрит и
        // выгружаться безопасно.
        InterlockedExchange(&state->hookReady, FALSE);
        g_mono.reset();
        closeIpc();
        return kWorkerUnloadSelf;
    }

    InterlockedExchange(&state->hookReady, TRUE);
    SetStatus(state, L"Hook загружен • ожидание игрового мира...");

    DWORD result = kWorkerNormalExit;
    ULONGLONG nextPost = 0;

    while (InterlockedCompareExchange(&g_running, TRUE, TRUE)) {
        PublishHookHeartbeat(state);

        if (UnloadRequested(state) || LauncherGone(state)) {
            if (CleanupMainThreadBridge(state)) {
                MarkUnloadReady(state);
                result = kWorkerUnloadSelf;
                break;
            }
            // Не выгружаем DLL, пока её WndProc может быть ещё жив.
            Sleep(250);
            continue;
        }

        // Unity умеет пересоздавать своё окно при смене режима экрана. Старый
        // HWND и снятый с него WndProc после этого мусор, так что цепляемся
        // к новому окну, а не шлём тики в никуда до конца сессии.
        if (g_gameWindow && !IsWindow(g_gameWindow)) {
            LogLine(L"Game window was recreated; re-attaching the main-thread bridge.");
            InterlockedExchange(&g_subclassInstalled, FALSE);
            InterlockedExchange(&g_tickPending, FALSE);
            InterlockedExchange(&state->hookReady, FALSE);
            g_gameWindow = nullptr;
            g_originalWndProc = nullptr;

            // Контроллер специально не трогаем. Его деструктор зовёт RestoreAll,
            // то есть Mono, а мы сейчас в worker-потоке - ровно то, ради чего
            // вся эта схема с главным потоком и затевалась. Картинки и классы
            // переживают смену окна, а мёртвые снимки уйдут по слабым хэндлам.
            ClearApplied(state);
            ClearSupport(state);
            ResetTickState();

            SetStatus(state, L"Окно игры пересоздано • переподключение...");
            if (!InstallMainThreadBridge(state)) {
                result = kWorkerNormalExit;
                break;
            }
            InterlockedExchange(&state->hookReady, TRUE);
            continue;
        }

        const ULONGLONG now = GetTickCount64();
        const bool pending =
            InterlockedCompareExchange(&g_tickPending, TRUE, TRUE) != FALSE;

        if (now >= nextPost && !pending && g_gameWindow) {
            InterlockedExchange(&g_tickPending, TRUE);
            if (PostMessageW(g_gameWindow, kTrainerTickMessage, 0, 0)) {
                nextPost = now + 33; // ~30 Hz bridge; heavy work is internally rate-limited.
            } else {
                InterlockedExchange(&g_tickPending, FALSE);
                nextPost = now + 250;
            }
        }

        Sleep(10);
    }

    if (result != kWorkerUnloadSelf) {
        CleanupMainThreadBridge(state);
        ClearApplied(state);
        InterlockedExchange(&state->hookReady, FALSE);
    }

    g_mono.reset();
    closeIpc();
    return result;
}

DWORD WINAPI WorkerThread(void* parameter) {
    g_module = static_cast<HMODULE>(parameter);
    LogLine(L"CrimeTrainerHook loaded.");
    const DWORD result = WorkerBody();

    if (result == kWorkerUnloadSelf) {
        LogLine(L"Graceful unload complete; leaving Crime Simulator running.");
        FreeLibraryAndExitThread(g_module, 0);
    }
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        g_module = module;
        HANDLE thread = CreateThread(nullptr, 0, WorkerThread, module, 0, nullptr);
        if (thread) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&g_running, FALSE);
    }
    return TRUE;
}
