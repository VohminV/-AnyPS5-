# Источники: каталог

Легенда доступности: ✅ проверено локально/открыто · ⬇️ требует установки (не ставилось) · ❌ отсутствует локально · 📖 онлайн-документ.

## 1. Официальная документация Khronos (первоисточники)

| # | Документ | Ссылка | Конкретный раздел и чему учит | Доступность |
|---|---|---|---|---|
| 1 | Vulkan Specification (latest) | https://docs.vulkan.org/spec/latest/index.html | Valid Usage (VUID) после каждой функции/структуры; главы memory, synchronization, robustness, BDA | 📖 онлайн |
| 2 | Vulkan Guide | https://docs.vulkan.org/guide/latest/index.html | Лёгкое введение + ссылки: sync, memory allocation, BDA, BDA alignment, robustness, pipeline cache, descriptor indexing, external memory | 📖 изучен частично (см. ниже) |
| 3 | — Guide / Synchronization | https://docs.vulkan.org/guide/latest/synchronization.html | Execution vs memory dependencies, pipeline barriers, fence/semaphore/события; ссылки на whitepaper по sync-валидации | 📖 изучен |
| 4 | — Guide / Memory Allocation | https://docs.vulkan.org/guide/latest/memory_allocation.html | Sub-allocation, staging buffers, discrete vs UMA, LAZILY_ALLOCATED | 📖 изучен |
| 5 | — Guide / Buffer Device Address | https://docs.vulkan.org/guide/latest/buffer_device_address.html | BDA только из `vkGetBufferDeviceAddress`; SPIR-V `PhysicalStorageBuffer64`; нужен shaderInt64; `OpConstantNull` нельзя для physical pointer; кросс-стейдж — через uvec2/int64 | 📖 изучен |
| 6 | — Guide / BDA Alignment | https://docs.vulkan.org/guide/latest/buffer_device_address_alignment.html | `Aligned` — обещание; нарушение = UB; раскладка host-структур обязана совпадать с шейдерной | 📖 изучен |
| 7 | — Guide / Robustness | https://docs.vulkan.org/guide/latest/robustness.html | robustBufferAccess — только буферы, OOB-записи МОГУТ менять данные; robustBufferAccess2 — запрещает; OOB store/atomic в image — no-op; OOB в копиях не покрыт; nullDescriptor | 📖 изучен |
| 8 | — Guide / External Memory and Synchronization | https://docs.vulkan.org/guide/latest/extensions/external.html | Память и синхронизация — разные наборы расширений; импорт через `VkImport*Info` в `vkAllocateMemory`; handle types Win32/POSIX/AHWB; к импорту обычно нужен парный fence/semaphore | 📖 изучен |
| 9 | — Guide / Validation Overview | https://docs.vulkan.org/guide/latest/validation_overview.html | VU/VUID (explicit vs implicit), единый `VK_LAYER_KHRONOS_validation`, где брать слой (SDK), best-practices layer, special-use теги | 📖 изучен |
| 10 | Vulkan Tutorial (Khronos) | https://docs.vulkan.org/tutorial/latest/00_Introduction.html | База Vulkan 1.4: dynamic rendering, timeline semaphores, Slang, Vulkan-Hpp RAII; продвинутые курсы (sync, compute, engine) | 📖 оглавление изучено |
| 11 | SPIR-V Registry | https://registry.khronos.org/SPIR-V/ | Unified spec (HTML/PDF), грамматики JSON, заголовки SPIRV-Headers, реестр расширений и вендоров | 📖 индекс изучен |
| 12 | SPIRV-Tools (README/docs) | https://github.com/KhronosGroup/SPIRV-Tools | `spirv-as/dis/val/opt/link/diff/cfg/reduce/fuzz`: валидатор НЕПОЛОН (one-sided error); Khronos рекомендует валидировать генерируемый SPIR-V в debug | 📖 README изучен |
| 13 | Vulkan-Samples (repo + docs) | https://github.com/KhronosGroup/Vulkan-Samples и https://docs.vulkan.org/samples/latest/README.html | Практики: `buffer_device_address` (адреса только из драйвера, capture-replay caveat), `pipeline_cache`, `descriptor_management`, `device_fault`, `synchronization2`, `host_image_copy` | 📖 список + BDA-семпл изучены |
| 14 | Sync-validation whitepaper (LunarG) | https://www.lunarg.com/wp-content/uploads/2020/09/Final_LunarG_Guide_to_Vulkan-Synchronization_08_20.pdf (ссылка из гайда) | Как работает synchronization validation | 📖 ссылка зафиксирована, текст не читался |
| 15 | Vulkan Hardware Database | https://vulkan.gpuinfo.org/ | Проверка поддержки Vulkan/расширений конкретными GPU/драйверами | 📖 упомянут в туториале |

## 1b. Первоисточники DirectX 12 / NVIDIA / AMD (проверены в прод. миссии)

