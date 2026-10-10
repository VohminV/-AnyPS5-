# Таблица переноса: PS5/AGC → Vulkan / DirectX 12 / NVIDIA

Статусы: `CONFIRMED` (первоисточник открыт и проверен) · `PARTIAL` (часть подтверждена, часть — нет) · `HYPOTHESIS` (вывод без прямого источника) · `UNKNOWN` (закрытый SDK, нет данных). Имена закрытых функций AGC и номера регистров НЕ приводятся.

## Легенда источников

- [V-Spec] https://docs.vulkan.org/spec/latest/ · [V-Guide:*] разделы гайда ·
  [SPIRV-Reg] https://registry.khronos.org/SPIR-V/ ·
  [V-Samples:*] https://github.com/KhronosGroup/Vulkan-Samples ·
  [MS-D3D12:*] https://learn.microsoft.com/en-us/windows/win32/direct3d12/* ·
  [NV-Nsight] https://docs.nvidia.com/nsight-graphics/ ·
  [AMD-ISA] ROCm-хаб + прямые PDF (RDNA2 PDF проверен gateway-запросом: 291 стр., AMD, 2020-12-01; текст не извлекался) ·
  [Local] замерено на этой машине (`vulkaninfo`, реестр, ФС).

## A. Очереди, submission, синхронизация

| Механизм | Vulkan (аналог) | DirectX 12 (аналог) | NVIDIA-инструменты | Семантические различия | Синхронизация/память | Проверка | Статус |
|---|---|---|---|---|---|---|---|
| Command buffers + queues | `VkCommandBuffer` + `VkQueue` (graphics/compute/transfer families) [V-Spec] | Command lists/bundles + `ID3D12CommandQueue` (DIRECT/COMPUTE/COPY); runtime сериализует submission с нескольких потоков; незакрытый/невалидный список — вызов сбрасывается [MS-D3D12:executing-and-synchronizing-command-lists] | Nsight Graphics Capture показывает очередь/события покадрово | D3D12: bundles только из direct-списков, строгая валидация submission; Vulkan: явные queue families | Vulkan: барьеры (execution vs memory раздельно) + semaphore/fence; D3D12: `ID3D12Fence` (integer) + resource barriers с before/after, mismatch ловит debug layer | Validation + sync validation; RenderDoc/NSight event view | PARTIAL (модель очередей CONFIRMED с обеих сторон; отображение конкретных PS5-очередей — UNKNOWN) |
| Межочередное владение ресурсом | Release/acquire барьеры между queue families [V-Spec] | Ресурс в write-состоянии эксклюзивен за очередью; переход в read/COMMON перед доступом с другой очереди [MS-D3D12:*same*] | Sync validation / NSight барьеры | D3D12: состояния ресурса явные (COMMON/RTV/UAV/…), mismatch — ошибка debug layer; Vulkan: layout'ы + access masks | Явные release/acquire в обоих API | Sync validation; VUID/debug-layer mismatch | CONFIRMED |
| Точки ожидания CPU/GPU | Fence, timeline semaphore [V-Guide] | `ID3D12Fence` + `Signal`/`SetEventOnCompletion`; значение fence = единица работы [MS-D3D12:*same*] | NSight GPU Trace (как CPU ждёт GPU) | Эквивалентны по выразительности | Fence-wait на CPU; timeline для сложных графов | Поведенческий тест lock-step; демо 07 (план) | CONFIRMED |
| Indirect draw/dispatch | `vkCmdDrawIndirect*`/`vkCmdDispatchIndirect` + `drawIndirectCount` (вариант) | `ExecuteIndirect` + command signatures | NSight показывает indirect args | Подсчёт вариантов и ограничения структур различаются | Аргументы — обычный GPU-буфер: те же барьеры writer→INDIRECT_COMMAND_READ | Валидация VUID (демо нет) | PARTIAL |
| Bundles (повторное использование записи) | Нет прямого аналога (вторичные буферы — nearest) | Bundles только из direct-списков, нельзя сабмитить напрямую; сброс аллокатора инвалидирует запись [MS-D3D12:*same*] | — | Модель повторного использования команд различается полностью | Аллокатор нельзя сбрасывать до завершения исполнения | VUID submit-time | PARTIAL |

