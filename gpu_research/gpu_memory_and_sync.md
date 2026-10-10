# Память и синхронизация GPU: руководство

Собрано из Guide/Synchronization, Guide/Memory Allocation, Guide/External Memory,
Guide/BDA (+Alignment), Guide/Robustness, Sample `buffer_device_address`, SPIRV-Tools.
Каждый пункт — что говорит источник и что это значит для переноса графики.

## 1. Хост-видимая память: coherence

- Память бывает `HOST_VISIBLE` + (`HOST_COHERENT` или нет). Для **non-coherent** хост обязан
  вызывать `vkFlushMappedMemoryRanges` после записи и `vkInvalidateMappedMemoryRanges`
  перед чтением — иначе CPU и GPU видят разное. (Spec, раздел memory.)
- Для дискретной GPU (наша RTX 3050) путь CPU→GPU идёт через staging-буфер и
  `vkCmdCopy*` (Guide/Memory Allocation: PCIe — узкое место, dedicated transfer queue
  при наличии `VK_QUEUE_TRANSFER_BIT`).
- **Применение к нам**: любой прямой CPU-доступ к результатам GPU (снапшоты, readback)
  обязан идти через барьер + доступность (`VK_ACCESS_HOST_READ_BIT`) и учитывать
  coherence импортов; «прочитать сразу после записи» без них — UB с тихими неверными данными.

## 2. Внешняя (импортированная) память

- Импорт: `VkImportMemoryWin32HandleInfoKHR` (Windows) / FD (POSIX) в `pNext` цепочке
  `VkMemoryAllocateInfo` → `VkDeviceMemory` поверх чужой памяти. (Guide/External.)
- Память и синхронизация — РАЗНЫЕ наборы расширений: к импорту обычно прилагается парный
  fence/semaphore, управляющий передачей владения между API.
- **Применение к нам**: наш host-import гостевой памяти обязан иметь явные точки
  владения (кто пишет: CPU-стор, GPU-копия, шейдер; кто читает следом) и fence/semaphore
  там, где владение пересекает границу CPU/GPU. Отсутствие точки = тихая порча.

## 3. Барьеры: execution vs memory dependencies

- `vkCmdPipelineBarrier` (и sync2) задаёт ДВЕ вещи раздельно: какие стадии ждут
  (execution dependency) и какие записи становятся видимыми каким чтениям
  (memory dependency через access masks). (Guide/Synchronization + whitepaper LunarG.)
- Частая ошибка: поставить execution-зависимость без нужных access masks — GPU ждёт,
  но читает старое. Вторая: пропустить layout transition (UNDEFINED → нужное).
- **Применение к нам**: каждый барьер writeback-пути проверять по трём вопросам:
  1) какая запись (srcStage+srcAccess) 2) кому видна (dstStage+dstAccess) 3) layout до/после.
  Проверка — reading кода + synchronization validation (см. §6).

## 4. BDA: правила указателей

- Адрес — только из `vkGetBufferDeviceAddress` (+ смещение как uint64). Сырой гостевой
  адрес отдавать шейдеру НЕЛЬЗЯ — нужен перевод через таблицу (наш BdaAbi-дизайн
  соответствует образцу; Sample подтверждает: «адреса только из драйвера»).
- SPIR-V: `OpCapability PhysicalStorageBufferAddresses`, `OpMemoryModel
  PhysicalStorageBuffer64`, все `OpLoad/OpStore` через physical pointer — с `Aligned N`.
- `Aligned` — обещание: неверное значение = UB (miscompiled vector-доступы; Guide отмечает,
  что часть GPU выполняет невекторные/невыровненные доступы корректно, полагаться на это нельзя).
- Через physical pointer НЕТ robustness: OOB = UB, валидация не ловит семантику.
  Таблицы диапазонов с проверками границ/прав — наша проектная компенсация этого пробела
  (не «рекомендованный Khronos паттерн», а следствие правил выше).
- Кросс-стейдж передача указателей — через uvec2/int64 (validation ругается на Location).
- Push constants МОГУТ нести указатели: их не покрывает дескрипторная валидация и
  дескрипторные кэши — отдельный путь аудита адресов.
- RenderDoc для BDA требует `bufferDeviceAddressCaptureReplay`, иначе повторы нестабильны.

## 5. Robustness: на что можно полагаться (таблица)

| Доступ OOB | Гарантия |
|---|---|
| Чтение/запись/атомик буфера, `robustBufferAccess` | чтение — 0/валидное; **запись/атомик МОЖЕТ менять данные** |
| То же, `robustBufferAccess2` | записи/атомики не меняют НИЧЕГО |
| Store/atomic в image OOB | **no effect** (core-гарантия) |
| Сэмпл OOB (robustImageAccess) | 0/edge по размеру view; невалидный LOD — UB |
| `vkCmdCopy*` OOB | **UB, ничем не покрыто** |
| null-дескриптор | только с фичей `nullDescriptor` (чтение 0, запись discard) |

**Применение к нам**: pitch-aligned stencil view (большой view поверх меньшего image) легален
именно благодаря image-robustness; а экстенты ВСЕХ копий обязаны быть точными всегда —
проверять кодом dim-Math построчно, слой этого не сделает.

## 6. Валидация: что ловит и чего не видит

- Ловит (VUID explicit/implicit): неверные хэндлы, layout, экстенты против image, барьерные
  ошибки API-уровня; sync-валидация — гонки доступа/отсутствующие зависимости.
- НЕ видит: семантику копий (правильные, но чужие адреса), гостевую логику (аллокаторы,
  время жизни), содержимое SPIR-V сверх валидатора, смысл push-constant указателей.
- Получение: LunarG SDK (Windows) или сборка из исходников; не шиппить в перф-сборках.
- Чистый лог валидации — необходимое, но НЕ достаточное условие корректности переноса.

## 7. SPIR-V и шейдеры

- Версии: unified spec 1.6 (наш SPIRV-Headers — 1.5.4-raytracing.fixed: отставшая, учесть).
- `spirv-val` — гонять по ВСЕМ генерируемым модулям в debug (валидатор неполон, но ловит
  структурные ошибки); `spirv-dis` — инспекция подозрительных модулей (BDA-адреса,
  Aligned, capabilities); `spirv-opt -O/-Os` — только по замерам.
- Наш SPIR-V бэкенд — единственный (SPIR-V only by construction): HLSL/DXIL пути нет,
  это архитектурное ограничение, а не пробел.

## 8. Диагностика GPU-крашей (лестница)

1. Validation + sync validation (дешёво, первым); D3D12-сторона: debug layer со строгим
   match before/after у барьеров.
2. `VK_EXT_device_fault` (образец — Samples `device_fault`): разбор драйверного краша;
   NVIDIA-аналог — Aftermath / GPU Crash Dumps в Nsight Graphics.
3. RenderDoc: покадровый захват → ресурсы/барьеры/шейдеры (+ caveat BDA).
4. Nsight Graphics (RTX): Graphics Capture, GPU Trace Profiler, Shader Profiler,
   occupancy, VRAM — после стабильности; residency-давление смотреть через
   `QueryVideoMemoryInfo`-подобный мониторинг (D3D12-модель) / `memory_budget` (Vulkan).
5. Ручная сверка барьеров по §3 настоящего файла.
