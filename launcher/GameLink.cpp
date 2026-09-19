#include "GameLink.h"

#include <TlHelp32.h>
#include <array>
#include <filesystem>

namespace crime_trainer {
namespace {

constexpr ULONGLONG kUnloadTimeoutMs = 8000;
constexpr ULONGLONG kConnectRetryMs = 4000;
constexpr DWORD kInjectionWaitMs = 5000;

std::filesystem::path ExecutableDirectory() {
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}

bool QueryModuleLoaded(DWORD pid, const wchar_t* moduleName, bool& loaded) {
    loaded = false;

    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 8; ++attempt) {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_BAD_LENGTH) return false;
        Sleep(10);
    }
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    bool enumerated = false;
    if (Module32FirstW(snapshot, &entry)) {
        enumerated = true;
        do {
            if (_wcsicmp(entry.szModule, moduleName) == 0) {
                loaded = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return enumerated;
}

bool IsModuleLoaded(DWORD pid, const wchar_t* moduleName) {
    bool loaded = false;
    return QueryModuleLoaded(pid, moduleName, loaded) && loaded;
}

} // namespace

FeatureState SharedState::* FeatureMember(int index) {
    static constexpr std::array<FeatureState SharedState::*, kFeatureSlots> kMembers{
        &SharedState::freezeTime,   &SharedState::ignorePolice, &SharedState::ignoreCivilians,
        &SharedState::ignoreCameras, &SharedState::ignoreSensors, &SharedState::infiniteAmmo,
        &SharedState::unbreakableItems,
    };
    if (index < 0 || index >= static_cast<int>(kMembers.size())) return nullptr;
    return kMembers[static_cast<std::size_t>(index)];
}

DWORD FindProcessId(const wchar_t* executableName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD pid = 0;

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, executableName) == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return pid;
}

bool ProcessAlive(DWORD pid) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return false;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

GameLink::~GameLink() {
    Disconnect();
}

// Пока поток загрузчика не завершился, он всё ещё держит свой аргумент, так
// что освобождать память рано. В старом коде это было написано в комментарии,
// но сама функция не вызывалась ни разу - утекали два хэндла и страница в игре.
bool GameLink::FinishPendingInjection() {
    if (!injectionThread_) return true;
    if (WaitForSingleObject(injectionThread_, 0) != WAIT_OBJECT_0) return false;

    DWORD exitCode = 0;
    const bool loaded = GetExitCodeThread(injectionThread_, &exitCode) && exitCode != 0;

    VirtualFreeEx(injectionProcess_, injectionArgument_, 0, MEM_RELEASE);
    CloseHandle(injectionThread_);
    CloseHandle(injectionProcess_);
    injectionThread_ = nullptr;
    injectionProcess_ = nullptr;
    injectionArgument_ = nullptr;

    if (!loaded) {
        lastError_ = L"Не удалось загрузить hook DLL; перезапусти трейнер";
        if (state_) wcsncpy_s(state_->status, lastError_.c_str(), _TRUNCATE);
    }
    return true;
}

bool GameLink::InjectLibrary(DWORD pid, const std::wstring& dllPath, std::wstring& error) {
    if (!std::filesystem::exists(dllPath)) {
        error = L"Рядом с EXE отсутствует CrimeTrainerHook.dll";
        return false;
    }

    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);

    if (!process) {
        error = L"Нет доступа к процессу игры. Запусти трейнер от имени администратора.";
        return false;
    }

    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        error = L"Не удалось выделить память в процессе игры";
        CloseHandle(process);
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remote, dllPath.c_str(), bytes, &written) || written != bytes) {
        error = L"Не удалось передать hook DLL в процесс игры";
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel, "LoadLibraryW"));
    if (!loadLibrary) {
        error = L"LoadLibraryW недоступна";
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (!thread) {
        error = L"Windows заблокировала загрузку hook DLL";
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    if (WaitForSingleObject(thread, kInjectionWaitMs) != WAIT_OBJECT_0) {
        // Медленный загрузчик - это ещё не ошибка. Отдаём владение циклу опроса,
        // он освободит память, когда поток реально закончит.
        injectionProcess_ = process;
        injectionThread_ = thread;
        injectionArgument_ = remote;
        return true;
    }

    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);

    if (exitCode == 0) {
        error = L"Hook DLL не загрузилась в Crime Simulator";
        return false;
    }
    return true;
}

