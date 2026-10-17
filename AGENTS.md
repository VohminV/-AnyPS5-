# AGENTS.md

## Instructions for AI Agents

### Do Not Use Git
- Do not run git commands (commit, push, diff, status, log, etc.)
- Do not create or modify git configuration
- Do not stage or commit changes
- The user will handle all git operations manually

### Mandatory Agent Protocol
Before any build, file processing, or game launch, do all of these in order:
1. Study the existing implementation (no parallel duplicate of working code).
2. Run environment diagnostics: `python tools/preflight.py --target <windows|linux> --game-dir <dir>`.
   Never guess host OS, target OS, binary paths, CLI flags, or dependencies.
3. Inspect the inputs by content: `python tools/inspect_executable.py <file> [--game-dir <dir>]`.
4. Establish real tool capabilities: `python tools/capabilities.py` (registry:
   location, version, formats, CLI flags, limits, verification).
5. Build the execution plan: `python tools/plan.py --target <...> --game-dir <...> [--run]`.
   No stage starts unless its preconditions hold; exit 0 alone never passes a
   stage — artifacts are verified (existence, format, size, validity).
6. Check preconditions, execute via `tools/run_game.py` (or `.ps1`/`.sh`
   wrappers), verify the result, keep the diagnostics (`logs/run_*.log`).

- Host OS (`platform.system()`) and Relinker target OS are different things.
  The target comes only from `--target` (default: `linux` = Linux ELF output).
  `build_toolchain` (what builds here) and `runtime_environment` (what runs
  the result: target + Vulkan + built libs) are reported separately.
- The Relinker CLI in this checkout has **no `--help` flag**; bare invocation
  prints usage. `--windows` is decided once from the target in
  `tools/preflight.py::build_relink_argv`, never by retrying with/without it.
- A `.exe` output name alone does not select Windows (the Relinker warns and
  still emits Linux ELF). Never infer the target from the output extension.
- Detect SELF **before** Relinker via `tools/inspect_executable.py`. SELF is
  never passed to Relinker. The only supported conversion path is the
  project's `self_inspector` (`--report`/`--extract`, plaintext remap, no
  crypto) into a **separate** directory; the original file is preserved.
  Never invent third-party converter CLIs and never download tools ad hoc.
- Game layout (dump root vs input dir vs output): the user names the dump
  root (e.g. `E:\PPSA21733`, which contains only `app0/`). `--game-dir`
  must point at the input dir holding `eboot.bin` plus `sce_module/`
  (e.g. `E:\PPSA21733\app0`), never at the dump root. The Relinker
  `--output` goes **beside** the input dir (e.g. `E:\PPSA21733\app.exe`
  for windows, `E:\PPSA21733\app.elf` for linux), because the runtime
  layout is `app.exe + libs/ + app0/` per `docs/user/USAGE.md`.
  If `--game-dir` has no `eboot.bin`/`input.elf`/`app.elf` but has an
  `app0/` subdir containing them, re-run with `--game-dir <dir>\app0`
  and `--output <dir>\app.exe` (windows) — do not place the output
  inside `app0/`. Evidence: `GuestModuleBuilder.cpp` writes guest
  modules to `<output-dir>/app0/sce_module/*.guest.prx`, `main.cpp`
  prints the `app.exe + libs/ + app0/` layout.
- Before editing sources, establish root cause plus a reproducible test.
- Keep an attempt log: inputs, command, environment, tool version, exit code,
  validation result (`logs/run_*.log`, failures in
  `logs/.last_failed_command.json`). Before re-running a command, diff it
  against the previous one. A retry is allowed only with: changed inputs, a
  fixed confirmed cause, justified parameter changes, new diagnostic info, or
  a fixed environment issue. Never repeat an identical failed command.
- After two substantive attempts with different justified fixes and no
  progress, stop iterating: revisit the hypothesis and return to root-cause
  diagnostics. (A fixed cause resets the count — this is not a task limit.)
- Never rename files or tamper with magic bytes to bypass format checks.
- A passed conversion is not a launched game. Stages are separate:
  `PREFLIGHT` / `RELINK` / `RUNTIME_START` / `GAME_RUNNING` (see
  `tools/run_game.py`).
- Never modify rendering, shader translation, or ABI code to satisfy a
  diagnostic check.
- Never delete the user's uncommitted changes.
- Never create duplicate instruction/tool files; extend the existing ones.
- If docs conflict with behavior, the actual CLI, sources, and tests win;
  bring the docs in line.

