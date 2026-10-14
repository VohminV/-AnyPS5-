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
- Fixing TLSF allocator crash (UUM-83271) during scene transitions in PS5 emulator
- Crash: Unity's TLSF `app.exe+0x1a579ec` reads `0xffffffffffffffff` on `UnityGfxDeviceWorker` (free-list corrupted)
- Analysis: OS-level owner guard cannot see in-game TLSF free/realloc inside the same mapped arena; gen==0 (untracked) write-watch stamps stay silent on free-list pop (reads before writes), so stale GPU results were stored over the new owner's live bytes
- Fix: `streamingTextureMutex` is now `recursive_mutex`, held ONLY for the gen==0 fast-path check (never across FlushPending/GPU waits), skipped when GpuMutex is held (avoids streaming->Gpu vs Gpu->streaming inversion); plus TLSF-reuse guard in `writeBackLayers`: for gen==0 compare guest vs `original` snapshot before storing, drop on mismatch
- Kill switch: `APS5_NO_REFRESH_FAST_PATH=1` disables fast path AND streaming mutex (reuse guard also gated by `unregisteredDrop`)
- Ship patched libs: build `libs` target (NID patcher), copy from `build/core/libs/libs/*.prx` (NOT `unpatched/`) into game `libs/`

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
python -m unittest tools.tests.test_inspect_executable tools.tests.test_preflight tools.tests.test_capabilities tools.tests.test_plan tools.tests.test_vulkan_check
```
