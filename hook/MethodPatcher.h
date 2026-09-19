#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "MonoApi.h"

class MethodPatcher final {
public:
    explicit MethodPatcher(MonoApi& mono) : mono_(mono) {}
    ~MethodPatcher();

    struct Target final {
        std::string key;
        std::string className;
        std::string methodName;
        int parameterCount{-1}; // -1 = every overload with a supported return type
    };

    bool Resolve(MonoImage* image, const Target& target);
    bool SetEnabled(const std::string& key, bool enabled);
    bool SetEnabledByPrefix(const std::string& prefix, bool enabled);
    bool IsResolved(const std::string& key) const;
    std::size_t ResolvedByPrefix(const std::string& prefix) const;
    std::size_t ResolvedCount() const noexcept { return patches_.size(); }
    void RestoreAll();

    const std::vector<std::wstring>& Diagnostics() const noexcept { return diagnostics_; }

private:
    enum class ReturnMode : std::uint8_t {
        Unsupported,
        Void,
        FalseBoolean,
    };

    struct Patch final {
        std::string key;
        std::string className;
        std::string methodName;
        MonoMethod* method{};
        void* address{};
        ReturnMode mode{ReturnMode::Unsupported};
        // Меняем сразу 8 байт целиком, чтобы поток, который уже выполняет метод,
        // не увидел наполовину записанную инструкцию.
        std::uint64_t originalWord{};
        std::uint64_t patchedWord{};
        std::size_t size{};
        bool enabled{};
    };

    bool ResolveMethod(MonoClass* klass, const Target& target, MonoMethod* method, int overloadIndex);
    ReturnMode ReturnModeFor(MonoMethod* method) const;
    bool WordSwapSafe(const Patch& patch) const;
    bool Write(Patch& patch, bool enabled);
    void Diagnostic(const std::wstring& text);

    MonoApi& mono_;
    std::unordered_map<std::string, Patch> patches_;
    std::vector<std::wstring> diagnostics_;
};
