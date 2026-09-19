#include "GameController.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace {

bool NameEquals(const char* a, const char* b) {
    if (!a || !b) return false;
    return _stricmp(a, b) == 0;
}

} // namespace

GameController::~GameController() {
    RestoreAll();
}

void __cdecl GameController::ImageLookup(MonoAssembly* assembly, void* userData) {
    auto* ctx = static_cast<ImageLookupContext*>(userData);
    if (!ctx || ctx->image) return;

    MonoImage* image = ctx->mono->assembly_get_image(assembly);
    if (!image) return;

    const char* name = ctx->mono->image_get_name(image);
    if (!name) return;

    if (NameEquals(name, ctx->wanted)) {
        ctx->image = image;
        return;
    }

    std::string withDll = ctx->wanted;
    withDll += ".dll";
    if (NameEquals(name, withDll.c_str())) ctx->image = image;
}

MonoImage* GameController::FindImage(const char* imageName) {
    ImageLookupContext ctx{ &mono_, imageName, nullptr };
    mono_.assembly_foreach(&GameController::ImageLookup, &ctx);
    return ctx.image;
}

bool GameController::Initialize() {
    domain_ = mono_.get_root_domain();
    if (!domain_) {
        Error(L"Корневой домен Mono ещё недоступен");
        return false;
    }

    // Initialize зовётся повторно, пока мир не загрузится, так что attach
    // делаем один раз. И мы уже на потоке окна игры.
    if (!threadAttached_) {
        mono_.thread_attach(domain_);
        threadAttached_ = true;
    }

    gameImage_ = FindImage("Assembly-CSharp");
    unityImage_ = FindImage("UnityEngine.CoreModule");
    if (!gameImage_) {
        Error(L"Assembly-CSharp ещё не загружена");
        return false;
    }
    if (!unityImage_) {
        Error(L"UnityEngine.CoreModule ещё не загружен");
        return false;
    }

    worldClass_ = mono_.class_from_name(gameImage_, "", "WorldManager");
    npcClass_ = mono_.class_from_name(gameImage_, "", "NPC");
    cameraClass_ = mono_.class_from_name(gameImage_, "", "SecurityCamera");

    if (!worldClass_ || !npcClass_) {
        Error(L"Не удалось разрешить основные классы этой версии Crime Simulator");
        return false;
    }

    npcBlindFieldPresent_ =
        mono_.class_get_field_from_name(npcClass_, "isBlind") != nullptr &&
        mono_.class_get_field_from_name(npcClass_, "isDeaf") != nullptr;
    npcIgnoreFieldPresent_ =
        mono_.class_get_field_from_name(npcClass_, "ignoreEverything") != nullptr;
    cameraDisablePropPresent_ = cameraClass_ &&
        mono_.class_get_property_from_name(cameraClass_, "isDisabled") != nullptr;

    if (!eventPatchesResolved_) {
        ResolveEventPatches();
        eventPatchesResolved_ = true;
    }

    lastError_.clear();
    return true;
}

MonoClass* GameController::GameClass(const char* className) {
    if (!gameImage_ && !Initialize()) return nullptr;
    return mono_.class_from_name(gameImage_, "", className);
}

MonoObject* GameController::ResolveWorld() {
    if (!worldClass_ && !Initialize()) return nullptr;

    for (const char* propertyName : { "Instance", "instance" }) {
        MonoProperty* prop = mono_.class_get_property_from_name(worldClass_, propertyName);
        if (!prop) continue;
        MonoMethod* getter = mono_.property_get_get_method(prop);
        if (!getter) continue;

        MonoObject* exc = nullptr;
        MonoObject* result = mono_.runtime_invoke(getter, nullptr, nullptr, &exc);
        if (!exc && result) return result;
    }

    MonoVTable* vtable = mono_.class_vtable(domain_, worldClass_);
    if (vtable) {
        for (const char* fieldName : { "_instance", "Instance", "instance", "m_Instance" }) {
            MonoClassField* field = mono_.class_get_field_from_name(worldClass_, fieldName);
            if (!field) continue;
            MonoObject* result = nullptr;
            mono_.field_static_get_value(vtable, field, &result);
            if (result) return result;
        }
    }

    Error(L"Игровой мир ещё не создан");
    return nullptr;
}

MonoObject* GameController::LocalPlayer() {
    MonoObject* world = ResolveWorld();
    return world ? GetObjectField(world, "localPlayer") : nullptr;
}

MonoObject* GameController::TimeSettings() {
    MonoObject* world = ResolveWorld();
    MonoObject* enviro = world ? GetObjectField(world, "enviro") : nullptr;
    if (!enviro) enviro = world ? GetObjectField(world, "Enviro") : nullptr;
    MonoObject* time = enviro ? GetObjectField(enviro, "Time") : nullptr;
    if (!time) time = enviro ? GetObjectField(enviro, "time") : nullptr;
    if (!time) return nullptr;

    MonoObject* settings = GetObjectField(time, "Settings");
    if (!settings) settings = GetObjectField(time, "settings");
    return settings;
}

