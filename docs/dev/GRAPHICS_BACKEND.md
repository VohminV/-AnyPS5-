# Graphics backend decision: Vulkan stays for the game, Direct3D 11 for the launcher

Date: 2026-10-09. Status: decided, Vulkan game backend is kept.
A full Direct3D 12 migration of the game renderer was audited and rejected
(see blockers below). This document records what was checked so that the
question does not need re-investigation.

## Where the game actually executes and renders

* The game runs natively on the PC. `core/relinker/main.cpp` converts the PS5
  ELF into a native PE; OS loader binds NID imports to native `libs/*.prx`
  (`docs/dev/ARCHITECTURE.md`). There is no PS5 in the loop, no streamed
  image, no remote decode.
* Vulkan IS the game renderer, not a compositor: PS5 GNM command buffers are
  translated and RDNA shaders are recompiled to SPIR-V
  (`core/shader/recompiler/Recompiler.cpp`, single backend: `SpirvBackend/`),
  then executed on the PC GPU. Presentation is a Vulkan swapchain
  (`FIFO`, vsync) into the SDL window:
  `core/libs/prx/libSceAgcDriver/Execution/src/VulkanDevice.cpp:1196,1854`.

## Vulkan inventory (game path)

* 35 non-test sources use Vulkan directly, all inside `libSceAgcDriver`
  (`Execution/` + `Graphics/`). `VulkanDevice.cpp` alone calls 63 distinct
  `vk*` functions (instance, device, swapchain, pipelines, memory, queues,
  fences, queries, presentation).
* Advanced features the engine requires: buffer device address + shaderInt64,
  descriptor indexing (bindless), 8-bit storage, mesh shaders, fragment
  interlock, barycentrics, shader clock, conservative rasterization,
  int64 atomics, depth-clip control, maintenance8, draw-indirect-count,
  external-memory host import, min/max samplers, image robustness.
* Build embeds GLSL shaders compiled to SPIR-V at build time
  (`core/libs/prx/libSceAgcDriver/CMakeLists.txt`: glslang `-V
  --target-env vulkan1.1`).
* The runtime↔graphics interface itself is Vulkan-typed
  (`PresentationWindow` carries `VkSurfaceKHR`/`VkInstance`;
  `AgcDriver::Context` carries `VkDevice` + resolved `vk*` pointers;
  `VkImage`/`VkBuffer` flow through textures, buffers, depth surfaces).

## Vulkan → Direct3D 12 mapping (audited, not implemented)

| Vulkan mechanism | D3D12 counterpart | Verdict |
|---|---|---|
| SPIR-V modules (`vkCreateShaderModule`), single `SpirvBackend` emitter | HLSL emitter from project IR + DXIL via dxc | BLOCKED: no HLSL emitter exists; writing one duplicates the largest recompiler component |
| SPIRV-Tools validation/optimization | DXC validation/optimization | BLOCKED: no `dxcapi.h` in MinGW, no `dxcompiler.dll`/`dxil.dll` on this machine; MinGW cannot consume DXC without hand-written COM headers (breaks WinLibs-only constraint) |
| glslang build-time shaders (`vulkan1.1` target) | Re-author for SM 6.x + compile | Feasible but churn; blocked behind the shader-emitter decision |
| Descriptor indexing / bindless | SM 6.6 dynamic resources | Feasible, moderate |
| Buffer device address, int64/float64, 8-bit storage | GPU VA, Int64, 16-bit types | Feasible, moderate |
| Mesh shaders (`VK_EXT_mesh_shader`) | Amplification/Mesh shaders | Feasible, substantial (different programming model details) |
| Fragment interlock / ROV-equivalents, barycentrics, conservative raster, shader clock, min/max sampler, int64 image atomics | ROV, `SV_Barycentrics`, CR tiers, clock via workarounds, min/max samplers, Int64 atomics | Mixed; each needs per-feature porting + validation |
| External-memory host import, dma-buf | Custom heaps / shared handles | Substantial; host-import path is performance-critical |
| Swapchain `FIFO` present, `OUT_OF_DATE`/`SUBOPTIMAL` resize | DXGI flip-model swapchain, `ResizeBuffers` | Feasible, small (already proven by the D3D11 launcher) |
| `libSceVideoOut` presentation (`SDL_Vulkan_*` surface) | `SDL` + DXGI surface | Feasible, small |

Net: porting the game renderer = a second shader backend + a second
execution backend (~40 files) + new shader-cache versioning + validation on
RTX 3050. Months of work, and MinGW cannot compile shaders without DXC.

## Blockers (concrete)

1. No HLSL/DXIL emitter; the recompiler is SPIR-V-only by construction.
2. No DirectXShaderCompiler usable from this toolchain/machine
   (headers and runtime DLLs absent).
3. The runtime↔graphics interface is Vulkan-typed end to end; a D3D12
   backend cannot reuse it, only replace it.
4. No measurement shows the API as the bottleneck: observed ~10 FPS occurs
   during shader/pipeline compilation (disk-cache writes, `vk-pipelines`
   driver cache re-saves) plus OBS + browser video load on the same RTX 3050;
   steady-state in-game FPS is still to be measured in a real playthrough.
   D3D12 would pay the same compilation cost with a cold cache.

## What is native today

* Launcher UI: Dear ImGui v1.91.8 (pinned, `3rdparty/imgui`) + Win32 +
  DirectX 11 (`d3d11.dll` inbox, MinGW headers/libs present). No Vulkan in
  the launcher process.
* Game: Vulkan (kept). The two APIs render different windows; this is
  separation, not duplication.

## Performance notes (measured, not projected)

* `nvidia-smi` idle: 9% GPU, 938/8192 MiB, 43°C (game closed).
* Run log: `[shader-disk-cache] 86 hits, 0 misses` per 10 s during load;
  `shader_cache/be44…` + NVIDIA `vk-pipelines-10de-2582…bin` actively
  written during first runs (compile stutter, then it settles).
* Boot/menu FPS ~5–10 observed while compiling + OBS capture active.
  No steady-state in-game measurement yet — pending a real playthrough
  (cannot be synthesized; not claimed).

## Crash fixes landed alongside (engine, API-independent)

* Stencil-plane sampling with reused address → snapshot fallback instead of
  `runtime_error` terminate (`Graphics/src/DepthSurface.cpp`).
* `AvPlayer::Stop()` use-after-free of Unity-held video buffers → retire at
  Stop, free at next Start/Close with bounded quiescence
  (`libSceAvPlayer/src/Source.cpp`).
* `crash.log` beside the exe (stderr is lost on double-click)
  (`libc/.../windows/CrashReport.cpp`).
* Enriched `readRegister` message with the register offset.