| # | Документ | Ссылка | Конкретный раздел и чему учит | Доступность |
|---|---|---|---|---|
| 29 | MS Learn: Residency (D3D12) | https://learn.microsoft.com/en-us/windows/win32/direct3d12/residency | Бюджет видеопамяти, heaps (committed/placed/reserved, выравнивания 64KB/4MB/4KB), приоритеты, MakeResident/Evict | 📖 изучен |
| 30 | MS Learn: Root Signatures Overview | https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signatures-overview | Root constants/descriptors/tables; у root descriptors НЕТ лимита размера → нет OOB-проверок (аналог BDA!); драйвер версионирует root state на каждый draw/dispatch | 📖 изучен |
| 31 | MS Learn: Executing and Synchronizing Command Lists | https://learn.microsoft.com/en-us/windows/win32/direct3d12/executing-and-synchronizing-command-lists | Типы очередей, fences как integer-единицы работы, правила доступа с нескольких очередей, барьеры before/after (mismatch ловит debug layer) | 📖 изучен |
| 32 | MS Learn: D3D12 Programming Guide (обзор) | https://learn.microsoft.com/en-us/windows/win32/direct3d12/directx-12-programming-guide | Точка входа в модель API | 📖 открыт (краткая страница) |
| 33 | NVIDIA Nsight Graphics Docs (v2026.3) | https://docs.nvidia.com/nsight-graphics/ | Activities: Graphics Capture, GPU Trace Profiler, Shader Debugger/Profiler, GPU Crash Dumps (Aftermath), SDK для программной инъекции | 📖 оглавление изучено |
| 34 | AMD RDNA ISA: хаб ROCm Docs + прямые PDF (проверено чтением хаба и gateway-запросом PDF) | Хаб: https://rocm.docs.amd.com/en/latest/reference/gpu-arch/rdna.html; RDNA2 PDF: https://gpuopen.com/download/rdna2-shader-instruction-set-architecture.pdf — ФАКТ: Title «"RDNA 2" Instruction Set Architecture: Reference Guide», Author AMD, 291 страница, ModDate 2020-12-01. Зеркало TechPowerUp существует, но отдаёт бот-проверку (не читалось). Микроархитектурные детали на хабе помечены «coming soon» | Система команд/состояние программ/волновые модели/иерархия памяти RDNA. Извлечены §§3.11, 7.5, 8.1–8.2 (см. `rdna2_notes.md`) | 📖 хаб + факт PDF проверены; ⚠️ остальное — по мере нужды |
| 35 | Vulkan Memory Model (Spec) | https://docs.vulkan.org/spec/latest/chapters/memory_model.html | Формальная модель памяти (атомарность, видимость, гонки) — не читалась здесь | 📖 ссылка зафиксирована, текст не читался |

## 1c. GPU-архитектура и открытые реализации (проверены в прод. миссии)

| # | Источник | Что установлено | Ограничения |
|---|---|---|---|
| 36 | ROCm Docs: Accelerator and GPU hardware specifications | https://rocm.docs.amd.com/en/docs-6.3.3/reference/gpu-arch-specs.html — таблица: RDNA2 = GFXIP 10.3, wavefront 32, LDS 128 KiB/CU, Infinity Cache, L0/L1 кэши, VGPR 512 KiB, SGPR 16 KiB; глоссарий (CU, wavefront, LDS, Infinity Cache, GFXIP) | 📖 прочитана. Факты об архитектуре, НЕ о PS5 API |
| 37 | KytyPS5 (открытый PS5 translation layer, GPL-2.0, ~7k stars) | https://github.com/KytyPS5/KytyPS5 — README/Developer Information: «PS5 graphics architecture is based on AMD RDNA 2», первичный референс кодировок — RDNA 2 ISA Doc 70648; структура `src/graphics/{shader/recompiler, guest_gpu, host_gpu}`; рендерер целит Vulkan 1.3; тесты `ctest`; shadPS4 — референс по memory behavior, GPU resource aliasing, cache coherency и AVPlayer | 📖 README прочитан. Вторичный источник: архитектура проекта и выбор референсов, НЕ копипаста кода |
| 38 | KytyPS5 AI Use policy (из того же README) | AI-допустим для research/reverse engineering/dev-assistance при полном человеческом ревью и раскрытии объёма AI-участия в PR | 📖 процедурный факт; релевантен нашему AI-воркфлоу |

## 2. Инструменты диагностики