MonoObject* GameController::GetObjectField(MonoObject* object, const char* fieldName) {
    if (!object) return nullptr;
    MonoClass* klass = mono_.object_get_class(object);
    if (!klass) return nullptr;
    MonoClassField* field = mono_.class_get_field_from_name(klass, fieldName);
    if (!field) return nullptr;

    MonoObject* result = nullptr;
    mono_.field_get_value(object, field, &result);
    return result;
}

bool GameController::GetBool(MonoObject* object, const char* fieldName, bool& value) {
    if (!object) return false;
    MonoClass* klass = mono_.object_get_class(object);
    if (!klass) return false;
    MonoClassField* field = mono_.class_get_field_from_name(klass, fieldName);
    if (!field) return false;
    bool result = false;
    mono_.field_get_value(object, field, &result);
    value = result;
    return true;
}

bool GameController::SetBool(MonoObject* object, const char* fieldName, bool value) {
    if (!object) return false;
    MonoClass* klass = mono_.object_get_class(object);
    if (!klass) return false;
    MonoClassField* field = mono_.class_get_field_from_name(klass, fieldName);
    if (!field) return false;
    mono_.field_set_value(object, field, &value);
    return true;
}

bool GameController::GetInt(MonoObject* object, const char* fieldName, int& value) {
    if (!object) return false;
    MonoClass* klass = mono_.object_get_class(object);
    if (!klass) return false;
    MonoClassField* field = mono_.class_get_field_from_name(klass, fieldName);
    if (!field) return false;
    int result = 0;
    mono_.field_get_value(object, field, &result);
    value = result;
    return true;
}

bool GameController::SetInt(MonoObject* object, const char* fieldName, int value) {
    if (!object) return false;
    MonoClass* klass = mono_.object_get_class(object);
    if (!klass) return false;
    MonoClassField* field = mono_.class_get_field_from_name(klass, fieldName);
    if (!field) return false;
    mono_.field_set_value(object, field, &value);
    return true;
}

bool GameController::GetFloat(MonoObject* object, const char* fieldName, float& value) {
    if (!object) return false;
    MonoClass* klass = mono_.object_get_class(object);
    if (!klass) return false;
    MonoClassField* field = mono_.class_get_field_from_name(klass, fieldName);
    if (!field) return false;
    float result = 0.0f;
    mono_.field_get_value(object, field, &result);
    value = result;
    return true;
}

bool GameController::SetFloat(MonoObject* object, const char* fieldName, float value) {
    if (!object) return false;
    MonoClass* klass = mono_.object_get_class(object);
    if (!klass) return false;
    MonoClassField* field = mono_.class_get_field_from_name(klass, fieldName);
    if (!field) return false;
    mono_.field_set_value(object, field, &value);
    return true;
}

bool GameController::GetPropBool(MonoObject* object, const char* name, bool& value) {
    if (!object) return false;
    MonoClass* klass = mono_.object_get_class(object);
    if (!klass) return false;
    MonoProperty* prop = mono_.class_get_property_from_name(klass, name);
    MonoMethod* getter = prop ? mono_.property_get_get_method(prop) : nullptr;
    if (!getter) return false;

    MonoObject* exc = nullptr;
    MonoObject* boxed = mono_.runtime_invoke(getter, object, nullptr, &exc);
    if (exc || !boxed) return false;

    void* raw = mono_.object_unbox(boxed);
    if (!raw) return false;
    value = *static_cast<bool*>(raw);
    return true;
}

bool GameController::SetPropBool(MonoObject* object, const char* name, bool value) {
    if (!object) return false;
    MonoClass* klass = mono_.object_get_class(object);
    if (!klass) return false;
    MonoProperty* prop = mono_.class_get_property_from_name(klass, name);
    MonoMethod* setter = prop ? mono_.property_get_set_method(prop) : nullptr;
    if (!setter) return false;

    void* args[1]{ &value };
    MonoObject* exc = nullptr;
    mono_.runtime_invoke(setter, object, args, &exc);
    return exc == nullptr;
}

MonoObject* GameController::InvokeNoArgs(MonoObject* object, const char* methodName) {
    if (!object) return nullptr;
    MonoClass* klass = mono_.object_get_class(object);
    if (!klass) return nullptr;
    MonoMethod* method = mono_.class_get_method_from_name(klass, methodName, 0);
    if (!method) return nullptr;

    MonoObject* exc = nullptr;
    MonoObject* result = mono_.runtime_invoke(method, object, nullptr, &exc);
    return exc ? nullptr : result;
}

bool GameController::InvokeVoidNoArgs(MonoObject* object, const char* methodName) {
    if (!object) return false;
    MonoClass* klass = mono_.object_get_class(object);
    if (!klass) return false;
    MonoMethod* method = mono_.class_get_method_from_name(klass, methodName, 0);
    if (!method) return false;

    MonoObject* exc = nullptr;
    mono_.runtime_invoke(method, object, nullptr, &exc);
    return exc == nullptr;
}

