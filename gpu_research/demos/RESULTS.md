# Демо: статусы реализации, сборки и запусков

Честная матрица (три статуса раздельно — по требованию миссии).

| # | Демо | Гипотеза | Исходники | Собрано | Запущено | Результат |
|---|---|---|---|---|---|---|
| 01 | device check | RTX 3050 имеет все нужные фичи → fallback'ы не объясняют артефакты | `01_device/device_check.cpp` ✅ | ❌ нет SDK | ❌ | — |
| 02 | triangle | FIFO present-цикл стабилен сам по себе | `02_triangle/triangle.cpp` + `shaders/triangle.{vert,frag}` ✅ | ❌ | ❌ | — |
| 03 | texture upload | Канонический staging→optimal→sample даёт шахматку | `03_texture/texture_upload.cpp` + `shaders/textured.*` ✅ | ❌ | ❌ | — |
| 04 | buffer copies | Точные копии round-trip байт-в-байт | `04_copies/buffer_copies.cpp` ✅ | ❌ | ❌ | — |
| 05 | BDA round-trip | Адрес из драйвера + `Aligned 8` + push constants → паттерн сходится | `05_bda/bda_roundtrip.cpp` + `shaders/bda_write.comp` ✅ | ❌ | ❌ | — |
| 06 | coherence | flush/invalidate необходимы и достаточны (non-coherent) | `06_coherence/coherence.cpp` ✅ | ❌ | ❌ | — |
| 07 | queue sync | semaphore+fence+ownership transfer упорядочивают пути | `07_sync/queue_sync.cpp` ✅ | ❌ | ❌ | — |
| 08 | negative/validation | Классы ошибок дают точные VUID (словарь); без слоя — чистый SKIP | `08_negative/validation_dictionary.cpp` ✅ | ❌ | ❌ | — |

Общее: `common/win32_window.h` ✅, `CMakeLists.txt` ✅ (автономный проект, требует
`VULKAN_SDK` + `glslangValidator`; в игровой CMake НЕ подключён).

## Проверки исходников без SDK (выполнены)

- Все `.cpp` используют только Vulkan 1.1 API + Win32; имена `VK_*`/`vk*` сверены
  со спецификацией/гайдом (не по памяти).
- GLSL: `buffer_reference` требует `GL_EXT_buffer_reference` (включено в шейдере);
  push-constant layout C++ (16 байт) совпадает с GLSL (8+4+pad).
- CMake: имена символов `add_spv_header` согласованы с `extern`-объявлениями в `.cpp`
  (`kVertSpv`, `kFragSpv`, `kTexturedVertSpv`, `kTexturedFragSpv`, `kBdaWriteSpv` + `…Words`).
- Демо 08 намеренно НЕ содержит исполняемых OOB (только record-time нарушения под слоем).

## Блокировки сборки/запуска

1. Нет LunarG SDK (заголовки, `glslangValidator`) — установка только вручную.
2. Нет Validation Layer на машине — демо 08 даст SKIP (это тоже результат: зафиксировать).
3. Нет MSVC/Windows SDK — значения не имеет (демо только Vulkan/MinGW), но фиксирует
   невозможность D3D12-демо здесь (нужен `d3d12.h` из Windows SDK).

## Ожидаемые результаты (для будущих прогонов)

- 01: exit 0; все required-фичи `true` (сверено с `vulkaninfo`: BDA, int64, mesh,
  barycentric, interlock, descriptorIndexing, timeline, sync2, robustness2 — все true).
- 02/03: окно 300 кадров, exit 0.
- 04/05/06/07: `OK`, exit 0.
- 08 без слоя: `SKIP: validation layer absent`, exit 0. Со слоем: ≥1 VUID на кейс 1,
  сообщения sync-валидации на кейс 2 (зафиксировать тексты сюда же).
