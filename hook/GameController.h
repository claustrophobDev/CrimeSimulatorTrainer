#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "MethodPatcher.h"
#include "MonoApi.h"

// Патроны докидываем до этого значения. Совсем огромные числа игра
// показывает криво в инвентаре, поэтому не наглеем.
inline constexpr int kAmmoTopUp = 999;

class GameController final {
public:
    enum class OpResult {
        Applied,     // сработало
        Waiting,     // нужен мир или игрок, которых пока нет
        Unsupported, // в этой сборке игры такого нет
    };

    explicit GameController(MonoApi& mono) : mono_(mono), patcher_(mono) {}
    ~GameController();

    bool Initialize();
    const std::wstring& LastError() const noexcept { return lastError_; }
    const std::vector<std::wstring>& PatchDiagnostics() const noexcept {
        return patcher_.Diagnostics();
    }
    std::size_t ResolvedPatchCount() const noexcept { return patcher_.ResolvedCount(); }

    // Всё ниже - только с главного потока Unity.
    OpResult SetFreezeTime(bool on);
    OpResult SetIgnorePolice(bool on);
    OpResult SetIgnoreCivilians(bool on);
    OpResult SetIgnoreCameras(bool on);
    OpResult SetIgnoreSensors(bool on);
    OpResult SetInfiniteAmmo(bool on);
    OpResult SetUnbreakableItems(bool on);
    OpResult SetSilentSteps(bool on);
    OpResult SetInfiniteStamina(bool on);
    OpResult SetNoDamage(bool on);
    bool GetClock(int& hour, int& minute);

    void RestoreAll();

private:
    struct ImageLookupContext {
        MonoApi* mono;
        const char* wanted;
        MonoImage* image;
    };

    // Запоминаем только спокойные поля-опции. seesPlayer, anger, callingCops и
    // прочий FSM трейнер не трогает - с ними игра разбирается сама.
    struct NpcSnapshot {
        std::uint32_t handle{};
        bool blind{};
        bool deaf{};
        bool ignoreEverything{};
        bool dontNoticeStolenItems{};
        bool dontNoticeDeadPeople{};
        bool dontNoticeOpenDoors{};
    };

    struct CameraSnapshot {
        std::uint32_t handle{};
        bool disabled{};
    };

    struct AmmoSnapshot {
        std::uint32_t handle{};
        int amount{};
    };

    struct SlotDamageSnapshot {
        std::uint32_t handle{};
        std::vector<float> damages;
    };

    // В Unity mono сборщик мусора не двигает объекты, поэтому адрес живёт
    // столько же, сколько сам объект, и годится как ключ. А то, что объект
    // ещё жив, проверяем по слабому хэндлу - адрес могли переиспользовать.
    template <typename T>
    using SnapshotMap = std::unordered_map<MonoObject*, T>;

    static void __cdecl ImageLookup(MonoAssembly* assembly, void* userData);

    MonoImage* FindImage(const char* imageName);
    MonoClass* GameClass(const char* className);
    MonoObject* ResolveWorld();
    MonoObject* LocalPlayer();
    MonoObject* TimeSettings();
    MonoObject* GetObjectField(MonoObject* object, const char* fieldName);

    bool GetBool(MonoObject* object, const char* fieldName, bool& value);
    bool SetBool(MonoObject* object, const char* fieldName, bool value);
    bool GetInt(MonoObject* object, const char* fieldName, int& value);
    bool SetInt(MonoObject* object, const char* fieldName, int value);
    bool GetFloat(MonoObject* object, const char* fieldName, float& value);
    bool SetFloat(MonoObject* object, const char* fieldName, float value);

    // Fusion держит [Networked] свойства в своём буфере состояния, а не в поле
    // _name. Поэтому туда надо писать через сеттер, иначе запись просто
    // ничего не делает.
    bool GetPropBool(MonoObject* object, const char* name, bool& value);
    bool SetPropBool(MonoObject* object, const char* name, bool value);

    MonoObject* InvokeNoArgs(MonoObject* object, const char* methodName);
    bool InvokeVoidNoArgs(MonoObject* object, const char* methodName);
    int BoxedInt(MonoObject* object) const;
    float BoxedFloat(MonoObject* object) const;

    int ListCount(MonoObject* list);
    MonoObject* ListGetObject(MonoObject* list, int index);
    float ListGetFloat(MonoObject* list, int index);
    bool ListSetFloat(MonoObject* list, int index, float value);

    std::vector<MonoObject*> FindObjectsOfTypeAll(const char* className);
    const std::vector<MonoObject*>& ActiveNpcs();

    // Выкидывает записи про уничтоженные объекты и освобождает хэндлы.
    template <typename T>
    void PruneDeadSnapshots(SnapshotMap<T>& snapshots);

    void ResolveEventPatches();
    void UpdateCombinedNpcPatches();
    bool ApplyNpcStableIgnore(bool police);
    void RestoreNpcSnapshots(SnapshotMap<NpcSnapshot>& snapshots);
    bool ApplyCameraStableDisable();
    void RestoreCameras();

    bool ApplyAmmo();
    void RestoreAmmo();
    bool ApplyDurability();
    void RestoreDurability();
    bool ApplyStamina();
    bool ApplyNoDamage();

    void Error(std::wstring text);

    MonoApi& mono_;
    MethodPatcher patcher_;
    MonoDomain* domain_{};
    MonoImage* gameImage_{};
    MonoImage* unityImage_{};
    MonoClass* worldClass_{};
    MonoClass* npcClass_{};
    MonoClass* cameraClass_{};

    std::wstring lastError_;
    bool threadAttached_{};
    bool eventPatchesResolved_{};

    // Проверяем один раз по загруженной сборке, чтобы функция могла честно
    // сказать "в этой сборке такого нет", а не молча ничего не делать.
    bool npcBlindFieldPresent_{};
    bool npcIgnoreFieldPresent_{};
    bool cameraDisablePropPresent_{};

    std::vector<MonoObject*> npcScanCache_;
    unsigned long long npcScanTick_{};

    bool freezeActive_{};
    bool freezeOriginalCaptured_{};
    bool freezeOriginal_ = true;

    bool policeActive_{};
    bool civilianActive_{};
    bool cameraActive_{};
    bool sensorActive_{};
    bool ammoActive_{};
    bool durabilityActive_{};
    bool silentActive_{};
    bool staminaActive_{};
    bool noDamageActive_{};

    bool carriedDamageCaptured_{};
    std::uint32_t weaponManagerHandle_{};
    float originalCarriedDamage_{};

    SnapshotMap<NpcSnapshot> policeNpcSnapshots_;
    SnapshotMap<NpcSnapshot> civilianNpcSnapshots_;
    SnapshotMap<CameraSnapshot> cameraSnapshots_;
    SnapshotMap<AmmoSnapshot> ammoSnapshots_;
    SnapshotMap<SlotDamageSnapshot> slotSnapshots_;
};
