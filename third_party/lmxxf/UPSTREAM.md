# lmxxf runtime source pin

- Upstream: https://github.com/lmxxf/dlss5-on-amd-9070xt-porting
- Commit: `7ef24e7c1498bce59738277e174249866608c4ed` (synced 2026-09-23)
- License: MIT, Copyright (c) 2026 Kien (`LICENSE`)
- `hip_api.h` also carries the AMD HIP runtime API MIT notice from ROCm 7.1.1

This is a **vendored source closure**, not a git submodule and not the `analysis/` clone.
Files are copied byte-for-byte from that commit unless a later commit in this tree
says otherwise.

`tools/sync-lmxxf-upstream.ps1` pins sync to `-UpstreamRef` (default `origin/main`) via
`git archive` into a temp tree — the upstream working-tree branch cannot poison the copy.
Pass `-SkipUpstreamFetch` / `-AllowOfflineUpstream` when fetch is unavailable. Live
`shaders/*.hlsl` are mirror-cleaned to the top-level glue set only (`dx12-network/` is not vendored).

## OURS (do not follow upstream on sync)

Local product / stability ownership. `tools/sync-lmxxf-upstream.ps1` **preserves** the vendor files below by default; pass the named switch only when intentionally refreshing from upstream and re-applying local patches (fail-closed).

| Path | Owner note | Sync default |
|---|---|---|
| `Development/HIP/hip_d3d12_bridge.h` | Queue drain / ClearOutput / zero-residual safeguards for `LmxxfNrRuntime` | **Preserve**; `-UpdateBridge` to overwrite + re-patch |
| `src/native_rgb_reflect.h` | Drop unused `#include "native_split.h"` so codec builds without the D3D12 network body | **Preserve**; `-UpdateReflect` to overwrite + re-drop include |
| `OptiScaler-…/dlssnr/backend/lmxxf_runtime/` (`LmxxfNrRuntime.cpp`, `LmxxfNrApi.h`, …) | OptiScaler bridge + C-ABI runtime (this product) | **Not in sync list** — never copied from upstream |
| `third_party/lmxxf/modules/` + local `hip/SHA256SUMS` gfx1201 rows | Shipping COMGR `.hsaco` built here (upstream git has no hsaco) | Built/refreshed by sync modules path, not taken from upstream git |

## FOLLOW (track upstream performance / recipe)

Synced from `-UpstreamRef` (default `origin/main`) via `git archive`. Intent: author kernel / geometry / codec / glue improvements.

| Path | Why |
|---|---|
| `hip/*.hip`, `hip/build-modules.ps1`, `hip/rtc_compile.cpp`, upstream `hip/SHA256SUMS` recipe rows | gfx1201 HIP kernels (then local rebuild of modules) |
| `Development/HIP/hip_api.h` | Loaded HIP ABI |
| `Development/HIP/hip_device_properties.h` | Device props |
| `Development/HIP/hip_reference_network.h` | HIP network (plus tiny local `#include <algorithm>` patch) |
| `Development/HIP/packed_weights.h` | Weight packing |
| `src/native_hip_network.h` | HIP entry |
| `src/native_network_geometry.h` | 720 / 900 / 1080 tiers + FIT_LARGE helpers |
| `src/native_input_geometry.h` | Input viewport / fit |
| `src/native_lab_paths.h` | Paths, typed views, weight IO |
| `src/native_game_codec.h` | Scene encode / decode host |
| `src/native_game_rgb_input.h`, `src/native_rgb_texture.h` | RGB IO |
| `src/native_device_identity.h`, `src/native_pinned_resource.h`, `src/native_pso.h`, `src/native_shader_cache.h` | Supporting glue |
| `shaders/*.hlsl` (top-level live glue only) | D3D12 glue; mirror-cleaned; `dx12-network/` not vendored |

## Excluded on purpose (not vendored)

- `src/native_submission_order_probe.cpp`, ReShade / MinHook addon
- `src/native_pre_upscale.h` (FFX replay; not a general splitter)
- `src/native_text_overlay.h`, `src/native_game_oneshot.h`, F6 overlay
- `src/native_game_frame.h` (`ProcessSubmittedFrame` convenience host)
- D3D12 network body (`native_split.h`, `native_actual_network70.h`, vit/c32/preblock, `shaders/dx12-network/`, etc.)
- `src/native_temporal_*.h` (first product version is history off)
- `Development/` notes, benchmarks, and `.ps1` experiments
- Upstream `OptiScaler-DLSS5-AMD-*` packages, weights, and gitignored `.hsaco`
- Magpie packaging

## Patches applied in this tree

### General Headers
1. `NativeLabRoot()` still matches upstream (may fall back to `D:\\DLSSNR-Lab` when no `DLSS5-AMD\\native-game-flags.txt` is found). Product installs write that flags file beside the game.
2. `native_rgb_reflect.h` (**pinned**): dropped unused `native_split.h`; codec compiles without the D3D12 network body. Sync preserves it unless `-UpdateReflect`.
3. `SetNoise` skips the 201 MiB buffer when `fast_prefix` is on.
4. `#include <algorithm>` for MinGW/MSVC `std::sort` / `std::min` in `hip_reference_network.h` and `hip_d3d12_bridge.h`.

