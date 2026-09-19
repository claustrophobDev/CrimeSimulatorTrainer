#include "MethodPatcher.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace {

constexpr int kMonoTypeVoid = 0x01;
constexpr int kMonoTypeBoolean = 0x02;

std::wstring Widen(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (length <= 1) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), length);
    result.resize(static_cast<std::size_t>(length - 1));
    return result;
}

bool IsReactionSuppressionKey(const std::string& key) {
    static constexpr const char* kPrefixes[] = {
        "police.", "civilian.", "npcboth.", "camera.", "sensor."
    };
    for (const char* prefix : kPrefixes) {
        if (key.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

bool IsProtectedDamageOrDeathMethod(const std::string& methodName) {
    // Урон и смерть функции игнора не трогают никогда. Список намеренно
    // перестраховочный: если в новой сборке появится такой метод, подавление
    // реакций обязано его пропустить, иначе NPC станут бессмертными.
    static constexpr const char* kProtected[] = {
        "RPC_DealDamage", "DealDamage", "TakeDamage", "ApplyDamage",
        "ReceiveDamage", "Damage", "Hit", "OnHit", "Die", "Death",
        "OnDeath", "Kill", "Killed", "OnKilled"
    };
    for (const char* protectedName : kProtected) {
        if (_stricmp(methodName.c_str(), protectedName) == 0) return true;
    }
    return false;
}

} // namespace

MethodPatcher::~MethodPatcher() {
    RestoreAll();
}

void MethodPatcher::Diagnostic(const std::wstring& text) {
    diagnostics_.push_back(text);
}

MethodPatcher::ReturnMode MethodPatcher::ReturnModeFor(MonoMethod* method) const {
    if (!method || !mono_.method_signature || !mono_.signature_get_return_type || !mono_.type_get_type) {
        return ReturnMode::Unsupported;
    }

    MonoMethodSignature* signature = mono_.method_signature(method);
    MonoType* returnType = signature ? mono_.signature_get_return_type(signature) : nullptr;
    if (!returnType) return ReturnMode::Unsupported;

    const int type = mono_.type_get_type(returnType);
    if (type == kMonoTypeVoid) return ReturnMode::Void;
    if (type == kMonoTypeBoolean) return ReturnMode::FalseBoolean;
    return ReturnMode::Unsupported;
}

bool MethodPatcher::ResolveMethod(
    MonoClass* klass,
    const Target& target,
    MonoMethod* method,
    int overloadIndex) {
    if (!klass || !method) return false;

    const ReturnMode mode = ReturnModeFor(method);
    if (mode == ReturnMode::Unsupported) return false;

    void* address = mono_.compile_method ? mono_.compile_method(method) : nullptr;
    if (!address) return false;

    Patch patch{};
    patch.key = target.key;
    if (overloadIndex > 0) patch.key += "#" + std::to_string(overloadIndex);

    // Если ключ уже есть, второй раз не регистрируем: сейчас по этому адресу
    // может лежать заглушка, и мы запомнили бы её как "оригинал".
    if (patches_.find(patch.key) != patches_.end()) return true;

    patch.className = target.className;
    patch.methodName = target.methodName;
    patch.method = method;
    patch.address = address;
    patch.mode = mode;

    std::uint8_t stub[sizeof(std::uint64_t)]{};
    if (mode == ReturnMode::Void) {
        patch.size = 1;
        stub[0] = 0xC3; // ret
    } else if (mode == ReturnMode::FalseBoolean) {
        patch.size = 3;
        stub[0] = 0x31; // xor eax,eax
        stub[1] = 0xC0;
        stub[2] = 0xC3; // ret
    }

    // Оба слова покрывают одни и те же 8 байт, поэтому включение и выключение -
    // это одна запись, а не побайтовый memcpy по живому коду.
    std::memcpy(&patch.originalWord, address, sizeof(patch.originalWord));
    patch.patchedWord = patch.originalWord;
    std::memcpy(&patch.patchedWord, stub, patch.size);

    patches_.insert_or_assign(patch.key, patch);

    std::wstringstream message;
    message << L"[OK] " << Widen(patch.className) << L"::" << Widen(patch.methodName);
    if (overloadIndex > 0) message << L" #" << overloadIndex;
    Diagnostic(message.str());
    return true;
}

bool MethodPatcher::Resolve(MonoImage* image, const Target& target) {
    if (!image) return false;

    if (IsReactionSuppressionKey(target.key) && IsProtectedDamageOrDeathMethod(target.methodName)) {
        Diagnostic(L"[SKIP-PROTECTED] damage/death path left intact: " +
            Widen(target.className) + L"::" + Widen(target.methodName));
        return false;
    }

    MonoClass* klass = mono_.class_from_name(image, "", target.className.c_str());
    if (!klass) {
        Diagnostic(L"[SKIP] class " + Widen(target.className) + L" not found");
        return false;
    }

    bool any = false;
    int overloadIndex = 0;

    if (target.parameterCount >= 0) {
        MonoMethod* method = mono_.class_get_method_from_name(
            klass, target.methodName.c_str(), target.parameterCount);
        if (!method) {
            Diagnostic(L"[SKIP] " + Widen(target.className) + L"::" +
                Widen(target.methodName) + L" not found");
            return false;
        }
        any = ResolveMethod(klass, target, method, 0);
        if (!any) {
            Diagnostic(L"[SKIP] unsupported return: " + Widen(target.className) + L"::" +
                Widen(target.methodName));
        }
        return any;
    }

    if (!mono_.class_get_methods || !mono_.method_get_name ||
        !mono_.method_signature || !mono_.signature_get_param_count) {
        Diagnostic(L"[SKIP] Mono method enumeration API unavailable");
        return false;
    }

    void* iter = nullptr;
    while (MonoMethod* method = mono_.class_get_methods(klass, &iter)) {
        const char* name = mono_.method_get_name(method);
        if (!name || target.methodName != name) continue;

        MonoMethodSignature* signature = mono_.method_signature(method);
        const std::uint32_t params = signature ? mono_.signature_get_param_count(signature) : 0;
        Target concrete = target;
        concrete.parameterCount = static_cast<int>(params);
        ++overloadIndex;
        if (ResolveMethod(klass, concrete, method, overloadIndex)) any = true;
    }

    if (!any) {
        Diagnostic(L"[SKIP] " + Widen(target.className) + L"::" +
            Widen(target.methodName) + L" not found/supported");
    }
    return any;
}

bool MethodPatcher::WordSwapSafe(const Patch& patch) const {
    // Запись переписывает все 8 байт, хотя меняются только size из них -
    // остальные возвращаются из того, что мы запомнили. Это верно, только пока
    // в эти же 8 байт не попадает начало другого пропатченного метода.
    const auto base = reinterpret_cast<std::uintptr_t>(patch.address);
    if (base % sizeof(std::uint64_t) != 0) return false;

    for (const auto& [key, other] : patches_) {
        (void)key;
        if (other.address == patch.address || !other.address) continue;
        const auto candidate = reinterpret_cast<std::uintptr_t>(other.address);
        if (candidate > base && candidate < base + sizeof(std::uint64_t)) return false;
    }
    return true;
}

bool MethodPatcher::Write(Patch& patch, bool enabled) {
    if (!patch.address || patch.size == 0) return false;
    if (patch.enabled == enabled) return true;

    constexpr std::size_t kWord = sizeof(std::uint64_t);

    DWORD oldProtect = 0;
    if (!VirtualProtect(patch.address, kWord, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    const std::uint64_t value = enabled ? patch.patchedWord : patch.originalWord;

    // В момент переключения поток игры может находиться внутри метода.
    // Выровненная 8-байтовая запись видна другим ядрам целиком, поэтому метод
    // либо полностью оригинальный, либо полностью заглушка - промежуточного
    // состояния, как при memcpy, не бывает.
    if (WordSwapSafe(patch)) {
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(patch.address),
            static_cast<LONG64>(value));
    } else {
        std::memcpy(patch.address, &value, patch.size);
    }

    FlushInstructionCache(GetCurrentProcess(), patch.address, kWord);

    DWORD ignored = 0;
    VirtualProtect(patch.address, kWord, oldProtect, &ignored);
    patch.enabled = enabled;
    return true;
}

bool MethodPatcher::SetEnabled(const std::string& key, bool enabled) {
    bool found = false;
    bool ok = true;
    for (auto& [patchKey, patch] : patches_) {
        if (patchKey == key || patchKey.rfind(key + "#", 0) == 0) {
            found = true;
            ok = Write(patch, enabled) && ok;
        }
    }
    return found && ok;
}

bool MethodPatcher::SetEnabledByPrefix(const std::string& prefix, bool enabled) {
    bool found = false;
    bool ok = true;
    for (auto& [key, patch] : patches_) {
        if (key.rfind(prefix, 0) == 0) {
            found = true;
            ok = Write(patch, enabled) && ok;
        }
    }
    return found && ok;
}

bool MethodPatcher::IsResolved(const std::string& key) const {
    for (const auto& [patchKey, patch] : patches_) {
        (void)patch;
        if (patchKey == key || patchKey.rfind(key + "#", 0) == 0) return true;
    }
    return false;
}

std::size_t MethodPatcher::ResolvedByPrefix(const std::string& prefix) const {
    std::size_t count = 0;
    for (const auto& [key, patch] : patches_) {
        (void)patch;
        if (key.rfind(prefix, 0) == 0) ++count;
    }
    return count;
}

void MethodPatcher::RestoreAll() {
    for (auto& [key, patch] : patches_) {
        (void)key;
        if (patch.enabled) Write(patch, false);
    }
}