bool GameLink::Connect(DWORD pid, std::wstring& error) {
    Disconnect();
    pid_ = pid;

    const std::wstring mappingName = MappingName(pid);
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                  sizeof(SharedState), mappingName.c_str());
    if (!mapping_) {
        error = L"Не удалось создать IPC-канал";
        Disconnect();
        return false;
    }

    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    state_ = static_cast<SharedState*>(
        MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!state_) {
        error = L"Не удалось подключить IPC-канал";
        Disconnect();
        return false;
    }

    if (existed && (state_->magic != kSharedMagic || state_->version != kSharedVersion)) {
        // Нельзя чистить mapping, который может принадлежать живой чужой DLL.
        error = L"Несовместимая версия IPC; закрой старый трейнер и перезапусти игру";
        Disconnect();
        return false;
    }

    if (!existed) {
        ZeroMemory(state_, sizeof(*state_));
        state_->magic = kSharedMagic;
        state_->version = kSharedVersion;
        state_->hour = -1;
        state_->minute = -1;
        wcscpy_s(state_->status, L"Подготовка hook...");
    }

    InterlockedExchange(&state_->launcherAlive, TRUE);
    InterlockedExchange(&state_->unloadRequested, FALSE);
    InterlockedExchange(&state_->unloadComplete, FALSE);
    PublishHeartbeat();

    if (!IsModuleLoaded(pid, kHookDllName)) {
        const std::wstring hookPath = (ExecutableDirectory() / kHookDllName).wstring();
        if (!InjectLibrary(pid, hookPath, error)) {
            Disconnect();
            return false;
        }
    }
    return true;
}

void GameLink::Disconnect() {
    FinishPendingInjection();
    if (injectionThread_) {
        // Сюда обычно попадаем, только когда игра уже вышла. Оставить память
        // выделенной безопаснее, чем освободить её из-под LoadLibraryW.
        CloseHandle(injectionThread_);
        CloseHandle(injectionProcess_);
        injectionThread_ = nullptr;
        injectionProcess_ = nullptr;
        injectionArgument_ = nullptr;
    }
    if (state_) {
        ClearAllRequests();
        InterlockedExchange(&state_->launcherAlive, FALSE);
        UnmapViewOfFile(state_);
        state_ = nullptr;
    }
    if (mapping_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
    pid_ = 0;
    unloadDeadline_ = 0;
}

void GameLink::PublishHeartbeat() {
    if (!state_) return;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&state_->launcherHeartbeat),
                        static_cast<LONG>(GetTickCount()));
}

bool GameLink::Poll(std::wstring& announcement) {
    PublishHeartbeat();
    FinishPendingInjection();

    const auto report = [&](std::wstring text) {
        if (text == lastAnnouncement_) return;
        lastAnnouncement_ = text;
        announcement = std::move(text);
    };

    if (pid_ && !ProcessAlive(pid_)) {
        Disconnect();
        report(L"Crime Simulator закрыта. Подключение завершено.");
        return true;
    }

    if (pid_) return false;

    const ULONGLONG now = GetTickCount64();
    if (now < nextConnectAttempt_) return false;

    const DWORD pid = FindProcessId(kGameProcessName);
    if (!pid) return false;

    std::wstring error;
    if (Connect(pid, error)) {
        lastError_.clear();
        nextConnectAttempt_ = 0;
        report(L"Crime Simulator обнаружена. Трейнер подключается.");
        return true;
    }

    // Если открыть процесс не вышло (права, антивирус), не долбимся дважды
    // в секунду, а ждём.
    lastError_ = error;
    nextConnectAttempt_ = now + kConnectRetryMs;
    report(error);
    return true;
}