int GameController::BoxedInt(MonoObject* object) const {
    if (!object) return -1;
    MonoClass* klass = mono_.object_get_class(object);
    if (!klass) return -1;
    const char* name = mono_.class_get_name(klass);
    void* value = mono_.object_unbox(object);
    if (!name || !value) return -1;

    if (std::strcmp(name, "Int32") == 0) return *static_cast<int*>(value);
    if (std::strcmp(name, "UInt32") == 0) return static_cast<int>(*static_cast<unsigned int*>(value));
    if (std::strcmp(name, "Int16") == 0) return *static_cast<short*>(value);
    if (std::strcmp(name, "UInt16") == 0) return *static_cast<unsigned short*>(value);
    if (std::strcmp(name, "Byte") == 0) return *static_cast<unsigned char*>(value);
    return -1;
}

float GameController::BoxedFloat(MonoObject* object) const {
    if (!object) return 0.0f;
    MonoClass* klass = mono_.object_get_class(object);
    if (!klass) return 0.0f;
    const char* name = mono_.class_get_name(klass);
    void* value = mono_.object_unbox(object);
    if (!name || !value) return 0.0f;

    if (std::strcmp(name, "Single") == 0) return *static_cast<float*>(value);
    if (std::strcmp(name, "Double") == 0) return static_cast<float>(*static_cast<double*>(value));
    if (std::strcmp(name, "Int32") == 0) return static_cast<float>(*static_cast<int*>(value));
    return 0.0f;
}

int GameController::ListCount(MonoObject* list) {
    return BoxedInt(InvokeNoArgs(list, "get_Count"));
}

MonoObject* GameController::ListGetObject(MonoObject* list, int index) {
    if (!list || index < 0) return nullptr;
    MonoClass* klass = mono_.object_get_class(list);
    if (!klass) return nullptr;
    MonoMethod* method = mono_.class_get_method_from_name(klass, "get_Item", 1);
    if (!method) return nullptr;

    void* args[1]{ &index };
    MonoObject* exc = nullptr;
    MonoObject* result = mono_.runtime_invoke(method, list, args, &exc);
    return exc ? nullptr : result;
}

float GameController::ListGetFloat(MonoObject* list, int index) {
    return BoxedFloat(ListGetObject(list, index));
}

bool GameController::ListSetFloat(MonoObject* list, int index, float value) {
    if (!list || index < 0) return false;
    MonoClass* klass = mono_.object_get_class(list);
    if (!klass) return false;
    MonoMethod* method = mono_.class_get_method_from_name(klass, "set_Item", 2);
    if (!method) return false;

    void* args[2]{ &index, &value };
    MonoObject* exc = nullptr;
    mono_.runtime_invoke(method, list, args, &exc);
    return exc == nullptr;
}

std::vector<MonoObject*> GameController::FindObjectsOfTypeAll(const char* className) {
    std::vector<MonoObject*> result;
    if (!domain_ || !gameImage_ || !unityImage_) return result;

    MonoClass* target = GameClass(className);
    if (!target) return result;

    MonoType* targetType = mono_.class_get_type(target);
    MonoObject* reflectedType = targetType ? mono_.type_get_object(domain_, targetType) : nullptr;
    if (!reflectedType) return result;

    // Берём только активные объекты сцены. Resources.FindObjectsOfTypeAll
    // вернул бы ещё и prefab'ы, у которых Fusion ещё не вызвал Spawned().
    MonoClass* unityObject = mono_.class_from_name(unityImage_, "UnityEngine", "Object");
    MonoMethod* find = unityObject
        ? mono_.class_get_method_from_name(unityObject, "FindObjectsOfType", 1)
        : nullptr;

    if (!find) {
        MonoClass* resources = mono_.class_from_name(unityImage_, "UnityEngine", "Resources");
        find = resources
            ? mono_.class_get_method_from_name(resources, "FindObjectsOfTypeAll", 1)
            : nullptr;
    }
    if (!find) return result;

    void* args[1]{ reflectedType };
    MonoObject* exc = nullptr;
    MonoObject* raw = mono_.runtime_invoke(find, nullptr, args, &exc);
    if (exc || !raw) return result;

    auto* array = reinterpret_cast<MonoArray*>(raw);
    const std::uintptr_t count = mono_.ArrayLength(array);
    result.reserve(static_cast<std::size_t>(count));

    for (std::uintptr_t i = 0; i < count; ++i) {
        void* slot = mono_.ArrayElement(array, i, sizeof(void*));
        if (!slot) continue;
        MonoObject* object = *reinterpret_cast<MonoObject**>(slot);
        if (object) result.push_back(object);
    }
    return result;
}

const std::vector<MonoObject*>& GameController::ActiveNpcs() {
    // FindObjectsOfType обходит всю сцену, а полицию и гражданских обновляем
    // подряд в одном тике. Сканируем один раз, а не по разу на функцию.
    const ULONGLONG now = GetTickCount64();
    if (npcScanTick_ == 0 || now - npcScanTick_ > 50) {
        npcScanCache_ = FindObjectsOfTypeAll("NPC");
        npcScanTick_ = now;
    }
    return npcScanCache_;
}

template <typename T>
void GameController::PruneDeadSnapshots(SnapshotMap<T>& snapshots) {
    for (auto it = snapshots.begin(); it != snapshots.end();) {
        if (mono_.gchandle_get_target(it->second.handle) == it->first) {
            ++it;
            continue;
        }
        // Либо объект уничтожен и слабый хэндл обнулился, либо адрес уже занял
        // кто-то другой. В обоих случаях восстанавливать нечего.
        mono_.gchandle_free(it->second.handle);
        it = snapshots.erase(it);
    }
}

