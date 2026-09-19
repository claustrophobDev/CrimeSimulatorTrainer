# Что и где перехватывается

Все цели ищутся в метаданных Mono в рантайме. Чего нет — пропускается и пишется
в `CrimeTrainerHook.log`. Ни один адрес не патчится «на удачу».

---

## Главное правило

Патчить можно **только методы-события** — те, которые игра зовёт, когда что-то
случилось.

Методы, которые крутятся каждый кадр из `Update()`, патчить **нельзя**. Почти
всегда такой метод не только проверяет условие, но и сбрасывает своё состояние,
когда условие не выполнено. Заглушка `ret` убирает и сброс тоже, и флаг застывает
навсегда.

Реальные примеры из этой игры:

```csharp
public void Sight()          // вызывается из NPC.Update() каждый кадр
{
    ...
    if (playerInFOV && ... && !isBlind) { seesPlayer = true;  }
    else                                { seesPlayer = false; }   // <- вот это
    ...
    if (seesPlayer) seePlayerTimer += Time.deltaTime;
    else          { seePlayerTimer = 0f; lostPlayerTimer += ...; }
}

public void DetectedPlayer(bool yesOrNo)   // MotionSensor, обычный сеттер
{
    playerInRange = yesOrNo;
}
```

Оба когда-то были в списке патчей. `Sight` и `Hearing` замораживали NPC в
реакции, `DetectedPlayer` — датчик в сработавшем состоянии.

**Как проверить перед добавлением цели:**

```bat
dotnet tool install -g ilspycmd
ilspycmd -o out -t NPC "D:\...\Crime Simulator_Data\Managed\Assembly-CSharp.dll"
```

Дальше глазами: зовётся ли метод из `Update()` / `FixedUpdate()` / `LateUpdate()`
и есть ли в нём ветка, которая что-то обнуляет. Если да — искать вместо патча
штатное поле-опцию.

---

## Полиция

- `PoliceManager::RPC_PoliceCalled`
- `PoliceManager::TrySpawnPolice` — спавнит машины, дрон, вертолёт. Таймеры
  крутятся в `Update()` **до** вызова, так что заглушка ничего не ломает
- `PoliceManager::RPC_ForceUpdateLastPosition`, `RPC_UpdateLastPosition`
- `PoliceManager::RPC_UpdateMinimapArea`
- `NPC::CallBackup`, `NPC::TrySpawnPolice`
- `PoliceDrone::RPC_Yell`

На переходе выкл → вкл уже начавшаяся погоня снимается **один раз** штатным
`RPC_CallOffCops`. Никаких циклов сброса alert-полей.

## Гражданские

Поля-опции на каждом NPC (обычные `public bool`, не networked):

- `isBlind` — зрение
- `isDeaf` — слух
- `ignoreEverything` — ранний выход из `Hearing()`, в `Sight()` помогает лишь
  частично, поэтому только как дополнение
- `dontNoticeStolenItems`, `dontNoticeDeadPeople`, `dontNoticeOpenDoors`

Перебираются только активные объекты сцены. Волатильные `seesPlayer`,
`hearsPlayer`, `disturbed`, anger, combat и `callingCops` не трогаются вообще.

События:

- `NPC::RPC_NoticedStolenItem`
- `FPP_Player::RPC_WasHeardMessage`
- `StolenItemCollider::OnTriggerEnter`, `AlertPosessionNPCs`, `DetectedNewLoot`
- `PlayerPosessionCollider::OnTriggerEnter`

`OnTriggerExit` специально не трогается: он флаг **снимает**, а не ставит.
Заглушка на нём оставила бы «игрок на моей территории» висеть навсегда.

## Усиленный режим

Включается, только когда одновременно выключены полиция и гражданские:

`RPC_AlertToLocation`, `RPC_PullOutWeapon`, `RPC_CheckWeapon`, `CheckIllegal`,
`CheckNoiseRange`, `TriggerAttack`, `AlertOtherTenants`, `AlertPosessionNPCs` —
каждый только если нашёлся и его можно безопасно вернуть.

## Камеры

`isDisabled` — Fusion `[Networked]` свойство, пишется **через сеттер**. Запись в
поле `_isDisabled` не делает ничего после `Spawned()`, см. ниже.

Плюс `RPC_TriggerAlarm`, `RPC_Mark`, `TriggerAlarm`.

## Датчики и сигнализации

`MotionSensor::RPC_TriggerAlarm`, `RPC_Mark`, `LaserAlarm::TriggerAlarm`,
`Alarm`, `CarController::RPC_TriggerAlarm`, `RPC_Alarm`, `RPC_WasSeenBySomeone`,
`ElectronicSafe::RPC_Alarm`, `Keypad::RPC_AlarmSound`, `RPC_BlockedAndAlarm`,
`RPC_TriggerAlarm`, `Openable::RPC_TriggerAlarm`,
`WindowShutter::RPC_AlarmOnDestroy`.