## B. Память, residency, lifetime

| Механизм | Vulkan (аналог) | DirectX 12 (аналог) | NVIDIA-инструменты | Семантические различия | Синхронизация/память | Проверка | Статус |
|---|---|---|---|---|---|---|---|
| Выделение/пулы | `VkDeviceMemory` + sub-allocation (лимит `maxMemoryAllocationCount`) [V-Guide] | Heaps: committed/placed/reserved; выравнивания 64KB (4MB MSAA, 4KB small) [MS-D3D12:residency] | NSight memory view; `memory_budget` семпл | D3D12: heap — минимальная единица residency; placed нельзя переместить напрямую; reserved — только при tiled tier | Бюджет/пулы: UMA один пул, discrete два; D3D12: `QueryVideoMemoryInfo`, приоритеты `SetResidencyPriority` | `vulkaninfo` heaps/types [Local]; бюджет DXGI для D3D12 | PARTIAL |
| Residency/эвикция | Нет явного API (драйвер+ОС) | `MakeResident`/`Evict`, приоритеты, demote при давлении; без бюджета — фризы/ошибки создания [MS-D3D12:residency] | NSight memory pressure | В Vulkan residency неявный; в D3D12 явный и частично управляемый | Следить за бюджетом, уничтожать неиспользуемое ~каждый кадр | Мониторинг VRAM под нагрузкой | PARTIAL |
| Повторное использование (reuse) | На совести приложения: версии, поколения, инвалидация (см. ниже кэши) | Переиспользование placed-ресурсов требует новых дескрипторов на новое место [MS-D3D12:residency] | RenderDoc history ресурсов | Оба API требуют от приложения отслеживать «кто владеет диапазоном сейчас» | Поколения/сериалы + инвалидация при free; барьеры перед повторным чтением | Юнит-тесты reuse (как портированные upstream-тесты) | HYPOTHESIS (механизм общий; конкретный баг — по данным прогонов, не по докам) |
| Host-visible/import | `VkImportMemoryWin32HandleInfoKHR`; coherence: flush/invalidate для non-coherent [V-Guide/External] | Shared heaps/handles (`D3D12_HEAP_FLAG_SHARED`); CPU-доступ с write-combine осторожностью | NSight memory | Модель владения задаёт приложение в обоих API | Парный fence/semaphore на передачу (Vulkan-гайд); явные точки чтения/записи | Демо 04/06 (исходники готовы, не собраны) | PARTIAL |
| Sparse/tiled (резерв под стриминг) | Sparse resources (отдельная глава гайда, здесь не читалась) | Reserved resources при tiled tier ≥1; `CopyTileMappings`/`UpdateTileMappings` на очереди [MS-D3D12:*same*] | — | D3D12 tile-модель богаче и прямо документирована; Vulkan sparse — отдельно | Маппинги меняются на очереди; синхронизация как у копий | Не покрыто демо | HYPOTHESIS |

## C. Дескрипторы, адреса, root-константы

