# Relinker graphics blockers — Ghost of Yōtei (NVIDIA RTX 3050 8GB, Windows)

Дата: 2026-10-12. Игра не объявлена рабочей: повторный запуск с этими патчами
ещё не выполнялся. Каждый пункт ниже: факт кода → спецификация → патч → тест.

Карта компонентов (все пути — текущий форк, без переписывания):

- `core/shader/recompiler/RdnaDecoder/` — декодер ISA → `RdnaProgram`
- `core/shader/recompiler/ControlFlow/` — `GraphBuilder` (swappc/call-анализ), `Structurizer`
- `core/shader/recompiler/Translation/` — IR
- `core/shader/recompiler/Optimization/` — `SrtWalker`, `ResourceTracker`,
  `ResourceMaterializer`, `BindingAllocator`, `DescriptorBindingBuilder`
- `core/shader/recompiler/SpirvBackend/` — SPIR-V emit
- `core/shader/recompiler/Recompiler.cpp` — `PrepareShader` / `compileVariant` /
  `materializeResult`, `RuntimeAbi.hpp` — ABI-константы (`Version 11`)
- `core/libs/prx/libSceAgcDriver/` — `ShaderRegistry` (register/deferred/on-demand),
  `ShaderResources`/`ShaderValidation` (layout, device limits), `VulkanDevice`
  (features), `ShaderDiagnostics` (дампы), `ShaderDeviceProfile` (проверка caps)

Лог-признаки (run.log 12.10, 1570 строк): deferred 121 = swappc 104 / VOP2 9 /
GetImage 6 / heap 2; slow 10 (до 118 с); кэш холодный (0 хитов, ~1 ГБ записей).

## P0. Шейдер 55144 слова (0x8000083e00) — 10+ мин без лога

Путь register-подготовки (`ShaderRegistry.cpp: PrepareRegistered` → `PrepareShader`
→ `findOrCompileVariant` → `PrepareResourceProgram` + `compileVariant`):
decode → Build CFG → Structurize → Translate → SSA → fold/DCE → readLane →
maskedSelect → SrtPlan → Track → ApplyStaticInterface → Collect → Allocate →
SPIR-V Emit. Создание Vulkan-пайплайна здесь не участвует (оно позже, на draw).