void GameController::ResolveEventPatches() {
    if (!gameImage_) return;

    auto add = [this](const char* key, const char* klass, const char* method, int params = -1) {
        patcher_.Resolve(gameImage_, MethodPatcher::Target{ key, klass, method, params });
    };

    // ВАЖНО. Сюда можно класть только методы-события, которые игра зовёт,
    // когда что-то случилось. Методы, которые крутятся каждый кадр из
    // Update(), патчить нельзя: они же обычно и сбрасывают своё состояние,
    // и после ret оно застывает навсегда. Из-за этого NPC переставали ходить,
    // так что NPC::Sight и NPC::Hearing отсюда убраны.

    // Полиция.
    add("police.manager.called", "PoliceManager", "RPC_PoliceCalled");
    add("police.manager.spawn", "PoliceManager", "TrySpawnPolice");
    add("police.manager.forcepos", "PoliceManager", "RPC_ForceUpdateLastPosition");
    add("police.manager.updatepos", "PoliceManager", "RPC_UpdateLastPosition");
    add("police.manager.minimap", "PoliceManager", "RPC_UpdateMinimapArea");
    add("police.npc.backup", "NPC", "CallBackup");
    add("police.npc.spawn", "NPC", "TrySpawnPolice");
    add("police.drone.yell", "PoliceDrone", "RPC_Yell");

    // Гражданские: кражи, шум, уведомления.
    add("civilian.npc.stolen", "NPC", "RPC_NoticedStolenItem");
    add("civilian.player.heardmsg", "FPP_Player", "RPC_WasHeardMessage");
    add("civilian.possession.alert", "StolenItemCollider", "AlertPosessionNPCs");
    add("civilian.possession.enter", "StolenItemCollider", "OnTriggerEnter");
    add("civilian.possession.detect", "StolenItemCollider", "DetectedNewLoot");
    add("civilian.possession.player_enter", "PlayerPosessionCollider", "OnTriggerEnter");
    // OnTriggerExit специально не трогаем: он снимает флаг, а не ставит.
    // Застабив его, можно навсегда оставить "игрок на моей территории".

    // Общие реакции NPC. Включаются, только когда выключены И полиция,
    // И гражданские. Урон и смерть сюда не попадают никогда - игнорящего
    // гражданского всё ещё можно ударить и убить.
    add("npcboth.alert", "NPC", "RPC_AlertToLocation");
    add("npcboth.weapon", "NPC", "RPC_PullOutWeapon");
    add("npcboth.checkweapon", "NPC", "RPC_CheckWeapon");
    add("npcboth.illegal", "NPC", "CheckIllegal");
    add("npcboth.noise", "NPC", "CheckNoiseRange");
    add("npcboth.attack", "NPC", "TriggerAttack");
    add("npcboth.other_tenants", "NPC", "AlertOtherTenants");
    add("npcboth.possession", "NPC", "AlertPosessionNPCs");

    // Камеры: сетевые alarm-RPC. Само обнаружение гасится флагом isDisabled.
    add("camera.trigger", "SecurityCamera", "RPC_TriggerAlarm");
    add("camera.mark", "SecurityCamera", "RPC_Mark");
    add("camera.alarm", "SecurityCamera", "TriggerAlarm");

    // Датчики и сигнализации.
    add("sensor.motion.trigger", "MotionSensor", "RPC_TriggerAlarm");
    add("sensor.motion.mark", "MotionSensor", "RPC_Mark");

    add("sensor.laser.trigger", "LaserAlarm", "TriggerAlarm");
    add("sensor.laser.alarm", "LaserAlarm", "Alarm");

    for (const char* klass : { "SensorDetector", "DoorSensor", "ExclamationSensor",
                               "AttackSensor", "SightSensor", "DroneSensor" }) {
        const std::string base = std::string("sensor.generic.") + klass + ".";
        patcher_.Resolve(gameImage_, { base + "trigger", klass, "TriggerSensor", -1 });
        patcher_.Resolve(gameImage_, { base + "alarm", klass, "TriggerAlarm", -1 });
    }

    add("sensor.car.trigger", "CarController", "RPC_TriggerAlarm");
    add("sensor.car.alarm", "CarController", "RPC_Alarm");
    add("sensor.car.seen", "CarController", "RPC_WasSeenBySomeone");

    add("sensor.safe.alarm", "ElectronicSafe", "RPC_Alarm");
    add("sensor.keypad.sound", "Keypad", "RPC_AlarmSound");
    add("sensor.keypad.blocked", "Keypad", "RPC_BlockedAndAlarm");
    add("sensor.keypad.trigger", "Keypad", "RPC_TriggerAlarm");
    add("sensor.openable.trigger", "Openable", "RPC_TriggerAlarm");
    add("sensor.shutter.destroy_alarm", "WindowShutter", "RPC_AlarmOnDestroy");
}

