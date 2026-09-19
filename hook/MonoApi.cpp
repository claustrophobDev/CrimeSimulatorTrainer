#include "MonoApi.h"

namespace {

// Разметка MonoArray на x64.
constexpr std::size_t kArrayMaxLengthOffset = 3 * sizeof(void*); // 0x18
constexpr std::size_t kArrayVectorOffset = 4 * sizeof(void*);    // 0x20

} // namespace

bool MonoApi::Initialize() {
    module_ = GetModuleHandleW(L"mono-2.0-bdwgc.dll");
    if (!module_) module_ = GetModuleHandleW(L"mono.dll");
    if (!module_) return false;

    missingExport_.clear();

    const bool required =
        Require(get_root_domain, "mono_get_root_domain") &&
        Require(thread_attach, "mono_thread_attach") &&
        Require(assembly_foreach, "mono_assembly_foreach") &&
        Require(assembly_get_image, "mono_assembly_get_image") &&
        Require(image_get_name, "mono_image_get_name") &&
        Require(class_from_name, "mono_class_from_name") &&
        Require(object_get_class, "mono_object_get_class") &&
        Require(class_get_name, "mono_class_get_name") &&
        Require(class_get_type, "mono_class_get_type") &&
        Require(type_get_object, "mono_type_get_object") &&
        Require(class_get_field_from_name, "mono_class_get_field_from_name") &&
        Require(field_get_value, "mono_field_get_value") &&
        Require(field_set_value, "mono_field_set_value") &&
        Require(field_static_get_value, "mono_field_static_get_value") &&
        Require(class_get_property_from_name, "mono_class_get_property_from_name") &&
        Require(property_get_get_method, "mono_property_get_get_method") &&
        Require(property_get_set_method, "mono_property_get_set_method") &&
        Require(class_get_method_from_name, "mono_class_get_method_from_name") &&
        Require(class_vtable, "mono_class_vtable") &&
        Require(runtime_invoke, "mono_runtime_invoke") &&
        Require(object_unbox, "mono_object_unbox") &&
        Require(gchandle_new, "mono_gchandle_new") &&
        Require(gchandle_get_target, "mono_gchandle_get_target") &&
        Require(gchandle_free, "mono_gchandle_free");

    if (!required) {
        module_ = nullptr; // IsReady() должен остаться false, чтобы worker продолжил ждать
        return false;
    }

    // Эти два - макросы, их может не быть. Есть запасной путь, так что не фатально.
    Load(array_length, "mono_array_length");
    Load(array_addr_with_size, "mono_array_addr_with_size");

    Load(gchandle_new_weakref, "mono_gchandle_new_weakref");

    // Нужны только патчеру. Если их нет, время/патроны/прочность всё равно работают.
    Load(class_get_methods, "mono_class_get_methods");
    Load(method_get_name, "mono_method_get_name");
    Load(method_signature, "mono_method_signature");
    Load(signature_get_param_count, "mono_signature_get_param_count");
    Load(signature_get_return_type, "mono_signature_get_return_type");
    Load(type_get_type, "mono_type_get_type");
    Load(compile_method, "mono_compile_method");

    return true;
}

std::uintptr_t MonoApi::ArrayLength(MonoArray* array) const {
    if (!array) return 0;
    if (array_length) return array_length(array);
    return *reinterpret_cast<const std::uintptr_t*>(
        reinterpret_cast<const char*>(array) + kArrayMaxLengthOffset);
}

void* MonoApi::ArrayElement(MonoArray* array, std::uintptr_t index, std::size_t elementSize) const {
    if (!array) return nullptr;
    if (array_addr_with_size) {
        return array_addr_with_size(array, static_cast<int>(elementSize), index);
    }
    return reinterpret_cast<char*>(array) + kArrayVectorOffset + index * elementSize;
}

std::uint32_t MonoApi::WeakHandle(MonoObject* object) const {
    if (!object) return 0;
    if (gchandle_new_weakref) return gchandle_new_weakref(object, FALSE);
    return gchandle_new ? gchandle_new(object, FALSE) : 0;
}
