# Анализ: Уход от SPIR-V в AnyPS5 shader рекомпиляторе

## Вывод

**Полный отказ от SPIR-V в Vulkan невозможен.** `vkCreateShaderModule` принимает только SPIR-V код. Vulkan API не предоставляет способа загрузить нативный GPU машинный код (RDNA2 ISA, NVIDIA SASS, Intel Gen ISA) напрямую.

SPIR-V — это обязательный промежуточный формат для всех shaderステージ в Vulkan.

## Текущий конвейер компиляции

```
RDNA2 бинарник (PS5 shader)
    ↓
RdnaDecoder (RdnaDecoder/src/*.cpp)
    ↓
GraphBuilder (ControlFlow/src/GraphBuilder.cpp) — строит CFG
    ↓
Structurizer (ControlFlow/src/Structurizer.cpp) — структуризация циклов/условий
    ↓
InstructionTranslator (Translation/src/InstructionTranslator.cpp) — RDNA→IR
    ↓
Optimization (Optimization/src/*.cpp) — SSA, DCE, ресурсы, привязки
    ↓
SpirvBackend (SpirvBackend/src/SpirvModuleEmitter.cpp) — эмиссия SPIR-V
    ↓
VkShaderModule (VulkanDevice.cpp:3703, Pipeline.cpp:157, и т.д.)
    ↓
VkPipeline (vkCreateGraphicsPipelines / vkCreateComputePipelines)
```

## Что уже сделано (п reducción нагрузки)

### 1. AOT Shader Cache (`core/shader/recompiler/AotCache.{hpp,cpp}`)
- Ключ: `hash(raw RDNA2 бинарник + static pipeline layout)`
- Файлы: `aot_shader_cache/<hash>.avc`
- Сжатие: `re-spirv -O --eliminate-dead-code-aggressive` (940 КБ → ~15 КБ)
- Kill switch: `APS5_NO_AOT_CACHE=1`
- Валидация: проверка магического числа SPIR-V `0x07230203` при загрузке

### 2. Shader Disk Cache (`core/shader/recompiler/ShaderDiskCache.cpp/.hpp`)
- Ключ: полный ключ шейдера (код + layout + specialization + target)
- Файлы: `shader_cache/<hex>.bin`
- Формат v22 с валидацией хеша ключа и полезной нагрузки

### 3. Исправления исправлены в этом сеансе
- `findOrCompileVariant`: синхронизация `layout`/`pushConstant*` при AOT хите
- `IsAotLayoutCompatible`: проверка совместимости `IrBindingLayout.UsesPushData()` с текущим `request.pushConstantSizeBytes`
- `AotCache::Lookup`: проверка магического числа SPIR-V при загрузке
- `RecompileImpl`: та же проверка совместимости для AOT fast path

## Что можно сделать дальше (реалистичный план)

### Этап 2: VK_EXT_shader_object (Vulkan 1.4 / EXT)

**Проблема:** Каждый вызов `vkCreateGraphicsPipelines`/`vkCreateComputePipelines` — дорогая операция (10-30мс на shader).

**Решение:** Использовать `VK_EXT_shader_object` для создания shader объектов без создания pipeline:

```cpp
// Вместо:
// vkCreateShaderModule → vkCreateGraphicsPipelines

// С VK_EXT_shader_object:
VkShaderCreateInfoEXT shaderInfo{
    VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
    nullptr,
    VK_SHADER_STAGE_FRAGMENT_BIT,
    nextStage,  // 다음 stage или 0 для последнего
    spirvCodeSize,
    spirvCode
};
vkCreateShadersEXT(device, 1, &shaderInfo, nullptr, &shaderObject);

// Затем в pipeline:
VkPipelineCreateInfoEXT pipelineInfo{
    VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO_EXT,
    ...,
    .stageCount = 1,
    .pStages = &(VkPipelineShaderStageCreateInfoEXT){
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO_EXT,
        nullptr,
        shaderObject,
        "main"
    }
};
```

