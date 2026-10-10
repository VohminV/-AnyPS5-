# План автономных Vulkan-демонстраций (исходники готовы, сборки нет)

Статус: исходники 8 демо + шейдеры + автономный `demos/CMakeLists.txt` НАПИСАНЫ и
сверены чтением (имена символов, layout push constants, ключи кэша — см. `RESULTS.md`).
Сборка и запуск ЗАБЛОКИРОВАНЫ отсутствием LunarG SDK. Ничего не скачано, не собрано
и не подключено к игровому проекту. Матрица статусов — в `demos/RESULTS.md`.

## Предлагаемые зависимости (Windows, WinLibs/MinGW-w64)

| Зависимость | Получение | Зачем |
|---|---|---|
| LunarG Vulkan SDK (≥1.4) | https://vulkan.lunarg.com/sdk/home — ручная установка | Заголовки, слои, `vulkaninfo`, `glslangValidator`, spirv-инструменты |
| CMake ≥3.22, Ninja | Уже есть в системе | Сборка демо |
| MinGW-w64 GCC 15.2 (WinLibs) | Уже есть (`C:\winlibs`) | Компилятор (тот же тулчейн) |
| Raw Win32 API (без SDL/GLFW) | Системные заголовки | Окно без лишних зависимостей |
| RenderDoc (опционально) | https://renderdoc.org/ | Проверка захватов демо |

## Демо 01. Устройство + очередь + слой валидации

Файл: `demos/01_device/…` (предлагается, не создан).
- `vkCreateInstance` с `VK_EXT_debug_utils` + `VK_LAYER_KHRONOS_validation` (только если слой
  найден через `vkEnumerateInstanceLayerProperties`, иначе — вежливый пропуск).
- Выбор physical device, очереди graphics/compute, `vkCreateDevice`.
- Цель: эталонный шаблон инициализации + проверка наличия слоя на этой машине.

## Демо 02. Графический pipeline + треугольник

Файлы: `demos/02_triangle/…` (предлагаются).
- GLSL → `glslangValidator -V` → SPIR-V; `spirv-val` до создания модуля.
- Render pass (или dynamic rendering), pipeline, vertex buffer, draw, present (FIFO).
- Цель: минимальный correctness baseline рендера + захват в RenderDoc.

## Демо 03. Текстура: staging → device → сэмплирование → экран

Файлы: `demos/03_texture/…` (предлагаются).
- Staging buffer (host-visible) → `vkCmdCopyBufferToImage` → `SHADER_READ_ONLY_OPTIMAL`.
- Барьеры transfer→fragment по руководству (execution + memory dependencies раздельно).
- Цель: эталон upload-пути для сверки с движком (наши загрузки текстур).

## Демо 04. Host-visible import + CPU/GPU когерентность

Файлы: `demos/04_import/…` (предлагаются).
- `VkImportMemoryWin32HandleInfoKHR` поверх участка host-памяти; compute shader пишет,
  CPU читает через `vkInvalidateMappedMemoryRanges` (non-coherent случай) и наоборот
  через `vkFlushMappedMemoryRanges`.
- Цель: эталон модели владения нашей host-import памяти (см. `gpu_memory_and_sync.md` §1–2).

## Демо 05. BDA треугольник (push constant + device address)

Файлы: `demos/05_bda/…` (предлагаются).
- `SHADER_DEVICE_ADDRESS_BIT` + `DEVICE_ADDRESS_BIT`; адрес из `vkGetBufferDeviceAddress`;
  передача через push constants; `Aligned` в шейдере; `spirv-dis` контроль.
- Цель: эталон BDA-пути для сверки BdaAbi (таблица диапазонов, fault-репортинг).

## Демо 06. Намеренные ошибки под слоем валидации

Файлы: `demos/06_negative/…` (предлагаются).
- OOB-копия, пропущенный барьер, неверный layout — каждое по одному запуску под
  validation + sync validation; зафиксировать тексты VUID.
- Цель: словарь «как выглядят наши классы ошибок в слое» для будущей диагностики.

## Сборка (когда будет SDK — вручную)

```powershell
cmake -S gpu_research/demos -B gpu_research/demos/build -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build gpu_research/demos/build
```

Каталог `demos/build/` — в игнор, в репозиторий не коммитить. Демо НЕ добавлять
в корневой CMakeLists игрового проекта.