Причина тишины: стадии не логируются вообще; долгий прогон неотличим от
deadlock. Подозреваемые сверхлинейности (не доказаны — нет бинарника):
`Structurizer`, bindless material-сканы (`MaterialScanLimit`), `EquivalentValue`
сравнения в ` intern`ах, SPIR-V emit 55k инструкций. Дополнительно: подготовка
держит `SourceEntry::mutex`, параллельные потоки ждут (см. upstream-заметку
к #1789 про 11 потоков в `ShaderPreparationTransaction`).

Патч (только диагностика, семантика не менялась):

- `Recompiler.cpp`: `PrepareStageClock` — heartbeat `[shader-stages] 0xADDR
  <stage> start` при `APS5_TRACE_SHADER_STAGES=1` (stall виден как последняя
  строка) + всегда-при-`total>10с` сводка `words/instr/blocks/edges/irOps`
  (front-end) и `spirvWords/buffers/images/samplers` (back-end) с разбивкой
  по стадиям. Фонового потока-сторожа сознательно нет: рекомпилятор — чистая
  функция входа на чужих потоках, авто-килл запрещён.
- Файлы: `core/shader/recompiler/Recompiler.cpp`.

Тесты: сборка `shader_recompiler_agc_driver` + вся usually-затрагиваемая
регрессия зелёная (ниже). Время 55k-шейдера до/после: до — 10+ мин без единой
строки; после — не измерено (нужен запуск с `APS5_TRACE_SHADER_STAGES=1`).

## P1. VOP2 `encoding 0x0` (9 случаев, word 0, PC ~85–90% больших шейдеров)

Факт кода: `DecodeRdnaVop2` (`RdnaVectorOpDecoder.cpp`) бросает через
`lookupVectorOpcode(vop2Opcodes, 0, ...)`; нулевое слово попадает строго в
семейство VOP2 (`bit31=0`), opcode 0.

Спецификация: `gpu_research/amd-rdna2-isa.pdf`, §12.7 — таблица VOP2
начинается с opcode **1** (`V_CNDMASK_B32`); opcode 0 не существует. Таблица
проекта совпадает с ISA — «недостающей инструкции» нет, добавлять
фиктивный opcode запрещено и не нужно.

Патч (по прямому указанию владельца, бинарное подтверждение отложено):

- `RdnaInstructionDecoder.cpp` (`DecodeRdnaProgram`): пропуск interior
  zero-word по одному слову, PC после — выровнены; счётчик
  `RdnaProgram::skippedZeroWords` (`RdnaProgram.hpp`); трейс
  `APS5_TRACE_ZERO_WORDS=1`. Безопасность: нулевое слово сегодня бросает
  всегда → ни одна успешно декодирующаяся программа нулей в диапазоне
  декода не содержит → их поведение не меняется. Ветка в паддинг по-прежнему
  упадёт невалидной целью. Прямой декодер (`DecodeRdnaVop2`) и front-программы
  строгие как раньше.
- Диагностика (была раньше, сохранена): `VOP2 opcode is not implemented
  (encoding 0x..) at program counter .. (word .., vdst/src)` — в лог уже
  пишет адрес/код/PC/слово.
- Файлы: `RdnaDecoder/src/RdnaInstructionDecoder.cpp`,
  `RdnaDecoder/include/RdnaDecoder/RdnaProgram.hpp`,
  `libSceAgcDriver/tests/RdnaTruncatedModifier.cpp` (`CheckZeroWords`:
  interior-нули, PC, счётчик; trailing-нули как раньше; all-zero — ошибка).

Тесты: `agc_rdna_truncated_modifier` + `scalar_swappc`, `vector_fused_multiply_add`
зелёные. Нужно от следующего запуска: `APS5_DUMP_FAILED_SHADERS=1` для любого
из 9 шейдеров (`shader_fail_*.bin`) — отличить паддинг от десинхрона/таблицы.

## P2. Storage heap group 30 (needs 5 и 8 при capacity 4)

Факт кода: `BindingAllocator::Allocate` — каждый DynamicStorage-образ
резервирует `StorageHeapCapacity` (4) копий в группе; два таких образа в
группе 30 (binding 31, 2D float storage) дают 8 > 4; один + один обычный — 5.
`DescriptorBindingForImage` → группа; `HeapCapacity(binding)` — потолок;
`ShaderValidation.cpp:473` и `ShaderResources.cpp:2527,2538` сверяют
скомпонованный count с capacity и с `maxPerStageDescriptor*` устройства.

Upstream (источник решения, не замена архитектуры): issue #1789 — тот же
симптом вплоть до `imageGroups[30]` binding 31 needs 5; закрыт мерджами
#2141 (sampler 16→32, `Version` 10→11, asserts 13760→14528 / 3432→3624),
#2282 (split `StorageMipSlots=4` / `StorageHeapCapacity=16`), #2347
(sampled 16→64, split `BindlessTableSlots=16`). Проверено на Astro Bot /
Unreal / Sonic Origins, ctest 497/497.

Патч (точный порт трёх PR под наш форк, файлы совпали с pre-image):

- `RuntimeAbi.hpp`: `Version 11`, sampled 64, `+BindlessTableSlots 16`,
  `+StorageMipSlots 4`, storage 16, sampler 32, asserts 14528/3624.
- `BindingAllocator.cpp:117`, `DescriptorBindingBuilder.cpp:312`,
  `ResourceMaterializer.cpp:609,716,760` — mip-резерв через `StorageMipSlots`;
  `BindlessSlots()` — через `BindlessTableSlots`.
- `tests/ShaderDeviceProfile.cpp` — кейсы из трёх PR (40 sampled, 2 dynamic
  storage, full heap 16, 16 sampler→32, `BindlessSlots()==16`).
- Совместимость: `Version 11` инвалидирует старые артефакты везде
  (`RequireVersion` в `ShaderResources:957`, `VulkanDevice:2670,3182,3475`;
  дисковый кэш отбраковывает по версии + `SourceVersion` меняется от нового
  хэша исходников → старый кэш игнорируется, GC — 14 дней).

Тесты: `agc_shader_device_profile` (обновлён), `agc_shader_memory`,
`agc_shader_bindless`, `agc_shader_disk_cache`, `agc_driver_graphics` —
зелёные. Наши кейсы (needs 5/8) теперь проходят потолок 16.

Vulkan capabilities (проверены, не из заголовков): `nullDescriptor`
(`VK_EXT_robustness2`, `VulkanDevice.cpp:1170-1175`, требуется), non-uniform
sampled+storage indexing (1046-1051, требуется), per-stage лимиты сверяются
в `addImageBinding` (2538+). Значения на RTX 3050 — снять в следующем запуске
(`ShaderDeviceProfile`).

## P3. GetImageResource / GetAddressResource (6 случаев, 2 семейства)

Путь (верифицирован по исходникам, без изменений): user data →
`GetUserData` → арифметика адреса → `GetAddressResource`/`GetBufferResource`/
`GetImageResource` (`Translation/MemoryInstructions`) → `ResourceTracker`
валидирует runtime-значения (скалярные загрузки — только через bindless-план
`TryMakeTableImage`) → `ResourceMaterializer` (режимы, `Prepare`) →
`BindingAllocator` (группы) → SPIR-V (`SpirvImageEmitter`, `ResourceIndex`) →
`DescriptorBindingBuilder::Prepare/Populate` (per-snapshot compact +
специализационные константы `DescriptorIndex`) → `addImageBinding` (layout,
лимиты устройства) → `ShaderValidation` (длина массива == binding count).

Семейства в логе: (a) большие шейдеры, дескриптор из scalar-таблицы без
bindless-паттерна; (b) мелкие (136–156 слов), `LoadAddressU32@pc=0x78` через
`GetAddressResource(GetUserData)`. Данных для фикса нет (нужны
`APS5_TRACE_BINDLESS=1` + бинарник) — по правилу «нет данных → только
диагностика» код не менялся. PR #1184 скачать не удалось (сеть к GitHub
нестабильна: `.diff`/`/files`/API — таймауты); issue #1789 выкачан полностью
и использован для P2.