void GameController::UpdateCombinedNpcPatches() {
    patcher_.SetEnabledByPrefix("npcboth.", policeActive_ && civilianActive_);
}

bool GameController::ApplyNpcStableIgnore(bool police) {
    if (!npcBlindFieldPresent_ && !npcIgnoreFieldPresent_) return false;

    auto& snapshots = police ? policeNpcSnapshots_ : civilianNpcSnapshots_;
    PruneDeadSnapshots(snapshots);

    for (MonoObject* npc : ActiveNpcs()) {
        bool isPolice = false;
        if (!GetBool(npc, "isPolice", isPolice) || isPolice != police) continue;

        if (snapshots.find(npc) == snapshots.end()) {
            NpcSnapshot fresh{};
            fresh.handle = mono_.WeakHandle(npc);
            if (!fresh.handle) continue;
            GetBool(npc, "isBlind", fresh.blind);
            GetBool(npc, "isDeaf", fresh.deaf);
            GetBool(npc, "ignoreEverything", fresh.ignoreEverything);
            GetBool(npc, "dontNoticeStolenItems", fresh.dontNoticeStolenItems);
            GetBool(npc, "dontNoticeDeadPeople", fresh.dontNoticeDeadPeople);
            GetBool(npc, "dontNoticeOpenDoors", fresh.dontNoticeOpenDoors);
            snapshots.emplace(npc, fresh);
        }

        // isBlind и isDeaf стоят ровно в том if, который решает, видит и слышит
        // ли NPC игрока. Sight() и Hearing() при этом отрабатывают целиком и
        // сами обнуляют seesPlayer/hearsPlayer и свои таймеры. Заморозки нет.
        SetBool(npc, "isBlind", true);
        SetBool(npc, "isDeaf", true);
        SetBool(npc, "ignoreEverything", true);
        SetBool(npc, "dontNoticeStolenItems", true);
        SetBool(npc, "dontNoticeDeadPeople", true);
        SetBool(npc, "dontNoticeOpenDoors", true);
    }
    return true;
}

void GameController::RestoreNpcSnapshots(SnapshotMap<NpcSnapshot>& snapshots) {
    for (auto& [object, snapshot] : snapshots) {
        MonoObject* npc = mono_.gchandle_get_target(snapshot.handle);
        if (npc == object) {
            SetBool(npc, "isBlind", snapshot.blind);
            SetBool(npc, "isDeaf", snapshot.deaf);
            SetBool(npc, "ignoreEverything", snapshot.ignoreEverything);
            SetBool(npc, "dontNoticeStolenItems", snapshot.dontNoticeStolenItems);
            SetBool(npc, "dontNoticeDeadPeople", snapshot.dontNoticeDeadPeople);
            SetBool(npc, "dontNoticeOpenDoors", snapshot.dontNoticeOpenDoors);
        }
        mono_.gchandle_free(snapshot.handle);
    }
    snapshots.clear();
}

bool GameController::ApplyCameraStableDisable() {
    if (!cameraDisablePropPresent_) return false;

    PruneDeadSnapshots(cameraSnapshots_);

    for (MonoObject* camera : FindObjectsOfTypeAll("SecurityCamera")) {
        if (cameraSnapshots_.find(camera) == cameraSnapshots_.end()) {
            CameraSnapshot fresh{};
            fresh.handle = mono_.WeakHandle(camera);
            if (!fresh.handle) continue;
            if (!GetPropBool(camera, "isDisabled", fresh.disabled)) {
                mono_.gchandle_free(fresh.handle);
                continue;
            }
            cameraSnapshots_.emplace(camera, fresh);
        }
        SetPropBool(camera, "isDisabled", true);
    }
    return true;
}

void GameController::RestoreCameras() {
    for (auto& [object, snapshot] : cameraSnapshots_) {
        MonoObject* camera = mono_.gchandle_get_target(snapshot.handle);
        if (camera == object) SetPropBool(camera, "isDisabled", snapshot.disabled);
        mono_.gchandle_free(snapshot.handle);
    }
    cameraSnapshots_.clear();
}

bool GameController::ApplyAmmo() {
    MonoObject* player = LocalPlayer();
    MonoObject* weaponManager = player ? GetObjectField(player, "weaponManager") : nullptr;
    MonoObject* weapons = weaponManager ? GetObjectField(weaponManager, "weapons") : nullptr;
    if (!weapons) return false;

    PruneDeadSnapshots(ammoSnapshots_);

    const int weaponCount = ListCount(weapons);
    for (int i = 0; i < weaponCount; ++i) {
        MonoObject* weapon = ListGetObject(weapons, i);
        MonoObject* ammoTypes = weapon ? GetObjectField(weapon, "ammoTypes") : nullptr;
        const int ammoCount = ammoTypes ? ListCount(ammoTypes) : 0;

        for (int j = 0; j < ammoCount; ++j) {
            MonoObject* ammo = ListGetObject(ammoTypes, j);
            if (!ammo) continue;

            if (ammoSnapshots_.find(ammo) == ammoSnapshots_.end()) {
                AmmoSnapshot snapshot{};
                snapshot.handle = mono_.WeakHandle(ammo);
                if (!snapshot.handle) continue;
                GetInt(ammo, "amountInInventory", snapshot.amount);
                ammoSnapshots_.emplace(ammo, snapshot);
            }

            // Пишем, только если игра реально потратила патроны. Иначе будем
            // зря дёргать инвентарь по 5 раз в секунду.
            int current = 0;
            if (GetInt(ammo, "amountInInventory", current) && current >= kAmmoTopUp) continue;
            SetInt(ammo, "amountInInventory", kAmmoTopUp);
        }
    }
    return true;
}