### New Functionality Development
When implementing new features or fixes:
1. First understand the existing codebase patterns and conventions
2. Propose the approach before implementing
3. Write clean, minimal code following existing style
4. Add tests where appropriate
5. Run lint/typecheck/build verification if commands are available
6. Document any new environment variables or configuration options

### Current Task Context
- GTA V (PPSA04263) port: black screen after splash, shader storm at intro video.
- **Этап 1 (ComposePackedAncillary) завершен**: `rectangle ABI` краш устранен, 0 «unsupported live use», 0 «rectangle ABI» в игре. Порт `ComposePackedAncillary()` из `boykopovar/AnyPS5` (discussion #1203) с восстановленным `Fail` в `ShaderInfoCollector`, spec-id 8233/8234, сериализатор v14, диск-кэш v22, тест `PixelAncillary.cpp` зелёный.
- **Этап 2 (ResourceTracker::GetHandle)**: удален ранний `ReadConstBuffer`-fail, валидация уходит в `ValidateSource`/`SrtWalker` (поддерживает `GetSrtResource → ReadConst → GetBufferResource → ReadConstBuffer` цепочки RAGE). Тест `ResourceTracker` пройден.
- **AOT Shader Cache** (`core/shader/recompiler/AotCache.{hpp,cpp}`): ключ = hash(raw RDNA2 бинарник + static pipeline layout), файлы `aot_shader_cache/<hash>.avc`, сжатие через `re-spirv -O --eliminate-dead-code-aggressive` (без spirv-opt фолбэка), kill switch `APS5_NO_AOT_CACHE=1`. При hit SPIR-V эмиссия пропускается.
- **Indirect-call fast fallback**: `APS5_ALLOW_INDIRECT_CALLS=1` позволяет шейдерам с неразрешимыми `s_swappc_b64` компилироваться вместо `deferred` (`GraphBuilder.cpp`, `InstructionTranslator.cpp`).
- **Structurizer guard**: bounds check на `successor` block id в `rebuildPredecessors` (источник SIGSEGV на больших графах, 819+ блоков).
- Окно игры закрывается пользователем (SDL_QUIT: MOUSEMOTION→LEAVE→CLOSE→QUIT), не самой игрой. Игра штормит ~2600 шейдеров после интро видео; до сцены не доходит без длинного тайм-аута и без закрытия окна.
- Запуск: `app.elf` déjà exécutable, лежит рядом с `libs/` и `app0/` (раунтайм-レイаут `app.elf + libs/ + app0/` per USAGE.md). Прямой запуск: `cd PPSA04263 && ./app.elf`. Через run_game.py: `--game-dir app0 --output app.elf --run --run-timeout 3600` (не закрывать окно после интро).
- Сборка: `ninja -C build core/libs/libs/libSceAgcDriver.prx`, копировать в `PPSA04263/libs/`, verificar file count (117).
- ctest: ровно 13 предсуществующих падений (список в рабочих заметках), новых нет.
- Рабочий каталог: `/home/vita/Документы/-AnyPS5-/`; игра: `/home/vita/Документы/PPSA04263/`.
- Только `python3`; git-команды запрещены AGENTS.md (пользователь делает commit/push вручную, здесь — по прямой просьбе пользователя).
- Upstream tarball: `/tmp/opencode/upstream/AnyPS5-main/` (эталон порта).
- Логи: `/home/vita/Документы/-AnyPS5-/logs/run_<stamp>.log`.

### Build Commands
```bash
# Relinker only (no submodules needed)
cmake -S . -B build-relinker -G Ninja -DCMAKE_BUILD_TYPE=Release -DANYPS5_RELINKER_ONLY=ON -DBUILD_TESTING=ON
cmake --build build-relinker --parallel
ctest --test-dir build-relinker --output-on-failure

# Build libSceAgcDriver
cmake --build build --target libSceAgcDriver --config Release -j 8

# Run tests
ctest --test-dir build -R "agc_write_tracking|agc_driver_graphics|agc_dcc_key_sharing|agc_display_buffer_dcc|agc_draw_snapshot_reuse" --output-on-failure

# Python tool tests (no build needed)
python -m unittest tools.tests.test_inspect_executable tools.tests.test_preflight tools.tests.test_capabilities tools.tests.test_plan tools.tests.test_vulkan_check tools.tests.test_run_game
```