| Механизм | Vulkan (аналог) | DirectX 12 (аналог) | NVIDIA-инструменты | Семантические различия | Синхронизация/память | Проверка | Статус |
|---|---|---|---|---|---|---|---|
| Таблицы дескрипторов | Descriptor sets/pools/layouts, indexing, buffer/heap варианты [V-Guide] | Descriptor heaps + tables; root signature связывает списки с ресурсами [MS-D3D12:root-signatures-overview] | NSight descriptor inspection | D3D12: драйвер версионирует root state на каждый draw/dispatch автоматически | Обновление наборов до записи команд, использующих их | Validation (дескрипторы); ревизия ключей кэша | PARTIAL |
| Inline-константы | Push constants (≤128+ байт, быстрые) [V-Guide] | Root constants: inline 32-битные значения в root arguments [MS-D3D12:*same*] | — | Прямой аналог; ограничение размера с обеих сторон | Часть командного потока, без дескрипторов | Поведенческий тест | CONFIRMED |
| Raw GPUVA в шейдерах | BDA: адрес ТОЛЬКО из `vkGetBufferDeviceAddress` [V-Guide/BDA; V-Samples:bda] | GPU virtual address (`D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT`; root descriptors без лимита размера → без OOB-проверок [MS-D3D12:root-signatures-overview]) | NSight (адреса в шейдерах видны) | D3D12 root descriptors: нет OOB-проверок вообще (как BDA без robustness); Vulkan BDA: то же через UB | Таблицы диапазонов + проверки в шейдере (наш BdaAbi-паттерн) | `spirv-dis` (Aligned/capabilities); fault-репортинг движка | PARTIAL (Vulkan/DX12 стороны CONFIRMED; GNM-источник адресов — UNKNOWN) |
| Descriptor indexing (bindless) | `descriptorIndexing`, non-uniform indexing [V-Guide] | SM 6.6 dynamic resources (по открытым данным; детально не изучалось здесь) | — | Проверять поддержкой устройства в обоих | Лимиты device | `vulkaninfo` [Local: true] | PARTIAL |

## D. Pipeline, шейдеры, SPIR-V

| Механизм | Vulkan (аналог) | DirectX 12 (аналог) | NVIDIA-инструменты | Семантические различия | Синхронизация/память | Проверка | Статус |
|---|---|---|---|---|---|---|---|
| Graphics/compute pipeline | `vkCreate*Pipelines`, layout, render pass / dynamic rendering [V-Tutorial] | PSO + root signature [MS-D3D12:programming-guide] | NSight Shader Profiler; Samples `pipeline_cache` | D3D12 связывает PSO с root signature; Vulkan — с layout+pass | Компиляция асинхронна от записи команд; кэш по UUID | Демо 02; `spirv-val`; время создания замерить | PARTIAL |
| SPIR-V: capabilities | `PhysicalStorageBufferAddresses`, mesh/barycentric capabilities [SPIRV-Reg] | DXIL shader models (здесь не изучались) | — | SPIR-V — единственный бэкенд нашего проекта (архитектурное ограничение, не пробел) | Capabilities обязаны совпадать с включёнными фичами устройства | `spirv-val` + `spirv-dis` | PARTIAL |
| Валидация шейдеров | `spirv-val` (неполон: one-sided error) [SPIRV-Tools] | DXIL validation (здесь не изучалась) | NSight Shader Debugger | Валидатор ловит структуру, не семантику | Гонять в debug-сборках | `spirv-val` по генерируемым модулям | PARTIAL |
| Кэш pipeline | `vkGetPipelineCacheData`, UUID, версионирование — на приложении [V-Guide; Samples:pipeline_cache] | D3D12: кэширование PSO — на приложении/драйвере (детально не изучалось) | — | Форматы несовместимы между API | Атомарная запись, проверка UUID/вендора | Существующая реализация версионирована (проверено ранее) | PARTIAL |

## E. Копии, барьеры, layout'ы

| Механизм | Vulkan (аналог) | DirectX 12 (аналог) | NVIDIA-инструменты | Семантические различия | Синхронизация/память | Проверка | Статус |
|---|---|---|---|---|---|---|---|
| Копии | `vkCmdCopyBuffer/Image*` с точными экстентами; OOB — UB [V-Guide/Robustness] | `CopyBufferRegion/CopyTextureRegion`; placed-оффсеты с выравниваниями [MS-D3D12:residency] | RenderDoc copy inspection | Оба требуют точности; валидация ловит только API-нарушения | Барьеры transfer-write → читатель | Демо 04 (исходники готовы); построчный аудит dim-math | PARTIAL |
| Layout/state transitions | `VkImageLayout` + барьеры (execution vs memory раздельно) [V-Guide/Sync] | Resource states + barriers с before/after; mismatch ловит debug layer [MS-D3D12:executing…] | Sync validation / D3D debug layer | D3D12 проверяет before/after строже на уровне API; Vulkan — через access masks | Каждый переход: стадия+доступ+layout с обеих сторон | Демо 03/08; sync validation | PARTIAL |
| Depth/stencil | aspects, layout'ы, `VK_FORMAT_D*` [V-Spec] | DSV, depth-stencil states | — | Форматы PS5↔VkFormat — только через тесты | Те же барьеры | Поведенческие тесты | HYPOTHESIS |