void GameController::RestoreAmmo() {
    for (auto& [object, snapshot] : ammoSnapshots_) {
        MonoObject* ammo = mono_.gchandle_get_target(snapshot.handle);
        if (ammo == object) SetInt(ammo, "amountInInventory", snapshot.amount);
        mono_.gchandle_free(snapshot.handle);
    }
    ammoSnapshots_.clear();
}

bool GameController::ApplyDurability() {
    MonoObject* player = LocalPlayer();
    MonoObject* inventory = player ? GetObjectField(player, "inventory") : nullptr;
    MonoObject* weaponManager = player ? GetObjectField(player, "weaponManager") : nullptr;
    MonoObject* slots = inventory ? GetObjectField(inventory, "itemSlots") : nullptr;

    if (!weaponManager && !slots) return false;

    if (weaponManager) {
        if (!carriedDamageCaptured_) {
            weaponManagerHandle_ = mono_.WeakHandle(weaponManager);
            if (weaponManagerHandle_) {
                GetFloat(weaponManager, "carriedItemDamage", originalCarriedDamage_);
                carriedDamageCaptured_ = true;
            }
        }
        float carried = 0.0f;
        if (!GetFloat(weaponManager, "carriedItemDamage", carried) || carried != 0.0f) {
            SetFloat(weaponManager, "carriedItemDamage", 0.0f);
        }
    }

    PruneDeadSnapshots(slotSnapshots_);

    const int slotCount = slots ? ListCount(slots) : 0;
    for (int i = 0; i < slotCount; ++i) {
        MonoObject* slot = ListGetObject(slots, i);
        MonoObject* damages = slot ? GetObjectField(slot, "damagesPerItem") : nullptr;
        if (!slot || !damages) continue;

        auto entry = slotSnapshots_.find(slot);
        if (entry == slotSnapshots_.end()) {
            SlotDamageSnapshot fresh{};
            fresh.handle = mono_.WeakHandle(slot);
            if (!fresh.handle) continue;
            entry = slotSnapshots_.emplace(slot, std::move(fresh)).first;
        }
        SlotDamageSnapshot& snapshot = entry->second;

        const int count = ListCount(damages);
        if (count < 0) continue;

        for (int j = 0; j < count; ++j) {
            const float wear = ListGetFloat(damages, j);
            if (static_cast<int>(snapshot.damages.size()) <= j) {
                snapshot.damages.resize(static_cast<std::size_t>(j) + 1, 0.0f);
                snapshot.damages[static_cast<std::size_t>(j)] = wear;
            }
            if (wear != 0.0f) ListSetFloat(damages, j, 0.0f);
        }
    }
    return true;
}

void GameController::RestoreDurability() {
    for (auto& [object, snapshot] : slotSnapshots_) {
        MonoObject* slot = mono_.gchandle_get_target(snapshot.handle);
        MonoObject* damages = slot == object ? GetObjectField(slot, "damagesPerItem") : nullptr;
        const int count = damages ? ListCount(damages) : 0;
        const int restoreCount = std::min(count, static_cast<int>(snapshot.damages.size()));
        for (int i = 0; i < restoreCount; ++i) ListSetFloat(damages, i, snapshot.damages[i]);
        mono_.gchandle_free(snapshot.handle);
    }
    slotSnapshots_.clear();

    if (carriedDamageCaptured_) {
        MonoObject* manager = mono_.gchandle_get_target(weaponManagerHandle_);
        if (manager) SetFloat(manager, "carriedItemDamage", originalCarriedDamage_);
        mono_.gchandle_free(weaponManagerHandle_);
        weaponManagerHandle_ = 0;
        carriedDamageCaptured_ = false;
    }
}

bool GameController::ApplyStamina() {
    MonoObject* player = LocalPlayer();
    if (!player) return false;

    float maxStamina = 0.0f;
    if (!GetFloat(player, "maxStamina", maxStamina) || maxStamina <= 0.0f) return false;

    float current = 0.0f;
    if (GetFloat(player, "currentStamina", current) && current < maxStamina) {
        SetFloat(player, "currentStamina", maxStamina);
    }
    // Пока висит cooldown, игра запрещает прыжок и режет скорость бега.
    SetBool(player, "staminaCooldown", false);
    SetBool(player, "staminaRegenDelay", false);
    return true;
}

bool GameController::ApplyNoDamage() {
    MonoObject* player = LocalPlayer();
    if (!player) return false;

    // damage - это накопленный урон, 0 значит целый. Сам расчёт урона не
    // трогаем, просто откатываем результат.
    float damage = 0.0f;
    if (!GetFloat(player, "damage", damage)) return false;
    if (damage != 0.0f) SetFloat(player, "damage", 0.0f);
    return true;
}

