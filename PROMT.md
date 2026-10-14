# PROMT: подготовить и запустить игру в AnyPS5

Практический workflow. Не вспоминай последовательность команд — вызывай
скрипты ниже в порядке протокола из `AGENTS.md`. Фактический CLI, исходники
и тесты определяют поведение; данный файл следует им.

## 0. Однократная подготовка Relinker

```sh
cmake -S . -B build-relinker -G Ninja -DCMAKE_BUILD_TYPE=Release -DANYPS5_RELINKER_ONLY=ON -DBUILD_TESTING=ON
cmake --build build-relinker --parallel
ctest --test-dir build-relinker --output-on-failure
```

Без сабмодулей. Полная сборка (включая `libs`) — см. `docs/dev/BUILD.md`.

## 1. Диагностика окружения — всегда первой

```sh
python tools/preflight.py --target linux --game-dir "/path/to/game"
python tools/preflight.py --target windows --game-dir "D:\Games\MyGame"
```

- `--target` — единственный источник target OS. По умолчанию `linux`
  (выход — Linux ELF; суффикс `.exe` сам по себе Windows не выбирает).
- В отчёте разделены `host_os`, `target`, `build_toolchain` (что собирает
  здесь) и `runtime_environment` (что запускает результат).
- Проверь строку `STATUS:`: `READY` / `READY_WITH_WARNINGS` — можно идти
  дальше; любой `BLOCKED_*` — сначала устрани причину из полей
  `reason` → `evidence` → `next`.
- Слой Vulkan печатается послойно (`loader`, `devices`, `features`, …);
  `not_attempted` означает «не проверено без GPU-задачи», а не успех.
  Детали: `python tools/vulkan_check.py`.

## 2. Проверка входных данных по содержимому (read-only)

```sh
python tools/inspect_executable.py eboot.bin --game-dir "/path/to/game"
python tools/inspect_executable.py input.elf --json
```

Вердикт — только по содержимому (magic, заголовок ELF64, таблицы
программ/секций с проверкой границ, наличие `PT_DYNAMIC`), не по имени.
`SELF` блокируется до Relinker (`BLOCKED_INPUT_FORMAT`). Единственный
поддерживаемый путь — `self_inspector --report … --extract …` в отдельный
каталог (plaintext remap, без крипто); оригинал сохраняется.
Модули: `sce_module` XOR `sce_modules`, `prx` может соседствовать с одним
из них (`docs/user/USAGE.md`).

## 3. Возможности инструментов — из реестра, не из памяти

```sh
python tools/capabilities.py
```

Расположение, версия, форматы входа/выхода, флаги CLI (из реального
бинарника или `CliArgs.cpp`), зависимости, ограничения, способ проверки
результата. У Relinker в этом checkout **нет флага `--help`**; вызов без
аргументов печатает usage. `--windows` выбирается из target один раз
(`build_relink_argv`), а не перебором с/без флага.

## 4. План — до исполнения

```sh
python tools/plan.py --target linux --game-dir "/path/to/game"
python tools/plan.py --target windows --game-dir "D:\Games\MyGame" --run
```

Каждый этап содержит цель, входы, предусловия, инструмент, команду,
ожидаемые артефакты, критерии успеха, таймаут, проверку и действия при
ошибке. Этап без выполненных предусловий не запускается; exit 0 без
проверки артефакта этап не закрывает.

## 5. Подготовка и запуск — единый сценарий по плану

```powershell
.\tools\run_game.ps1 --game-dir "D:\Games\MyGame" --target windows
```

```sh
./tools/run_game.sh --game-dir "$HOME/games/MyGame" --target linux
./tools/run_game.sh --game-dir "$HOME/games/MyGame" --target linux --run
```

Стадии раздельны: `PREFLIGHT` → `RELINK` → `RUNTIME_START` → `GAME_RUNNING`.
Успешная конвертация ≠ запущенная игра. Без `--run` — только подготовка
с проверкой выходного формата (PE для windows, ELF для linux).
Повтор идентичной упавшей команды запрещён
(`logs/.last_failed_command.json`; после двух обоснованных попыток без
прогресса — пересмотр гипотезы, см. `AGENTS.md`). Каждый запуск пишет
`logs/run_<дата>.log` (команда, exit code, коммит, окружение, stdout/stderr).

Пути с пробелами поддерживаются (выполнение — массивом `subprocess`,
без сборки shell-строк).

## 6. Проверки

```sh
python -m unittest tools.tests.test_inspect_executable tools.tests.test_preflight tools.tests.test_capabilities tools.tests.test_plan tools.tests.test_vulkan_check tools.tests.test_run_game
python -m unittest tools.tests.test_import_audit tools.tests.test_progress
ctest --test-dir build-relinker --output-on-failure
```

Тесты — на синтетических заголовках во временных каталогах, без дампов игр.
Следующая команда после зелёного preflight печатается в строке `next:`.

Архитектура диагностики: `docs/dev/DIAGNOSTICS.md`.
