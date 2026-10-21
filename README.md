# About

Tool for automatic executables porting to Linux and Windows.

Includes a [relinker](core/relinker) that converts executable to the target system's native format and implementations of [system prx libraries](core/libs/prx) suitable for dynamic linking. No emulation or separate runtime process.

[Usage](docs/user/USAGE.md), [Build instructions](docs/dev/BUILD.md), [Architecture](docs/dev/ARCHITECTURE.md), [Technical debt of the project](docs/dev/TechnicalDebt.md), [code style conventions](docs/dev/CONVENTIONS.md)

## Status

[![libraries](https://boykopovar.github.io/AnyPS5/badge-libraries.svg)](https://boykopovar.github.io/AnyPS5/) [![shaders](https://boykopovar.github.io/AnyPS5/badge-shaders.svg)](https://boykopovar.github.io/AnyPS5/)

[![progress map](https://boykopovar.github.io/AnyPS5/progress.svg)](https://boykopovar.github.io/AnyPS5/)

<sub>* System libraries: percentage of the functions known to the project so far (declared in [core/libs/prx](core/libs/prx)), not of every PS5 system function. The total grows as more functions are declared.</sub>

[List of verified games](docs/user/COMPATIBILITY.md)

Dreaming Sarah (2D platformer) runs at a stable 60 fps on a GTX 1050 Ti / i5-7500 3.4GHz.

Unsupported or unexpected states strictly throw `std::runtime_error`. `what()` is printed to stderr and the process terminates.

The [shader recompiler](core/shader/recompiler/Recompiler.cpp) successfully produces SPIR-V (validated via [Spirv-Tools](3rdparty/SPIRV-Tools) when built with `ANYPS5_ENABLE_SPIRV_TOOLS`).

## Diagnostics and launch

```sh
python tools/preflight.py --target linux --game-dir "/path/to/game"   # environment check first
python tools/inspect_executable.py eboot.bin --game-dir "/path/to/game" # content-based format check
python tools/plan.py --target linux --game-dir "/path/to/game"          # execution plan
./tools/run_game.sh --game-dir "/path/to/game" --target linux           # prepare (add --run to launch)
```

Windows: `.\tools\run_game.ps1 --game-dir "D:\Games\MyGame" --target windows`.
Details: [PROMT workflow](PROMT.md), [diagnostics architecture](docs/dev/DIAGNOSTICS.md).

## Compatibility

See the [game compatibility list](docs/user/COMPATIBILITY.md) for tested games and known issues.

## Differences from upstream

This fork is based on upstream [AnyPS5](https://github.com/boykopovar/AnyPS5) plus its own work:

- Prepare-and-launch toolchain: `tools/preflight.py`, `tools/inspect_executable.py`, `tools/plan.py`, `tools/run_game.py` (plus `.ps1`/`.sh` wrappers), `tools/capabilities.py`, `tools/vulkan_check.py`, and the `self_inspector` SELF converter — staged workflow `PREFLIGHT → RELINK → RUNTIME_START → GAME_RUNNING` (see `PROMT.md`, `docs/dev/DIAGNOSTICS.md`).
- Windows game launcher (`launcher/`, ImGui + Win32 + DirectX 11): game library, covers, themes, system-wide F1 menu.
- CPU packet-execution backend for the graphics driver (`CpuBackend/`, `ANYPS5_CPU_BACKEND=legacy|fast|auto`), CPU fallback for depth-bounds tests (`APS5_DEPTH_BOUNDS=hardware|cpu|auto`), capability-driven unified memory (`GuestUnifiedMemory`).
- Storage-texture write-back owner guard and TLSF-reuse guard in `Graphics/src/Texture.cpp` (kill switch `APS5_NO_REFRESH_FAST_PATH=1`).
- Low-VRAM FullHD cap for sampled texture snapshots (`Graphics/include/LowVramDownscale.hpp`, auto-enabled on device-local heaps ≤ 9 GiB, `ANYPS5_DOWNSCALE*` overrides; see [USAGE.md](docs/user/USAGE.md)).
- Gamepad-only input (keyboard/mouse pad bindings removed) and a local tested-games table (`docs/user/COMPATIBILITY.md`).
- Shader recompiler: Runtime AOT shader cache (`core/shader/recompiler/AotCache.{hpp,cpp}`, compressed with `re-spirv`, kill switch `APS5_NO_AOT_CACHE=1`), indirect-call fast fallback for unresolvable `s_swappc_b64` (`APS5_ALLOW_INDIRECT_CALLS=1`), structurizer bounds check on large CFGs. GTA V (PPSA04263): `ComposePackedAncillary()` port (spec-id 8233/8234, serializer v14, disk cache v22) and `ResourceTracker::GetHandle` SRT chain validation fix.

## Input mapping

SDL-mapped game controllers are supported, including analog sticks and triggers. The gamepad (Xbox/PS layout via SDL_GameController) is the only pad input; keyboard/mouse pad bindings were removed. The window starts in fullscreen and the title shows only FPS. See [input mapping](docs/user/INPUT_MAPPING.md) for the controller layout.

## Disclaimer

This project is intended for interoperability, research, preservation, and compatibility purposes. It does not include, distribute, or require copyrighted software, firmware, cryptographic keys, or proprietary libraries. Users are responsible for ensuring that any binaries used with this project are obtained and used in accordance with applicable laws and their respective license terms.

## License

This project is licensed under the GNU General Public License version 2 only.
