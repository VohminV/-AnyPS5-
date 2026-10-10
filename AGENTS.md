# AGENTS.md

## Instructions for AI Agents

### Do Not Use Git
- Do not run git commands (commit, push, diff, status, log, etc.)
- Do not create or modify git configuration
- Do not stage or commit changes
- The user will handle all git operations manually

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
# Build libSceAgcDriver
cmake --build build --target libSceAgcDriver --config Release -j 8

# Run tests
ctest --test-dir build -R "agc_write_tracking|agc_driver_graphics|agc_dcc_key_sharing|agc_display_buffer_dcc|agc_draw_snapshot_reuse" --output-on-failure
```