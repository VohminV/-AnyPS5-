# Diagnostics architecture

Self-diagnostics infrastructure for OpenCode and users: environment
detection, input analysis, tool capability registry, processing planner,
unified launch, and tests. All decisions come from observed properties of
the environment, file contents, and real tool capabilities — never from file
names, terminal names, or blind retries.

## Components

| Tool | Role | Key facts |
|------|------|-----------|
| `tools/preflight.py` | Mandatory environment preflight. Statuses `READY` / `READY_WITH_WARNINGS` / `BLOCKED_ENVIRONMENT` / `BLOCKED_INPUT_FORMAT` / `BLOCKED_DEPENDENCY` / `BLOCKED_BUILD`, each blocker with reason + evidence + next action. | Host OS from `platform.system()`; target only from `--target` (default `linux`). Relinker CLI probed from the real binary usage, fall back to `core/relinker/cli/src/CliArgs.cpp`. This Relinker has **no `--help`**; `--windows` is decided once from the target. Reports `build_toolchain` and `runtime_environment` separately. |
| `tools/inspect_executable.py` | Read-only input analyzer. Formats `ELF` / `SELF` / `PKG` / `UNKNOWN` / `TRUNCATED` / `TOO_SMALL` / `MISSING` / `UNREADABLE` by content. For ELF: header fields, program/section header tables with bounds checks, `PT_DYNAMIC` requirement, per-format `supported_ops` and refusal reasons. For SELF: points at `self_inspector`, states the no-crypto limit. | Mirrors `ElfReader.cpp` magic checks; never modifies originals. Game module layout follows `GuestModuleBuilder.cpp` (`sce_module` XOR `sce_modules`, `prx` may coexist). |
| `tools/capabilities.py` | Single tool capability registry: location, version, availability, input/output formats, CLI flags, dependencies, limits, result verification. | Relinker version = source commit (binary has no `--version`). `self_inspector` = plaintext remap only. |
| `tools/vulkan_check.py` | Layered Vulkan diagnostics: `loader`, `devices` (via `vulkaninfo --summary`), `features`, plus `logical` / `shaders` / `pipeline` / `present` as explicit `not_attempted` (they need real GPU work). | Never claims success for unattempted layers. Missing loader/device blocks `RUNTIME_START`, not relink. |
| `tools/plan.py` | Processing planner: builds stages `PREFLIGHT` / `RELINK` (+`CONVERT_SELF`) / `RUNTIME_START` / `GAME_RUNNING` from task + input state + registry + target, each with goal, inputs, preconditions, tool, command, artifacts, success criteria, timeout, verification, on-error actions. | No stage starts with unmet preconditions; exit 0 alone never passes a stage. |
| `tools/run_game.py` (+`.ps1`/`.sh`) | Executes the plan with timeouts, repeat guard (`logs/.last_failed_command.json`), artifact verification, and per-run logs (`logs/run_*.log`: command, exit code, commit, env, stdout/stderr). | Conversion success is never reported as game launch. Paths with spaces handled via argv arrays. SELF trees are mirrored into `<game-dir>/_anyps5_extracted/` (main plus modules); the game output stays beside the original game dir. |

## Data flow

```
preflight ──┬──▶ report(STATUS, relink_argv, next_action)
            │
inspect ────┤──▶ file/module verdicts (content-based)
            │
capabilities┤──▶ registry (what each tool can really do)
            │
vulkan ─────┘
              ▼
        plan.build_plan() ──▶ stages + first_block
              ▼
        run_game executes stages in order, verifies artifacts, logs
```

## Testing

`tools/tests/test_{inspect_executable,preflight,capabilities,plan,vulkan_check}.py`
run under `unittest` and `ctest` (root `CMakeLists.txt`). Synthetic headers
and temp dirs only — no game dumps, keys, or sensitive data.