Плюс `TriggerSensor` / `TriggerAlarm` у `SensorDetector`, `DoorSensor`,
`ExclamationSensor`, `AttackSensor`, `SightSensor`, `DroneSensor`, если такие
классы вообще есть в сборке.

## Время

`enviro.Time.Settings` (класс `EnviroTime` из Enviro3.Runtime):

- `simulate` — остановка часов. Побочка: `NPC.Update` переключает распорядок дня
  по `prevHour != Time.hours`, так что со стоящими часами NPC навсегда остаются
  в том занятии, в котором их застали
- `cycleLengthInMinutes` — длина полных суток в реальных минутах, по умолчанию 10.
  Замедление умножает её на 8: часы идут, распорядок работает

## Игрок

- `blockAllNoise` — `[Networked]`, через сеттер. Игра сама каждый кадр обнуляет
  `noisePower` и `noiseRange`, пока флаг стоит
- `currentStamina` / `staminaCooldown` / `staminaRegenDelay` — обычные поля
- `damage` — обычное поле, накопленный урон, откатывается в ноль

---

## Fusion `[Networked]` свойства

Важная ловушка. Выглядит так:

```csharp
[Networked]
public unsafe bool isDisabled
{
    get { return ReadWriteUtilsForWeaver.ReadBoolean(base.Ptr + 1); }
    set {        ReadWriteUtilsForWeaver.WriteBoolean(base.Ptr + 1, value); }
}
private bool _isDisabled;   // только для стартовых значений из инспектора
```

Поле `_isDisabled` переливается в сетевое состояние один раз в
`CopyBackingFieldsToState()` при спавне. Писать в него после этого бессмысленно.

Такие поля в этой сборке: `SecurityCamera.isDisabled`, `MotionSensor.isDisabled`,
`FPP_Player.blockAllNoise`, `detectionDistance`, `noiseRange`, `isDead`.

Отличить просто — у них есть приватный дубль с подчёркиванием и атрибут
`[Networked]`.

---

## Потоки

`WorkerThread` не вызывает Unity API и не трогает Mono-объекты. Он только:

1. читает общую память;
2. находит и сабклассит окно игры;
3. шлёт приватные Win32-сообщения.

Весь `GameController` выполняется из `TrainerWndProc`, то есть на потоке окна.

Тяжёлый перебор объектов — раз в секунду, инвентарь — раз в 200 мс, мост шлёт
тики ~30 раз в секунду. Новый тик не отправляется, пока не забрали предыдущий,
иначе при фризе игры они копились бы в очереди.

Если Unity пересоздаст окно (смена режима экрана), worker перецепится к новому.
`GameController` при этом **не** уничтожается: его деструктор зовёт `RestoreAll`,
а это Mono-вызовы из worker-потока — ровно то, чего вся схема избегает.

---

## Heartbeat

Обе стороны пишут тик в общую память на каждой итерации.

Если лаунчер убили, а не закрыли, тики прекращаются. Hook замечает это за
`kHeartbeatTimeoutMs` (4 с) и делает ту же штатную выгрузку, что и кнопка
«Завершить». Раньше проверялся только флаг `launcherAlive`, и убитый через
диспетчер задач лаунчер оставлял DLL в игре с записанными патчами.

---

## Как пишется патч

Подавляются только методы с безопасным возвращаемым типом:

- `void` → `ret` (1 байт);
- `bool` → `xor eax,eax; ret` (3 байта, всегда false).

Остальное пропускается.

Включение и выключение — одна выровненная 8-байтовая запись, а не побайтовый
`memcpy`. Поток игры, который уже внутри метода, видит либо полностью оригинал,
либо полностью заглушку. Если адрес не выровнен или в те же 8 байт попадает
начало другого пропатченного метода — откат на обычное копирование только
изменившихся байт.

Повторный resolve уже известного ключа игнорируется: по адресу сейчас может
лежать заглушка, и мы запомнили бы её как «оригинал».

---

## Урон и смерть — не трогаем

Игнор это фильтр реакций, а не бессмертие для NPC. Патчер отказывается ставить
заглушку на `RPC_DealDamage`, `DealDamage`, `TakeDamage`, `ApplyDamage`,
`ReceiveDamage`, `Damage`, `Hit`, `OnHit`, `Die`, `Death`, `OnDeath`, `Kill`,
`Killed`, `OnKilled` — и пишет в лог `[SKIP-PROTECTED]`.
