# Progress reporting

[`tools/progress.py`](../../tools/progress.py) generates `progress.json`, HTML, SVG maps and badges. The [Progress workflow](../../.github/workflows/progress.yml) publishes them after pushes to `main`. The [Progress report workflow](../../.github/workflows/progress-report.yml) compares the base and head trees of pull requests.

## Library counts

The scanner reads `APS5_VABI` function definitions under `core/libs/prx/<library>/`. A definition is pending when its body calls `NotImplemented_nid_no_patch`, directly or through a recognized local stub wrapper. Other definitions are counted as implemented. These counts describe declared functions, not every firmware export, verified ABI compatibility or playable games. Silent behavior and unverified assumptions remain in [Technical debt](TechnicalDebt.md).

A library with no scanned definitions can include a sibling library's source builder. The scanner recognizes literal `include(${CMAKE_CURRENT_SOURCE_DIR}/../<library>/<file>.cmake)` references, including quoted paths. It does not evaluate arbitrary CMake expressions.

For these wrappers, the JSON group adds `shared_sources` with the source library's directory name. Its own counts and name lists remain empty. The HTML table links to the source library instead of showing `0/0`; the source library's map tooltip lists its wrappers. Global totals count the definitions at the source library once.

## Comparisons

Comparisons identify functions by library and name. When a head group becomes a shared-source wrapper, its previous definitions are compared under the source library's identity. Removing a duplicate implementation is reported as sharing sources, without reporting the retained functions as removed.

Real removals and transitions back to stubs remain visible. Percentages retain the original snapshot totals, so eliminating duplicate accounting can change a percentage without changing available functionality. Older JSON snapshots without `shared_sources` are accepted.

## GTA V (PPSA04263) port status

- **Stage 1 (ComposePackedAncillary)**: completed. `rectangle ABI` crash fixed, 0 «unsupported live use», 0 «rectangle ABI» in game. Port of `ComposePackedAncillary()` from `boykopovar/AnyPS5` (discussion #1203) with restored `Fail` in `ShaderInfoCollector`, spec-id 8233/8234, serializer v14, disk cache v22, `PixelAncillary.cpp` test green.
- **Stage 2 (ResourceTracker::GetHandle)**: completed. Early `ReadConstBuffer` fail removed; validation moved to `ValidateSource`/`SrtWalker` supporting `GetSrtResource → ReadConst → GetBufferResource → ReadConstBuffer` RAGE chains.
- **AOT Shader Cache**: `core/shader/recompiler/AotCache.{hpp,cpp}` — key = hash(raw RDNA2 binary + static pipeline layout), files `aot_shader_cache/<hash>.avc`, compression via `re-spirv -O --eliminate-dead-code-aggressive`, kill switch `APS5_NO_AOT_CACHE=1`.
- **Indirect-call fast fallback**: `APS5_ALLOW_INDIRECT_CALLS=1` allows shaders with unresolvable `s_swappc_b64` to compile instead of `deferred`.
- **Structurizer guard**: bounds check on `successor` block id in `rebuildPredecessors` (SIGSEGV source on large graphs, 819+ blocks).
- **Known blocker**: game exits via user `SDL_QUIT` (MOUSEMOTION→LEAVE→CLOSE→QUIT) after ~4–12 min of shader storm (~2600 shaders) post-intro video; does not reach in-game scene without a long timeout and without closing the window.

## Verification

```sh
python3 tools/tests/test_progress.py
python3 tools/progress.py /tmp/anyps5-progress
```

The regression creates a source library and a copied wrapper, moves the wrapper to a shared builder, checks output labels and totals, then verifies real regressions and removals are still reported. With `BUILD_TESTING=ON` and Python available, CTest registers it as `progress_report`.