bool GameLink::HookReady() const {
    return pid_ != 0 && state_ &&
           InterlockedCompareExchange(&state_->hookReady, TRUE, TRUE) != FALSE;
}

bool GameLink::FeatureRequested(int index) const {
    auto member = FeatureMember(index);
    if (!state_ || !member) return false;
    return InterlockedCompareExchange(&(state_->*member).requested, TRUE, TRUE) != FALSE;
}

bool GameLink::FeatureApplied(int index) const {
    auto member = FeatureMember(index);
    if (!HookReady() || !member) return false;
    return InterlockedCompareExchange(&(state_->*member).applied, TRUE, TRUE) != FALSE;
}

LONG GameLink::FeatureSupport(int index) const {
    auto member = FeatureMember(index);
    if (!HookReady() || !member) return kSupportUnknown;
    return InterlockedCompareExchange(&(state_->*member).support, 0, 0);
}

void GameLink::SetFeatureRequested(int index, bool enabled) {
    auto member = FeatureMember(index);
    if (!state_ || !member) return;
    InterlockedExchange(&(state_->*member).requested, enabled ? TRUE : FALSE);
}

void GameLink::ClearAllRequests() {
    if (!state_) return;
    for (int i = 0; i < kFeatureSlots; ++i) SetFeatureRequested(i, false);
}

int GameLink::Hour() const {
    if (!HookReady()) return -1;
    return static_cast<int>(InterlockedCompareExchange(&state_->hour, 0, 0));
}

int GameLink::Minute() const {
    if (!HookReady()) return -1;
    return static_cast<int>(InterlockedCompareExchange(&state_->minute, 0, 0));
}

long GameLink::ResolvedPatches() const {
    if (!state_) return 0;
    return static_cast<long>(InterlockedCompareExchange(&state_->resolvedPatches, 0, 0));
}

std::wstring GameLink::Status() const {
    if (!state_) return {};
    // Hook пишет сюда через wcsncpy_s(_TRUNCATE), так что ноль там есть. Но
    // общей памяти на слово верить не хочется, поэтому ограничиваю длину сам.
    const std::size_t limit = std::size(state_->status);
    std::size_t length = 0;
    while (length < limit && state_->status[length] != L'\0') ++length;
    return std::wstring(state_->status, length);
}

void GameLink::BeginUnload() {
    if (!pid_ || !state_ || unloadDeadline_ != 0) return;

    bool loaded = false;
    if (QueryModuleLoaded(pid_, kHookDllName, loaded) && !loaded) {
        unloadDeadline_ = 0;
        return;
    }

    ClearAllRequests();
    InterlockedExchange(&state_->launcherAlive, TRUE);
    InterlockedExchange(&state_->unloadComplete, FALSE);
    InterlockedExchange(&state_->unloadRequested, TRUE);
    wcsncpy_s(state_->status, L"Восстановление состояний и выгрузка hook...", _TRUNCATE);
    unloadDeadline_ = GetTickCount64() + kUnloadTimeoutMs;
}

GameLink::Unload GameLink::PollUnload() {
    if (!pid_ || !state_) return Unload::Done;
    if (unloadDeadline_ == 0) return Unload::Done;

    PublishHeartbeat();
    if (!ProcessAlive(pid_)) {
        unloadDeadline_ = 0;
        return Unload::Done;
    }

    // Сначала дожидаемся загрузчика: выгружать DLL из-под незавершённой
    // LoadLibraryW - верный способ получить полузагруженный hook.
    if (!FinishPendingInjection()) {
        return GetTickCount64() >= unloadDeadline_ ? Unload::Failed : Unload::Pending;
    }

    bool stillLoaded = true;
    if (QueryModuleLoaded(pid_, kHookDllName, stillLoaded) && !stillLoaded) {
        unloadDeadline_ = 0;
        return Unload::Done;
    }

    if (GetTickCount64() >= unloadDeadline_) {
        unloadDeadline_ = 0;
        return Unload::Failed;
    }
    return Unload::Pending;
}

} // namespace crime_trainer