GameController::OpResult GameController::SetFreezeTime(bool on) {
    MonoObject* settings = TimeSettings();
    if (!settings) {
        if (freezeActive_) {
            freezeActive_ = false;
            freezeOriginalCaptured_ = false;
        }
        Error(L"Система игрового времени ещё недоступна");
        return OpResult::Waiting;
    }

    if (on) {
        if (!freezeOriginalCaptured_) {
            if (!GetBool(settings, "simulate", freezeOriginal_)) return OpResult::Unsupported;
            freezeOriginalCaptured_ = true;
        }
        if (!SetBool(settings, "simulate", false)) return OpResult::Unsupported;
        freezeActive_ = true;
    } else if (freezeActive_) {
        if (freezeOriginalCaptured_) SetBool(settings, "simulate", freezeOriginal_);
        freezeOriginalCaptured_ = false;
        freezeActive_ = false;
    }

    lastError_.clear();
    return OpResult::Applied;
}

// Enviro считает длину суток от cycleLengthInMinutes: чем больше, тем медленнее
// идут часы. В отличие от simulate = false часы всё-таки идут, а это важно -
// распорядок дня NPC двигается по смене часа, см. NPC.Update.
GameController::OpResult GameController::SetSlowTime(bool on) {
    MonoObject* settings = TimeSettings();
    if (!settings) {
        if (slowActive_) {
            slowActive_ = false;
            slowOriginalCaptured_ = false;
        }
        Error(L"Система игрового времени ещё недоступна");
        return OpResult::Waiting;
    }

    if (on) {
        if (!slowOriginalCaptured_) {
            if (!GetFloat(settings, "cycleLengthInMinutes", slowOriginalCycle_) ||
                slowOriginalCycle_ <= 0.0f) {
                return OpResult::Unsupported;
            }
            slowOriginalCaptured_ = true;
        }
        if (!SetFloat(settings, "cycleLengthInMinutes", slowOriginalCycle_ * kSlowTimeFactor)) {
            return OpResult::Unsupported;
        }
        slowActive_ = true;
    } else if (slowActive_) {
        if (slowOriginalCaptured_) SetFloat(settings, "cycleLengthInMinutes", slowOriginalCycle_);
        slowOriginalCaptured_ = false;
        slowActive_ = false;
    }

    lastError_.clear();
    return OpResult::Applied;
}

GameController::OpResult GameController::SetIgnorePolice(bool on) {
    const bool haveHooks = patcher_.ResolvedByPrefix("police.") > 0;
    if (!haveHooks && !npcBlindFieldPresent_) {
        policeActive_ = false;
        UpdateCombinedNpcPatches();
        Error(L"Для полиции этой версии игры не найдено безопасных hook'ов");
        return OpResult::Unsupported;
    }

    if (on) {
        if (!policeActive_) {
            // Уже начавшуюся погоню снимаем один раз штатным методом игры.
            // Никаких циклов сброса alert-полей.
            MonoObject* world = ResolveWorld();
            MonoObject* manager = world ? GetObjectField(world, "policeManager") : nullptr;
            if (manager) InvokeVoidNoArgs(manager, "RPC_CallOffCops");
        }
        policeActive_ = true;
        patcher_.SetEnabledByPrefix("police.", true);
        ApplyNpcStableIgnore(true);
    } else if (policeActive_) {
        patcher_.SetEnabledByPrefix("police.", false);
        RestoreNpcSnapshots(policeNpcSnapshots_);
        policeActive_ = false;
    }

    UpdateCombinedNpcPatches();
    lastError_.clear();
    return OpResult::Applied;
}

GameController::OpResult GameController::SetIgnoreCivilians(bool on) {
    const bool haveHooks = patcher_.ResolvedByPrefix("civilian.") > 0;
    if (!haveHooks && !npcBlindFieldPresent_) {
        civilianActive_ = false;
        UpdateCombinedNpcPatches();
        Error(L"Для гражданских этой версии игры не найдено безопасных hook'ов");
        return OpResult::Unsupported;
    }

    if (on) {
        civilianActive_ = true;
        patcher_.SetEnabledByPrefix("civilian.", true);
        ApplyNpcStableIgnore(false);
    } else if (civilianActive_) {
        patcher_.SetEnabledByPrefix("civilian.", false);
        RestoreNpcSnapshots(civilianNpcSnapshots_);
        civilianActive_ = false;
    }

    UpdateCombinedNpcPatches();
    lastError_.clear();
    return OpResult::Applied;
}

GameController::OpResult GameController::SetIgnoreCameras(bool on) {
    const bool haveHooks = patcher_.ResolvedByPrefix("camera.") > 0;
    if (!haveHooks && !cameraDisablePropPresent_) {
        cameraActive_ = false;
        Error(L"В этой версии игры не найден класс охранных камер");
        return OpResult::Unsupported;
    }

    if (on) {
        cameraActive_ = true;
        patcher_.SetEnabledByPrefix("camera.", true);
        ApplyCameraStableDisable();
    } else if (cameraActive_) {
        patcher_.SetEnabledByPrefix("camera.", false);
        RestoreCameras();
        cameraActive_ = false;
    }
    lastError_.clear();
    return OpResult::Applied;
}

