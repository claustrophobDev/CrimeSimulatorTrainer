#pragma once

#include <Windows.h>
#include <string>

#include "../include/SharedState.h"

namespace crime_trainer {

// Всё, что связано с подключением к запущенной игре: поиск процесса, инъекция
// DLL и канал общей памяти.
class GameLink final {
public:
    enum class Unload {
        Done,      // hook'а в процессе больше нет (или не было)
        Pending,   // ещё восстанавливается, спросить позже
        Failed,    // главный поток игры так и не подтвердил очистку
    };

    ~GameLink();

    GameLink(const GameLink&) = delete;
    GameLink& operator=(const GameLink&) = delete;
    GameLink() = default;

    // Ищет игру, если ещё не подключены, инжектит и цепляется.
    // announcement заполняется, только если есть что нового сказать: иначе
    // неудачное подключение спамило бы уведомлением два раза в секунду.
    bool Poll(std::wstring& announcement);

    void Disconnect();

    bool Connected() const noexcept { return pid_ != 0; }
    bool HookReady() const;
    DWORD Pid() const noexcept { return pid_; }
    const std::wstring& LastError() const noexcept { return lastError_; }

    SharedState* State() const noexcept { return state_; }

    bool FeatureRequested(int index) const;
    bool FeatureApplied(int index) const;
    LONG FeatureSupport(int index) const;
    void SetFeatureRequested(int index, bool enabled);
    void ClearAllRequests();

    int Hour() const;
    int Minute() const;
    long ResolvedPatches() const;
    std::wstring Status() const;

    // Выгрузка без блокировки. BeginUnload просит один раз, PollUnload дёргаем
    // из цикла сообщений, чтобы окно не висело несколько секунд, как раньше.
    void BeginUnload();
    Unload PollUnload();
    bool UnloadInProgress() const noexcept { return unloadDeadline_ != 0; }

private:
    bool Connect(DWORD pid, std::wstring& error);
    bool InjectLibrary(DWORD pid, const std::wstring& dllPath, std::wstring& error);
    bool FinishPendingInjection();
    void PublishHeartbeat();

    DWORD pid_ = 0;
    HANDLE mapping_ = nullptr;
    SharedState* state_ = nullptr;
    std::wstring lastError_;

    HANDLE injectionProcess_ = nullptr;
    HANDLE injectionThread_ = nullptr;
    void* injectionArgument_ = nullptr;

    ULONGLONG unloadDeadline_ = 0;
    ULONGLONG nextConnectAttempt_ = 0;
    std::wstring lastAnnouncement_;
};

FeatureState SharedState::* FeatureMember(int index);
DWORD FindProcessId(const wchar_t* executableName);
bool ProcessAlive(DWORD pid);

} // namespace crime_trainer