| # | Инструмент | Источник/получение | Назначение | Доступность |
|---|---|---|---|---|
| 16 | Validation Layer (`VK_LAYER_KHRONOS_validation`) | LunarG SDK https://vulkan.lunarg.com/sdk/home (Windows) или сборка из https://github.com/KhronosGroup/Vulkan-ValidationLayers | Проверка VU, включая synchronization validation; НЕ видит логику приложения | ❌ не установлен (проверено: нет `VkLayer_*` в System32, нет ExplicitLayers в реестре) |
| 17 | Vulkan SDK (Windows) | https://vulkan.lunarg.com/sdk/home | Слои, `vulkaninfo`, `vkconfig`, spirv-инструменты из коробки | ❌ не установлен (каталога `C:\VulkanSDK\…` нет; в PATH осталась только запись) |
| 18 | `vulkaninfo` | Поставляется со SDK и драйвером | Дамп устройств/расширений/фич/лимитов | ✅ РАБОТАЕТ: instance 1.4.341, RTX 3050 / driver 616.92 / api 1.4.351; фичи true: shaderInt64, bufferDeviceAddress (+CaptureReplay), barycentric, pixelInterlock, meshShader, robustBufferAccess2/ImageAccess2, descriptorIndexing, timelineSemaphore, synchronization2 |
| 34a | MSVC / Windows Kits (для D3D12-сборок) | — | `cl`, `d3d12.h` | ❌ ОТСУТСТВУЮТ (`cl` нет, `Windows Kits\10\Include` нет) → D3D12-демо здесь несобираемы |
| 34b | MinGW-w64 GCC 15.2 / CMake / Ninja | Уже в системе | Сборка Vulkan-демо (C API, без MSVC) | ✅ есть |
| 34c | OBS Vulkan layer | След вулкана в реестре (`VK_LAYER_OBS_HOOK`, API 1.3) | Чужой implicit-слой есть; валидационного нет | ⚠️ зафиксировано `vulkaninfo`-предупреждением |
| 19 | RenderDoc | https://renderdoc.org/ (MIT, исходники на GitHub) | Покадровый захват Vulkan/D3D11/D3D12/GL, Python API, интеграция в Unity/Unreal; caveat: для BDA нужен `bufferDeviceAddressCaptureReplay` | ⬇️ не ставился; сайт и назначение изучены |
| 20 | Nsight Graphics (NVIDIA) | Сайт NVIDIA (проприетарный, бесплатный) | GPU-trace/профилирование именно для RTX; range-профилирование, occupancy | ⬇️ не ставился; зафиксирован как следующий шаг для RTX 3050 |
| 21 | SPIRV-Tools бинарники | В составе LunarG SDK | `spirv-val` для генерируемого SPIR-V, `spirv-dis` для инспекции, `spirv-opt -O` рецепты | ⬇️ только как часть SDK |

## 3. PS5/AGC-сторона: граница закрытости (факт)

| # | Материал | Состояние |
|---|---|---|
| 22 | Локальный PS5 SDK (`C:\Program Files (x86)\SCE`) | ❌ ПУСТОЙ каталог (проверено 2026-10-10: 0 файлов). Заголовков AGC, примеров pipeline, описаний регистров нет |
| 23 | Официальная документация PS5 SDK / AGC | ❌ Закрыта NDA Sony. В исследовании не используется и не цитируется |
| 24 | Имена NID-функций, номера регистров, битовые поля пакетов из закрытого SDK | ❌ Не приводятся (правило миссии). Всё недокументированное помечается UNKNOWN |

## 4. Легальные альтернативы для PS5-стороны

| # | Источник | Что даёт | Ограничения |
|---|---|---|---|
| 25 | Публичные ISA-документы AMD (GPUOpen/docs.amd.com): RDNA Shader Instruction Set Architecture | RDNA2 ISA Doc 70648 (2020-11-30) — поколение PS5 GPU; также 70652 (RDNA1), 70650 (RDNA3): waves, SGPR/VGPR, иерархия памяти, кодировки инструкций | ⚠️ Существование и место подтверждены поиском; PDF не читались. ISA ≠ GNM API |
| 26 | Открытые clean-room проекты эмуляции (упомянуты в самом репозитории, напр. TechnicalDebt: KytyPS5, shadPS4, OpenAGC, SharpProspero, fpPS4, RPCS3 для cellFont) | Независимые наблюдения поведения, сигнатуры, مقایسه реализаций | ⚠️ Вторичный источник: согласуется — хорошо, расходится — нужен тест, а не голосование |
| 27 | Документация Unity (docs.unity3d.com) | Поведение движка игры: job system, workers, splash/video-плеер, руководство по GPU-crash'ам (чистая установка драйвера, очистка кэша, fallback API) | ⚠️ Описывает движок, а не PS5-вызовы |
| 28 | Vulkan-документация целевой стороны (§1) | Проверяемые требования к нашей реализации (валидация, барьеры, BDA, robustness) | ✅ Достаточно для аудита нашей стороны |

## 5. Что уже проверено локально ранее (контекст, не часть этого исследования)

- `vkGetBufferDeviceAddress` + `shaderInt64` требуются движком обязательно (без тихого fallback).
- Swapchain FIFO захардкожен; pipeline cache версионирован (magic/format/checksum, vendor/device/UUID, атомарная запись).
- SPIRV-Tools подключены только к тестам (`ANYPS5_ENABLE_SPIRV_TOOLS`), не к рантайму.
- VMA в бэкенде не используется (сабмодуль есть, вызовов нет).
(Это итоги предыдущих аудитов исходников, а не утверждения Khronos — держать раздельно.)