GameController::OpResult GameController::SetIgnoreSensors(bool on) {
    if (patcher_.ResolvedByPrefix("sensor.") == 0) {
        sensorActive_ = false;
        Error(L"Для датчиков этой версии игры не найдено безопасных hook'ов");
        return OpResult::Unsupported;
    }

    if (on) {
        sensorActive_ = true;
        patcher_.SetEnabledByPrefix("sensor.", true);
    } else if (sensorActive_) {
        patcher_.SetEnabledByPrefix("sensor.", false);
        sensorActive_ = false;
    }
    lastError_.clear();
    return OpResult::Applied;
}

GameController::OpResult GameController::SetInfiniteAmmo(bool on) {
    if (on) {
        if (!ApplyAmmo()) {
            Error(L"Оружие игрока ещё не загружено");
            return OpResult::Waiting;
        }
        ammoActive_ = true;
    } else if (ammoActive_) {
        RestoreAmmo();
        ammoActive_ = false;
    }
    lastError_.clear();
    return OpResult::Applied;
}

GameController::OpResult GameController::SetUnbreakableItems(bool on) {
    if (on) {
        if (!ApplyDurability()) {
            Error(L"Инвентарь игрока ещё не загружен");
            return OpResult::Waiting;
        }
        durabilityActive_ = true;
    } else if (durabilityActive_) {
        RestoreDurability();
        durabilityActive_ = false;
    }
    lastError_.clear();
    return OpResult::Applied;
}

GameController::OpResult GameController::SetSilentSteps(bool on) {
    // Если мы её и не включали, то и выключать нечего. Иначе раз в секунду
    // затирали бы флаг, который игра могла поставить сама (бонусы, способности).
    if (!on && !silentActive_) {
        lastError_.clear();
        return OpResult::Applied;
    }

    MonoObject* player = LocalPlayer();
    if (!player) {
        silentActive_ = false;
        Error(L"Игрок ещё не загружен");
        return OpResult::Waiting;
    }

    // blockAllNoise - штатный флаг игры. Пока он стоит, она сама каждый кадр
    // обнуляет noisePower и noiseRange, так что NPC физически нечего слышать.
    if (!SetPropBool(player, "blockAllNoise", on)) {
        silentActive_ = false;
        Error(L"В этой версии игры нет blockAllNoise");
        return OpResult::Unsupported;
    }
    silentActive_ = on;
    lastError_.clear();
    return OpResult::Applied;
}

GameController::OpResult GameController::SetInfiniteStamina(bool on) {
    if (on) {
        if (!ApplyStamina()) {
            Error(L"Игрок ещё не загружен");
            return OpResult::Waiting;
        }
        staminaActive_ = true;
    } else {
        staminaActive_ = false;
    }
    lastError_.clear();
    return OpResult::Applied;
}

GameController::OpResult GameController::SetNoDamage(bool on) {
    if (on) {
        if (!ApplyNoDamage()) {
            Error(L"Игрок ещё не загружен");
            return OpResult::Waiting;
        }
        noDamageActive_ = true;
    } else {
        noDamageActive_ = false;
    }
    lastError_.clear();
    return OpResult::Applied;
}

bool GameController::GetClock(int& hour, int& minute) {
    MonoObject* world = ResolveWorld();
    MonoObject* enviro = world ? GetObjectField(world, "enviro") : nullptr;
    if (!enviro) enviro = world ? GetObjectField(world, "Enviro") : nullptr;
    MonoObject* time = enviro ? GetObjectField(enviro, "Time") : nullptr;
    if (!time) time = enviro ? GetObjectField(enviro, "time") : nullptr;
    if (!time) return false;

    const int h = BoxedInt(InvokeNoArgs(time, "GetTimeOfDayHour"));
    const int m = BoxedInt(InvokeNoArgs(time, "GetTimeOfDayMinute"));
    if (h < 0 || m < 0) return false;

    hour = ((h % 24) + 24) % 24;
    minute = std::clamp(m, 0, 59);
    return true;
}

void GameController::RestoreAll() {
    if (!mono_.IsReady() || !domain_) return;

    // Сначала возвращаем байты методов, чтобы дальше работал штатный код игры.
    patcher_.RestoreAll();

    SetFreezeTime(false);
    SetSlowTime(false);

    if (silentActive_) {
        MonoObject* player = LocalPlayer();
        if (player) SetPropBool(player, "blockAllNoise", false);
        silentActive_ = false;
    }

    RestoreNpcSnapshots(policeNpcSnapshots_);
    RestoreNpcSnapshots(civilianNpcSnapshots_);
    RestoreCameras();
    RestoreAmmo();
    RestoreDurability();

    policeActive_ = false;
    civilianActive_ = false;
    cameraActive_ = false;
    sensorActive_ = false;
    ammoActive_ = false;
    durabilityActive_ = false;
    staminaActive_ = false;
    noDamageActive_ = false;

    npcScanCache_.clear();
    npcScanTick_ = 0;
}

void GameController::Error(std::wstring text) {
    lastError_ = std::move(text);
}