**Файлы для изменения:**
- `core/libs/prx/libSceAgcDriver/Execution/src/VulkanDevice.cpp` — `vkCreateShaderModule` → `vkCreateShadersEXT`
- `core/libs/prx/libSceAgcDriver/Graphics/src/Pipeline.cpp` — графические pipeline
- `core/libs/prx/libSceAgcDriver/Graphics/src/Recorder.cpp` — compute pipeline

**Эффект:** Снижение накладных расходов на pipeline creation, но SPIR-V всё ещё требуется.

### Этап 2б: VK_EXT_graphics_pipeline_library

**Проблема:** Полная пересборка pipeline при изменении одного shaderステージ.

**Решение:** Разделить pipeline на библиотеки (vertex, fragment, etc.) и кэшировать каждую отдельно:

```cpp
VkGraphicsPipelineLibraryCreateInfoEXT libInfo{
    VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
    nullptr,
    VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_BIT_EXT |
    VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT |
    ...
};
```

**Файлы для изменения:**
- `core/libs/prx/libSceAgcDriver/Graphics/src/Pipeline.cpp`
- `core/libs/prx/libSceAgcDriver/Execution/src/VulkanDevice.cpp`

**Эффект:** Быстрая пересборка pipeline при изменении только одногоステージ.

### Этап 3: Предкомпиляция SPIR-V офлайн

**Проблема:** Первый запуск игры требует компиляции ~2600 шейдеров.

**Решение:** Предкомпилировать популярные шейдеры офлайн и поставлять готовые `.avc` файлы с игрой:

```
aot_shader_cache/
    <hash>.avc  — предкомпилированный + re-spirv сжатый SPIR-V
```

**Файлы для изменения:**
- Добавить скрипт предкомпиляции в `tools/`
- Добавить проверку наличия предкомпилированного кэша при запуске

**Эффект:** Запуск без shader storm на первый запуск.

### Этап 4 (долгосрочно): Нативный бэкенд

**Проблема:** SPIR-V компиляция — bottleneck (10+ секунд на grandeshaders).

**Решение:** Написать бэкенд, который генерирует нативный GPU код напрямую из IR, минуя SPIR-V:

```
IR → NativeBackend → Машинный код цели (RDNA2/SASS/Gen) → Прямая загрузка в GPU
```

**Проблемы:**
- Vulkan не поддерживает загрузку нативного кода без SPIR-V
- Требуется поддержка каждого целевого GPU (AMD RDNA2, NVIDIA SASS, Intel Gen)
- Нужен отдельный бэкенд для каждого архитектуры

**Альтернатива:** Расширить `CpuBackend/` (уже есть в проекте) для полной эмуляции шейдеров на CPU:

```
IR → CpuBackend → Машинный код хост CPU → Выполнение на CPU
```

**Файлы:**
- `core/libs/prx/libSceAgcDriver/Execution/src/CpuBackend/` — уже существует
- `core/shader/recompiler/SpirvBackend/` — заменить на `NativeBackend/`

**Эффект:** Полный отказ от Vulkan shader компиляции, но потеря производительности GPU.

## Заключение

| Подход | Требует SPIR-V? | Сложность | Ожидаемый эффект |
|--------|----------------|-----------|-----------------|
| AOT кэш (уже сделано) | Да | Низкая | Быстрая загрузка кэшированных шейдеров |
| re-spirv сжатие (уже сделано) | Да | Низкая | Меньше дискового пространства |
| VK_EXT_shader_object | Да | Средняя | Меньше накладных расходов на pipeline |
| VK_EXT_graphics_pipeline_library | Да | Средняя | Быстрая пересборка pipeline |
| Предкомпиляция офлайн | Да | Низкая | Запуск без shader storm |
| Нативный бэкенд GPU | ❌ Нет | Очень высокая | Полный отказ от SPIR-V, но сложность поддержки |
| CPU бэкенд | ❌ Нет | Высокая | Работа без GPU шейдеров, но медленно |

**Рекомендация:** Продолжать использовать SPIR-V как обязательный формат, но инвестировать в AOT кэш и VK_EXT_shader_object для снижения накладных расходов. Полный отказ от SPIR-V невозможен в рамках Vulkan API.
