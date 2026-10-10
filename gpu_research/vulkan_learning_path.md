# Vulkan Learning Path (для переноса графики PS5 → Vulkan)

Порядок от базы к темам, нужным нашему движку. У каждого шага — зачем он нам.

## Этап 0. База (обязательно)

1. **Khronos Vulkan Tutorial** — https://docs.vulkan.org/tutorial/latest/00_Introduction.html
   Треугольник → трансформации → текстуры → модели. База: Vulkan 1.4, dynamic rendering,
   timeline semaphores, Vulkan-Hpp RAII. Нам: общий язык API (наш бэкенд держит Vulkan 1.1 API
   + расширения; расхождение версий держать в уме).
2. **Vulkan Guide: Queues; WSI; Threading** — https://docs.vulkan.org/guide/latest/index.html
   Нам: одна/few queues, present modes (наш baseline — FIFO), threading-правила вызовов.

## Этап 1. Память (критично для нас)

3. **Guide / Memory Allocation** — sub-allocation, staging buffers, discrete vs UMA, лимиты
   (`maxMemoryAllocationCount`, см. также `docs/misc → memory limits` в Vulkan-Samples).
   Нам: staging→device-local путь загрузок текстур; учёт VRAM 8 ГБ.
4. **Guide / External Memory and Synchronization** — импорт через `VkImport*Info`,
   handle types (Win32!), парный fence/semaphore на передачу владения.
   Нам: наш host-import гостевой памяти — сверить модель владения и точки синхронизации.
5. **Samples: `host_image_copy`, `memory_budget`** — копирование без staging-буфера;
   учёт бюджета памяти.

## Этап 2. BDA — сердце нашего адресного моста

6. **Guide / Buffer Device Address + BDA Alignment** — адреса ТОЛЬКО из драйвера (+uint64
   смещения); `PhysicalStorageBuffer64`; нужен shaderInt64; `Aligned` — обещание;
   OOB через physical pointer — UB; кросс-стейдж — через uvec2/int64.
   Нам: сверить BdaAbi (таблица диапазонов guest→device + fault-репортинг) с этими правилами.
7. **Samples: `buffer_device_address`** — канонический пример + caveat про
   `bufferDeviceAddressCaptureReplay` для RenderDoc.
8. **SPIR-V Registry: PhysicalStorageBuffer** — `OpCapability PhysicalStorageBufferAddresses`,
   `OpMemoryModel PhysicalStorageBuffer64`, `OpLoad/OpStore ... Aligned N`.
   Нам: проверять, что генерирует наш SPIR-V бэкенд (через `spirv-dis`).

## Этап 3. Синхронизация (наш главный риск)

9. **Guide / Synchronization + Synchronization Examples** — execution vs memory dependencies,
   pipeline barriers, fence/semaphore/event; затем whitepaper LunarG по sync-валидации.
   Нам: сверить каждый барьер writeback-пути (что делает видимым, для кого, когда).
10. **Samples: `synchronization2`, `timeline_semaphore`, `pipeline_barriers`, `async_compute`**
    Нам: образцы authorship наших паттернов (очереди, reclamation, async compute).
11. Включить **synchronization validation** из Validation Layer на диагностическом прогоне.

## Этап 4. Дескрипторы и robustness (наш второй риск)

12. **Guide / Descriptor indexing (+ ext-страница), Descriptor Buffer/Heap, Push Constants,
    Mapping Data to Shaders** — модели привязки; push constants могут нести указатели
    (тогда их не ловит никакая дескрипторная валидация!).
    Нам: сверить кэширование дескрипторных наборов и пути обновления адресов.
13. **Guide / Robustness** — таблица гарантий: robustBufferAccess (только буферы, OOB-записи
    МОГУТ менять данные) / robustBufferAccess2 (запрещает) / image OOB store+atomic = no-op /
    копии не покрыты / nullDescriptor. Нам: зафиксировать, на что мы вправе полагаться
    (почти ни на что при OOB-копиях).
14. **Samples: `descriptor_management`, `push_descriptors`, `descriptor_buffer_basic`**.

## Этап 5. Pipeline и шейдеры

15. **Guide / Pipeline Cache** + **Samples: `pipeline_cache`** — persist/restore, UUID,
    версионирование. Нам: сверить существующую реализацию (уже версионирована).
16. **SPIRV-Tools**: `spirv-val` (валидатор неполон — one-sided error, но обязателен для
    генерируемого SPIR-V в debug), `spirv-dis` (инспекция), `spirv-opt -O/-Os` (рецепты).
    Получение: LunarG SDK. Нам: прогнать `spirv-val` по генерируемым модулям.
17. **Samples: `fragment_shader_barycentric`, `conservative_rasterization`, `mesh_shading`,
    `dynamic_rendering`** — по одному на каждую продвинутую фичу, которую требует игра.
18. Туториал-курсы: **Synchronization**, **Advanced Vulkan Compute** (для compute-путей детайла).

## Этап 6. Диагностика

19. **Guide / Validation Overview** + LunarG docs (khronos_validation_layer, best practices,
     vkconfig) — VUID explicit/implicit, special-use теги.
20. **RenderDoc** (https://renderdoc.org/): захват кадра → ресурсы → барьеры → шейдеры;
     Python API для серийных захватов. Caveat BDA см. выше.
21. **Nsight Graphics** (https://docs.nvidia.com/nsight-graphics/, v2026.3): Graphics Capture,
     GPU Trace Profiler, Shader Debugger/Profiler, GPU Crash Dumps (Aftermath), SDK для
     программной инъекции — после стабильности, для RTX 3050.
22. **Samples: `device_fault`** (`VK_EXT_device_fault`) — штатный разбор GPU-крашей драйвером.

## Этап 7. DirectX 12 — сравнительный маршрут (без сборки здесь: нет MSVC/Windows SDK)

23. **MS Learn: programming guide → executing-and-synchronizing-command-lists → residency →
     root-signatures-overview** — очереди/fences, бюджет/пулы/приоритеты, root-константы/
     дескрипторы/таблицы (без OOB-проверок у root descriptors!).
24. Сопоставить по `ps5_vulkan_mapping.md` (колонка DX12): что переносится 1:1
     (fences, root constants, residency-модель), а что — нет (heap tiers, explicit states).
25. D3D12-демо НЕ писать здесь (нечем собрать); при появлении Windows SDK — зеркала демо
     02/03/07 на D3D12 для сверки барьеров и residency-поведения.

## Контрольные вопросы после прохождения

- Где в нашем writeback-пути execution dependency, а где memory dependency? Чего не хватает?
- Какой VkBuffer/VkDeviceAddress видит шейдер для гостевого диапазона X и откуда он взялся?
- Что произойдёт при OOB в каждой нашей копии (ответ: UB — значит, экстенты обязаны быть точными)?
- Что проверяет валидация, а что принципиально не видит (семантика копий, гостевая логика)?