## P4. s_swappc_b64 + deferred (104 старых случая, новых за 15 мин — 0)

Проверено без изменений рабочего кода:

- Декодер (`RdnaScalarOpDecoder.cpp:343`): `s_swappc s[14],s[14]` —
  нормальный swap-call (link==target), семантика сохранена.
- Наш предикат-фикс (self-исключение из `userDataPair`, прошлый шаг) на месте,
  покрыт `scalar_swappc` (uniform/written origin). Upstream #1836 (дифф
  получен): наш форк покрывает тот же сценарий своим catch-and-retry —
  регистрация ловит исключение (`ShaderRegistry.cpp:566` «deferred») →
  `SourceHandleFor`/`InvocationFor` (180-275) повторяют on-demand
  (`dispatch/invoke-ondemand`, `unregistered`) → неудача relevant draw
  реджектит draw, не убивая процесс. Порт #1836 (флаг `deferred`,
  пре-скан `UnresolvableSwappcTarget`) не выполнялся: архитектура форка
  сохранена, исход эквивалентен. Отсутствие новых swappc-событий причиной
  не считается.

## Интеграция и симптомы

Связь decoder→IR→SPIR-V→pipeline→heaps→cache→sync не менялась, кроме точек
выше. Мерцание кат-сцен / переходы / FPS~1 / долгая компиляция — без
доказательств не сводятся к одной причине (long-compile частично покрыт P0).

## Изменённые файлы (этот шаг)

- `core/shader/recompiler/Recompiler.cpp` — P0 `PrepareStageClock`
- `core/shader/recompiler/RuntimeAbi.hpp` — P2 константы/Version/asserts
- `.../Optimization/src/{BindingAllocator,DescriptorBindingBuilder,ResourceMaterializer}.cpp` — P2
- `libSceAgcDriver/tests/ShaderDeviceProfile.cpp` — P2 кейсы
- (ранее, сохранено): zero-skip + VOP2/swappc/heap/sysmodule диагностика,
  дампы в кэш-папку, тесты swappc/fused/sysmodule/truncated

## Тесты (после всех изменений)

11/11: write_tracking, dcc_key_sharing, display_buffer_dcc,
draw_snapshot_reuse, bindless, memory, disk_cache, device_profile,
control_flow, graphics, buffer_address; плюс ранее: swappc×3, fused,
sysmodule, truncated, scalar_call. Сборка `libs` + NID OK;
`libSceAgcDriver.prx` (`7562D267…`) скопирован в игру.

## Осталось / недостающие данные

1. Запуск игры (остановлена, `run.log` пропал): `ANYPS5_SHADER_CACHE_DIR=…\shader`,
   `APS5_DUMP_FAILED_SHADERS=1`, `APS5_TRACE_SHADER_STAGES=1`,
   `APS5_TRACE_ZERO_WORDS=1`, `APS5_TRACE_BINDLESS=1`.
2. Время 55k-шейдера до/после; стадия-тормоз из `[shader-stages]`.
3. `shader_fail_*.bin` для VOP2-нулей (паддинг vs десинхрон).
4. GetImage-families: bindless-trace + бинарники.
5. Значения `maxPerStageDescriptor*` на RTX 3050 из `ShaderDeviceProfile`.
6. #1184 — повторить при стабильной сети; #1836-порт — только при расхождении
   поведения (пока эквивалентно).
