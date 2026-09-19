#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>

struct _MonoDomain;
struct _MonoThread;
struct _MonoAssembly;
struct _MonoImage;
struct _MonoClass;
struct _MonoObject;
struct _MonoClassField;
struct _MonoProperty;
struct _MonoMethod;
struct _MonoVTable;
struct _MonoType;
struct _MonoArray;
struct _MonoMethodSignature;

using MonoDomain = _MonoDomain;
using MonoThread = _MonoThread;
using MonoAssembly = _MonoAssembly;
using MonoImage = _MonoImage;
using MonoClass = _MonoClass;
using MonoObject = _MonoObject;
using MonoClassField = _MonoClassField;
using MonoProperty = _MonoProperty;
using MonoMethod = _MonoMethod;
using MonoVTable = _MonoVTable;
using MonoType = _MonoType;
using MonoArray = _MonoArray;
using MonoMethodSignature = _MonoMethodSignature;

class MonoApi final {
public:
    using AssemblyForeachCallback = void(__cdecl*)(MonoAssembly*, void*);

    bool Initialize();
    bool IsReady() const noexcept { return module_ != nullptr; }

    // Какой обязательный экспорт не нашёлся. Пусто, если всё ок.
    // Без этого любая проблема выглядела как "Mono runtime не найден".
    const std::string& MissingExport() const noexcept { return missingExport_; }

    // mono_array_length и mono_array_addr_with_size в заголовках Mono - макросы,
    // и экспортируются не в каждой сборке Unity. Если экспорта нет, читаем
    // MonoArray напрямую, разметка не менялась никогда:
    // 0x00 vtable, 0x08 sync, 0x10 bounds, 0x18 max_length, 0x20 первый элемент.
    std::uintptr_t ArrayLength(MonoArray* array) const;
    void* ArrayElement(MonoArray* array, std::uintptr_t index, std::size_t elementSize) const;

    // Слабый хэндл. Сильный держал бы уничтоженные объекты Unity живыми до
    // конца сессии, поэтому он только запасной вариант.
    std::uint32_t WeakHandle(MonoObject* object) const;
    bool UsesStrongHandles() const noexcept { return gchandle_new_weakref == nullptr; }

    MonoDomain* (__cdecl* get_root_domain)() = nullptr;
    MonoThread* (__cdecl* thread_attach)(MonoDomain*) = nullptr;

    void (__cdecl* assembly_foreach)(AssemblyForeachCallback, void*) = nullptr;
    MonoImage* (__cdecl* assembly_get_image)(MonoAssembly*) = nullptr;
    const char* (__cdecl* image_get_name)(MonoImage*) = nullptr;

    MonoClass* (__cdecl* class_from_name)(MonoImage*, const char*, const char*) = nullptr;
    MonoClass* (__cdecl* object_get_class)(MonoObject*) = nullptr;
    const char* (__cdecl* class_get_name)(MonoClass*) = nullptr;
    MonoType* (__cdecl* class_get_type)(MonoClass*) = nullptr;
    MonoObject* (__cdecl* type_get_object)(MonoDomain*, MonoType*) = nullptr;

    MonoClassField* (__cdecl* class_get_field_from_name)(MonoClass*, const char*) = nullptr;
    void (__cdecl* field_get_value)(MonoObject*, MonoClassField*, void*) = nullptr;
    void (__cdecl* field_set_value)(MonoObject*, MonoClassField*, void*) = nullptr;
    void (__cdecl* field_static_get_value)(MonoVTable*, MonoClassField*, void*) = nullptr;

    MonoProperty* (__cdecl* class_get_property_from_name)(MonoClass*, const char*) = nullptr;
    MonoMethod* (__cdecl* property_get_get_method)(MonoProperty*) = nullptr;
    MonoMethod* (__cdecl* property_get_set_method)(MonoProperty*) = nullptr;
    MonoMethod* (__cdecl* class_get_method_from_name)(MonoClass*, const char*, int) = nullptr;
    MonoMethod* (__cdecl* class_get_methods)(MonoClass*, void**) = nullptr;
    const char* (__cdecl* method_get_name)(MonoMethod*) = nullptr;
    MonoMethodSignature* (__cdecl* method_signature)(MonoMethod*) = nullptr;
    std::uint32_t (__cdecl* signature_get_param_count)(MonoMethodSignature*) = nullptr;
    MonoType* (__cdecl* signature_get_return_type)(MonoMethodSignature*) = nullptr;
    int (__cdecl* type_get_type)(MonoType*) = nullptr;
    void* (__cdecl* compile_method)(MonoMethod*) = nullptr;

    MonoVTable* (__cdecl* class_vtable)(MonoDomain*, MonoClass*) = nullptr;
    MonoObject* (__cdecl* runtime_invoke)(MonoMethod*, void*, void**, MonoObject**) = nullptr;
    void* (__cdecl* object_unbox)(MonoObject*) = nullptr;

    std::uintptr_t (__cdecl* array_length)(MonoArray*) = nullptr;
    char* (__cdecl* array_addr_with_size)(MonoArray*, int, std::uintptr_t) = nullptr;

    std::uint32_t (__cdecl* gchandle_new)(MonoObject*, int) = nullptr;
    std::uint32_t (__cdecl* gchandle_new_weakref)(MonoObject*, int) = nullptr;
    MonoObject* (__cdecl* gchandle_get_target)(std::uint32_t) = nullptr;
    void (__cdecl* gchandle_free)(std::uint32_t) = nullptr;

private:
    template <typename T>
    bool Load(T& target, const char* name) {
        target = reinterpret_cast<T>(GetProcAddress(module_, name));
        return target != nullptr;
    }

    // Запоминаем первую неудачу, чтобы потом сказать, чего именно не хватило.
    template <typename T>
    bool Require(T& target, const char* name) {
        if (Load(target, name)) return true;
        if (missingExport_.empty()) missingExport_ = name;
        return false;
    }

    HMODULE module_ = nullptr;
    std::string missingExport_;
};