## F. Диагностика

| Механизм | Vulkan (аналог) | DirectX 12 (аналог) | NVIDIA-инструменты | Проверка | Статус |
|---|---|---|---|---|---|
| API-валидация | `VK_LAYER_KHRONOS_validation` + sync validation (отдельно включается) | D3D12 debug layer (+ строгий match before/after) | — | Прогон с обоими | PARTIAL (слоя нет локально — зафиксировано) |
| Покадровый разбор | RenderDoc (Vulkan/D3D11/D3D12; BDA caveat: нужен capture-replay) | RenderDoc / PIX (PIX здесь не изучался) | NSight Graphics Capture | Захват проблемного кадра | PARTIAL |
| GPU-краш | `VK_EXT_device_fault` (Samples `device_fault`) | NSight Aftermath / GPU Crash Dumps [NV-Nsight] | NSight Aftermath | Настроить до аварии | HYPOTHESIS |
| Профилирование | `vkCmdWriteTimestamp`, pipeline statistics; Samples (command_buffer_usage и др.) | NSight GPU Trace Profiler, occupancy [NV-Nsight] | NSight | Замеры после стабильности | HYPOTHESIS |

## G. PS5-специфика (статусы честности)

| Тема | Статус | Комментарий |
|---|---|---|
| PrimitiveType enum GNM | UNKNOWN | Значения закрыты; не додумывать |
| Регистр primitive restart | UNKNOWN | Vulkan-сторона открыта (listRestart), GNM — закрыта |
| V# дескриптор буфера (base/stride/records/format/type) | CONFIRMED | Поля ISA Table 37 (§8.1, RDNA2 PDF) совпадают с декодером 1:1: base 47:0, stride 61:48, records 95:64, format 114:108, type 127:126==0, all-zero=unbound. Детали — `rdna2_notes.md` |
| PM4-пакеты команд, остальные поля дескрипторов | UNKNOWN | Только clean-room наблюдения + тесты |
| OOB vertex fetch на PS5 (fault vs мусор) | UNKNOWN | Определяет, обязан ли эмулятор падать |
| RDNA ISA (декодирование шейдеров) | PARTIAL | Документы AMD публичны: хаб ROCm + RDNA2 PDF проверен (291 стр., AMD, 2020-12-01); текст не извлекался; ISA ≠ GNM API. Выбор RDNA2 ISA как первичного референса независимо подтверждён KytyPS5 (Developer Information) |
| Класс GPU PS5 (исполнительная модель) | PARTIAL | RDNA2: GFXIP 10.3, wavefront 32, LDS 128 KiB/CU, Infinity Cache, VGPR 512 KiB, SGPR 16 KiB — ROCm gpu-arch-specs (прочитана). Wavefront 32 согласуется с subgroupSize 32; остальное — для сверки лимитов/размеров, не для API |
| Выравнивание new/malloc 32 (PS5 Clang) | HYPOTHESIS | Из сообщения апстрим-фикса (вторичный источник) |
| Поведение Unity (jobs, splash, видео) | PARTIAL | docs.unity3d.com + руководство по GPU-crash'ам (чистый драйвер, очистка кэша, fallback API) |

## Вывод

Ни одна строка не утверждает прямого соответствия PS5→Vulkan/DX12 без источника.
Прямые аналоги с CONFIRMED/PARTIAL есть на уровне моделей API (очереди, fences,
root-константы, residency-модель, BDA-правила, robustness-таблица). Вся GNM-семантика —
UNKNOWN/HYPOTHESIS до поведенческих тестов. D3D12-колонка — только по открытым
MS Learn (без SDK проверить нечем: MSVC/Windows Kits отсутствуют — зафиксировано).