### `Development/HIP/hip_d3d12_bridge.h` (Vendor-Pinned & Patched)
> [!IMPORTANT]
> See **OURS** above. Sync preserves this header by default; only pass `-UpdateBridge` when intentionally pulling upstream bridge changes and verifying re-applied patches (fail-closed anchors + marker check).

### `src/native_rgb_reflect.h` (Vendor-Pinned & Patched)
> [!IMPORTANT]
> See **OURS** above. Upstream still `#include "native_split.h"` even though `NativeRgbReflect` does not use it; that would pull the excluded D3D12 network body. Sync preserves our header (include already removed) unless `-UpdateReflect`, which re-copies upstream then drops the include again.

1. **Queue Drain Completion Verification**: In `WaitForSubmittedWork()`, additionally checks `fence->GetCompletedValue() >= target` after `WaitForSingleObject` returns `WAIT_OBJECT_0`, preventing queue drain race conditions.
2. **Zero-Residual Fallback Path**:
   - `ClearOutputAsync()`: clears `output.mapped` via `hipMemsetAsync` and synchronizes the HIP stream.
   - `ClearOutputD3D12(targetQueue)`: synchronizes HIP stream first, then stages a zero-clear to `output.resource` on the target queue via a dedicated upload staging buffer (`zero_upload`), fences completion, and waits safely.
   - `ClearOutput(targetQueue)`: unified entry point for a submitted producer and an unsubmitted consumer. It validates the target queue, clears output, marks a later bridge-queue consumer pending for `WaitForSubmittedWork()`, and advances to the phase appropriate for whether the consumer was already recorded. The caller must drain other queues that previously used the output and a consumer submitted on a different queue.
3. **Clear Resource Lifecycle Management**:
   - Creates dedicated `zero_upload`, `clear_alloc`, and `clear_cmd` only on the first D3D12 fallback; normal `Create()` has no clear-only allocations.
   - Releases resources in destructor only after ensuring all in-flight GPU work has completed (`clear_submission_unconfirmed` check and `WaitForSubmittedWork()`).
4. **Failure State Recovery in Submit Notification**:
   - In `NotifyOutputSubmittedIfRecorded()`, if `failed` is true, safely resets `phase = Phase::Ready` without asserting `QueueContract`, allowing safe teardown or re-initialization.
5. **Runtime Opt-In and Consumer Queue Lifetime**:
   - `LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK` enables recovery in the C ABI; the default keeps strict enqueue errors.
   - Queue mismatch recovery waits for submitted work on both the original session queue and the queue supplied to `EnqueueHip` before zeroing. The Runtime retains and drains the supplied queue before frame reuse or destruction; callers must synchronize any additional queue that reads the output.

## Shipping modules (`.hsaco`)

Upstream **does not** publish `.hsaco` on git (`/release/` is gitignored; no GitHub release assets for modules).
Release zips copy `third_party/lmxxf/modules` as-is; GitHub Actions does **not** rebuild HIP kernels.

`tools/sync-lmxxf-upstream.ps1` therefore:

- syncs hip *sources* and upstream's non-gfx1201 `SHA256SUMS` rows from upstream git. The `gfx1201/` rows stay local: after the module refresh they are rehashed from `third_party/lmxxf/modules`, because upstream's rows come from a different COMGR and never match local builds;
- by default runs `hip/build-modules.ps1 -Targets gfx1201` into a local build dir, then copies into `third_party/lmxxf/modules`;
- or accepts an explicit `-ModulesPath` to already-built flat gfx1201 `.hsaco`;
- **fails closed** if hip recipes (`*.hip`, `build-modules.ps1`, `rtc_compile.cpp`) change but modules were neither rebuilt in the same run nor changed, if a module upstream lists is missing, or if robocopy fails.

Use `-SkipModules -AllowStaleModules` only for intentional header-only syncs. Use `-NoBuildModules` with `-ModulesPath` when modules were built offline.

## Upstream Contribution & Decoupling Roadmap

1. **Keep Recovery Policy in Runtime**:
   - `network`, `output.mapped`, and `network->Stream()` are private within `D3D12Bridge`; its public `ClearOutput` is the minimal safe transport primitive.
   - The C ABI Runtime decides when to invoke that primitive and owns the cross-queue drain contract. The upstream contribution should include both layers.
2. **Instance Config Refactor**:
   - Process-global geometry/env -> instance config.
3. **Proxy Forwarding**:
   - Host P1: List1-10 command-list proxy forward (then hooks). Not this vendor tree.

## Runtime ABI (this tree)

C ABI in `include/LmxxfNrApi.h`. MSVC (primary) or MinGW (fallback) `tools/build-lmxxf-runtime.cmd` compiles the HIP bridge and codec into `LmxxfNrRuntime.dll`.

- Modules: `third_party/lmxxf/modules` (COMGR gfx1201 hsaco, tracked in git; built from current `hip/` + local COMGR (see modules/README.md Commit Base)).
- Weights: `LMXXF_WEIGHTS_DIR` tiled assets (not 0.24.2 `HIP/`).
- `QueryCapabilities.hip_ready` stays **0**. Product wiring lives in OptiScaler `lmxxf_runtime` / `LmxxfWired()` (not this vendor doc alone).
