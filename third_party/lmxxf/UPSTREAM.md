# lmxxf runtime source pin

- Upstream: https://github.com/lmxxf/dlss5-on-amd-9070xt-porting
- Commit: `1e708c03cc984dcf88f44336376eb1aba093e497` (synced 2026-09-23)
- License: MIT, Copyright (c) 2026 Kien (`LICENSE`)
- `hip_api.h` also carries the AMD HIP runtime API MIT notice from ROCm 7.1.1

This is a **vendored source closure**, not a git submodule and not the `analysis/` clone.
Files are copied byte-for-byte from that commit unless a later commit in this tree
says otherwise.

## Included

| Path | Why |
|---|---|
| `hip/` | gfx1201 module recipes (`.hip`, `build-modules.ps1`, `SHA256SUMS`) |
| `Development/HIP/hip_d3d12_bridge.h` | D3D12 -> HIP external-fence bridge (**pinned & patched**) |
| `Development/HIP/hip_reference_network.h` | HIP network |
| `Development/HIP/hip_api.h` | Loaded HIP ABI |
| `Development/HIP/packed_weights.h` | Weight packing |
| `src/native_hip_network.h` | HIP entry used by the game host |
| `src/native_network_geometry.h` | 720 / 900 / 1080 tiers |
| `src/native_lab_paths.h` | Paths, typed views, weight IO |
| `src/native_game_codec.h` and encode/decode HLSL | Scene encode / decode |
| supporting `src/*.h` pulled by the above | device identity, PSO, RGB input/reflect/texture, shader cache |

## Excluded on purpose

- `src/native_submission_order_probe.cpp`, ReShade / MinHook addon
- `src/native_pre_upscale.h` (FFX replay; not a general splitter)
- `src/native_text_overlay.h`, `src/native_game_oneshot.h`, F6 overlay
- `src/native_game_frame.h` (`ProcessSubmittedFrame` convenience host)
- D3D12 network body (`native_actual_network70.h`, vit/c32/preblock/split, etc.)
- `src/native_temporal_*.h` (first product version is history off)
- `Development/` notes, benchmarks, and `.ps1` experiments
- `OptiScaler-DLSS5-AMD-0.24.2/` package, weights, and `.hsaco` binaries
- Magpie packaging

## Patches applied in this tree

### General Headers
1. `NativeLabRoot()` no longer falls back to `D:\\DLSSNR-Lab`. Missing assets throw.
2. `native_rgb_reflect.h` dropped unused `native_split.h`; codec compiles without the D3D12 network body.
3. `SetNoise` skips the 201 MiB buffer when `fast_prefix` is on.
4. `#include <algorithm>` for MinGW/MSVC `std::sort` / `std::min` in `hip_reference_network.h` and `hip_d3d12_bridge.h`.

### `Development/HIP/hip_d3d12_bridge.h` (Vendor-Pinned & Patched)
> [!IMPORTANT]
> `Development/HIP/hip_d3d12_bridge.h` contains critical stability safeguards for D3D12 queue ordering and zero-residual fallbacks required by `LmxxfNrRuntime.dll`. To avoid breaking local fixes when pulling upstream, `tools/sync-lmxxf-upstream.ps1` preserves this header by default. Only pass `-UpdateBridge` when intentionally pulling upstream bridge changes and verifying re-applied patches.

1. **Queue Drain Completion Verification**: In `WaitForSubmittedWork()`, additionally checks `fence->GetCompletedValue() >= target` after `WaitForSingleObject` returns `WAIT_OBJECT_0`, preventing queue drain race conditions.
2. **Zero-Residual Fallback Path**:
   - `ClearOutputAsync()`: clears `output.mapped` via `hipMemsetAsync` and synchronizes the HIP stream, advancing phase to `Phase::OutputRecorded`.
   - `ClearOutputD3D12(targetQueue)`: synchronizes HIP stream first, then stages a zero-clear to `output.resource` on the target queue via a dedicated upload staging buffer (`zero_upload`), fences completion, and waits safely.
   - `ClearOutput(targetQueue)`: unified entry point trying async HIP clear first, then D3D12 queue clear fallback. Called by `LmxxfNrRuntime.cpp` on error paths to prevent uninitialized GPU buffers from poisoning the game swapchain or FSR pipeline.
3. **Clear Resource Lifecycle Management**:
   - Manages dedicated `zero_upload`, `clear_alloc`, and `clear_cmd` instances inside `D3D12Bridge`.
   - Releases resources in destructor only after ensuring all in-flight GPU work has completed (`clear_submission_unconfirmed` check and `WaitForSubmittedWork()`).
4. **Failure State Recovery in Submit Notification**:
   - In `NotifyOutputSubmittedIfRecorded()`, if `failed` is true, safely resets `phase = Phase::Ready` without asserting `QueueContract`, allowing safe teardown or re-initialization.

## Upstream Contribution & Decoupling Roadmap

1. **Decouple Policy to Runtime Layer**:
   - Currently, `network`, `output.mapped`, and `network->Stream()` are private within `D3D12Bridge`, necessitating clear fallback methods inside the bridge class itself.
   - Target upstream PR: propose exposing a minimal bridge query/hook or moving zero-fill / fallback responsibilities cleanly into the outer runtime wrapper (`LmxxfNrRuntime`), keeping `D3D12Bridge` focused purely on D3D12 <-> HIP transport.
2. **Instance Config Refactor**:
   - Process-global geometry/env -> instance config.
3. **Proxy Forwarding**:
   - Host P1: List1-10 command-list proxy forward (then hooks). Not this vendor tree.

## Runtime ABI (this tree)

C ABI in `include/LmxxfNrApi.h`. MSVC (primary) or MinGW (fallback) `tools/build-lmxxf-runtime.cmd` compiles the HIP bridge and codec into `LmxxfNrRuntime.dll`.

- Modules: `third_party/lmxxf/modules` (COMGR gfx1201 hsaco, tracked in git; built from `68dc099`).
- Weights: `LMXXF_WEIGHTS_DIR` tiled assets (not 0.24.2 `HIP/`).
- `QueryCapabilities.hip_ready` stays **0**. `LmxxfWired()` stays false.
