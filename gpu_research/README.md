# GPU Research — PS5 SDK / AGC / Vulkan (+DX12/NVIDIA)

Отдельное техническое исследование для переноса графических операций PS5 (AGC/GNM)
на Vulkan (основной путь), DirectX 12 и NVIDIA-инструментарий (сравнение и диагностика).
Основано на общедоступной документации Khronos/Microsoft/NVIDIA/AMD, открытых репозиториях
и локальной инвентаризации машины разработчика (RTX 3050, driver 616.92, Vulkan 1.4.351).

## Строгие границы (соблюдены)

- Игровой проект НЕ изменён (нет коммитов, сборок, запусков игры в рамках исследования).
- Закрытые SDK-материалы НЕ копировались (локальный каталог SDK пуст — см. `sources.md`).
- Ничего не скачивалось и не собиралось автоматически.
- Факты отделены от выводов и гипотез в каждом документе.

## Состав пакета

| Файл | Содержание |
|---|---|
| `sources.md` | Каталог источников: официальные документы Khronos, инструменты, открытые проекты, локальная инвентаризация |
| `vulkan_learning_path.md` | Порядок изучения Vulkan от треугольника до BDA/robustness/validation |
| `ps5_vulkan_mapping.md` | Таблица соответствий PS5/AGC ↔ Vulkan (только подтверждённое; пробелы помечены) |
| `gpu_memory_and_sync.md` | Руководство: память, импорты, барьеры, BDA, robustness, валидация |
| `open_questions.md` | Неизвестное, нужные материалы, следующие шаги |
| `rdna2_notes.md` | Проверенные извлечения из RDNA2 ISA (violations, bounds, V# Table 37 + сверка декодера) |
| `demos/00_plan.md` + `demos/RESULTS.md` | План и честная матрица статусов 8 демо |
| `demos/01_device … 08_negative/` | Исходники демо + шейдеры + автономный CMake (в игровой CMake НЕ подключён; не собраны — нет SDK) |

## Статус проверки инструментов (факт, 2026-10-10)

- `vulkaninfo` работает: RTX 3050, driver 616.92, Vulkan 1.4.351; true — shaderInt64,
  bufferDeviceAddress (+CaptureReplay), meshShader, barycentric, pixelInterlock,
  descriptorIndexing, timelineSemaphore, synchronization2, robustBufferAccess2/ImageAccess2.
- Validation Layer отсутствует; LunarG SDK, glslangValidator, spirv-*, RenderDoc, Nsight,
  MSVC/Windows Kits — отсутствуют (D3D12-демо здесь несобираемы — зафиксировано).

## Ключевые итоги изучения (кратко)

1. **Vulkan — explicit API**: драйвер не проверяет вход (нет `glGetError`); корректность
   проверяется только Validation Layers. Чистый лог валидации ≠ корректность логики приложения
   (гостевой аллокатор, семантика SPIR-V, смысл копий валидация не видит).
2. **BDA — только из `vkGetBufferDeviceAddress` + смещения как uint64.** Сырые указатели не знают
   размера (нет robustness), `Aligned` — обещание компилятору, OOB через physical pointer — UB.
   Это прямо описывает модель нашего движка (таблица диапазонов guest→device с проверками в шейдере).
3. **Robustness**: `robustBufferAccess` — только буферы, и OOB-записи/атомики МОГУТ менять данные;
   `robustBufferAccess2` — запрещает любые OOB-модификации; записи/атомики OOB в изображения —
   no-op; OOB в **копиях** (`vkCmdCopy*`) не покрывается ничем.
4. **Импорт памяти**: память + синхронизация — отдельные наборы расширений; для импортируемой
   памяти обычно нужен парный fence/semaphore на передачу владения.
5. **SPIRV-Tools** (`spirv-val`, `spirv-dis`, `spirv-opt`): валидатор неполон («one-sided error» —
   ругается только на реализованные проверки), но Khronos рекомендует валидировать всё, что
   генерирует SPIR-V, особенно в debug-сборках.
6. **RenderDoc** (MIT): покадровый захват Vulkan/D3D11/D3D12/GL + Python API. Важно: для BDA нужен
   `bufferDeviceAddressCaptureReplay`, иначе адреса при повторе нестабильны.
7. **PS5 SDK закрыт**: локально (`C:\Program Files (x86)\SCE`) — пустой каталог, заголовков AGC нет.
   Легальные альтернативы: публичные ISA-документы AMD (RDNA2 Doc 70648 — ссылки проверены),
   открытые clean-room проекты, документация Unity, Vulkan-документация целевой стороны.
   Имена функций/регистры из закрытого SDK здесь НЕ приводятся.
8. **D3D12 (новое)**: residency (бюджет/пулы/приоритеты/MakeResident), root signatures
   (root descriptors — без OOB-проверок, как BDA), fences как integer-единицы работы,
   строгий match before/after у барьеров. MSVC/Windows Kits на машине НЕТ — D3D12-демо
   здесь несобираемы (зафиксировано).
9. **Устройство проверено**: RTX 3050 имеет все нужные фичи (BDA+CaptureReplay, int64, mesh,
   barycentric, interlock, descriptorIndexing, timeline, sync2, robustness2) — см. sources.md.

## Как пользоваться пакетом

1. Прочитать `sources.md` (что доступно прямо сейчас на этой машине).
2. Пройти `vulkan_learning_path.md` по порядку.
3. Смотреть конкретные механизмы в `gpu_memory_and_sync.md`.
4. Сопоставления — только через `ps5_vulkan_mapping.md` (не додумывать).
5. Новое неизвестное — дописывать в `open_questions.md`, а не в код.
