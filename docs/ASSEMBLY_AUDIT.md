# Что удалось вытащить из Assembly-CSharp

Собрано декомпиляцией живой сборки. Служит шпаргалкой — при обновлении игры
всё перепроверяется в рантайме, этот файл только справочный.

```
D:\SteamLibrary\steamapps\common\Crime Simulator\Crime Simulator_Data\Managed\Assembly-CSharp.dll
```

Unity + Mono + Photon Fusion.

```
dotnet tool install -g ilspycmd
ilspycmd -o out -t NPC "<путь>\Assembly-CSharp.dll"
```

`-t <Тип>` разбирает один класс. Целиком сборка на 5 МБ разворачивается в такое,
что искать в нём нечего.

---

## NPC — поля-опции

```
public bool isBlind;                 // проверяется в Sight(), ветка зрения
public bool isDeaf;                  // проверяется в Hearing(), ветка слуха
public bool ignoreEverything;        // ранний выход из Hearing(), в Sight() слабый
public bool dontNoticeStolenItems;
public bool dontNoticeDeadPeople;
public bool dontNoticeOpenDoors;
public bool isPolice;                // по нему делим полицию и гражданских
```

Все обычные, не networked. Пишутся напрямую.

## NPC — что вызывается каждый кадр

Хвост `NPC.Update()`:

```csharp
Sight();
DroneSight();
Hearing();
```

Эти три патчить нельзя ни при каких условиях.

`Sight()` дополнительно ведёт `seePlayerTimer`, `lostPlayerTimer`,
`sawPlayerBeforeKnockdown`, `playerInFOV` и зовёт `SpottedPlayer()`, когда
`currentAnger` переваливает порог. При `isBlind` до этого просто не доходит.

## NPC — события, их патчить можно

```
System.Void NPC::RPC_CallCopsSound()
System.Void NPC::RPC_PullOutWeapon()
System.Void NPC::RPC_CheckWeapon(System.Single)
System.Void NPC::RPC_NoticedStolenItem(UnityEngine.Vector3,System.Single,System.String)
System.Void NPC::RPC_AlertToLocation(UnityEngine.Vector3,System.Single,System.Boolean,System.Boolean,System.Single)
System.Void NPC::CallBackup()
System.Void NPC::AlertOtherTenants()
```

Отдельно: `NPC::RPC_DealDamage(System.Single,System.Boolean,System.Boolean)` —
найден, но подавляться не должен никогда.

## PoliceManager

```
System.Void PoliceManager::RPC_CallOffCops()
System.Void PoliceManager::RPC_PoliceCalled(System.Int32)
System.Void PoliceManager::RPC_ForceUpdateLastPosition(UnityEngine.Vector3)
System.Void PoliceManager::RPC_UpdateLastPosition(UnityEngine.Vector3,System.Boolean)
System.Void PoliceManager::RPC_UpdateMinimapArea(UnityEngine.Vector3)
System.Void PoliceManager::TrySpawnPolice()
```

`TrySpawnPolice()` зовётся из `Update()`, но только внутри
`if (spawnPoliceTimer > timeToSpawnPolice)`, а сам таймер крутится выше по
функции. Заглушка ничего не подвешивает.

## SecurityCamera / MotionSensor

```
System.Void SecurityCamera::RPC_TriggerAlarm(UnityEngine.Vector3)
System.Void SecurityCamera::RPC_Mark(System.Boolean)
System.Void SecurityCamera::RPC_SetDisabled()
System.Void MotionSensor::RPC_TriggerAlarm(UnityEngine.Vector3)
System.Void MotionSensor::RPC_Mark(System.Boolean)
```

У обоих `isDisabled` — `[Networked]`, писать через сеттер свойства.

`MotionSensor::DetectedPlayer(System.Boolean)` — обычный сеттер для
`playerInRange`. Не патчить.

## Прочие сигнализации

```
System.Void CarController::RPC_TriggerAlarm()
System.Void CarController::RPC_Alarm()
System.Void CarController::RPC_WasSeenBySomeone()
System.Void ElectronicSafe::RPC_Alarm()
System.Void ElectronicSafe::RPC_UnlockAndAlarm()
System.Void Keypad::RPC_AlarmSound()
System.Void Keypad::RPC_BlockedAndAlarm()
System.Void Keypad::RPC_TriggerAlarm(UnityEngine.Vector3,System.String)
System.Void Openable::RPC_TriggerAlarm()
System.Void WindowShutter::RPC_AlarmOnDestroy()
```

## FPP_Player

Обычные поля:

```
public float currentStamina = 100f;
public float maxStamina = 100f;
public bool  staminaCooldown;      // пока true - нет прыжка и режется бег
public bool  staminaRegenDelay;
public float damage;               // накопленный урон, 0 = целый
public bool  isInvisible;          // внутриигровая способность, с таймером
```

`[Networked]` свойства:

```
float detectionDistance    // пересчитывается каждый кадр, писать бесполезно
float detectionSpeed
float noiseRange           // тоже пересчитывается
float noiseRangeMultiplier
bool  blockAllNoise        // а вот это держится, игра сама зануляет шум
bool  isDead
```

Нужный кусок:

```csharp
noiseRange = num * noiseRangeMultiplier;
noisePower = num2;
...
if (blockAllNoise || isInvisible)
{
    noisePower = 0f;
    noiseRange = 0f;
}
```

Поэтому бесшумность сделана через `blockAllNoise`, а не записью в `noiseRange`:
последний игра всё равно перезапишет на следующем кадре.

`isInvisible` для трейнера не годится — это расходная способность: проигрывает
звук, показывает сообщение, прячет модель игрока, тикает таймером и сбрасывается
при получении урона.

## StolenItemCollider

```csharp
private void OnTriggerEnter(Collider other)  // ставит detectedNPC = true
private void OnTriggerExit(Collider other)   // снимает
private void Update()                        // раз в 0.25 с пуляет сенсором
```

Глушим только `OnTriggerEnter`. Заглушка на `OnTriggerExit` оставила бы флаг
висеть навсегда.
