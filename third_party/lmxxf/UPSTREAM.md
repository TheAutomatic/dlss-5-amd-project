# lmxxf runtime source pin

- Upstream: https://github.com/lmxxf/dlss5-on-amd-9070xt-porting
- Commit: `68dc099180b6b309a79751bef17046fe8d17e0e2`
- License: MIT, Copyright (c) 2026 Kien (`LICENSE`)
- `hip_api.h` also carries the AMD HIP runtime API MIT notice from ROCm 7.1.1

This is a **vendored source closure**, not a git submodule and not the `analysis/` clone.
Files are copied byte-for-byte from that commit unless a later commit in this tree
says otherwise.

## Included

| Path | Why |
|---|---|
| `hip/` | gfx1201 module recipes (`.hip`, `build-modules.ps1`, `SHA256SUMS`) |
| `Development/HIP/hip_d3d12_bridge.h` | D3D12 ↔ HIP external-fence bridge |
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
- D3D12 network body (`native_actual_network70.h`, vit/c32/preblock/split, …)
- `src/native_temporal_*.h` (first product version is history off)
- `Development/` notes, benchmarks, and `.ps1` experiments
- `OptiScaler-DLSS5-AMD-0.24.2/` package, weights, and `.hsaco` binaries
- Magpie packaging

`native_rgb_reflect.h` still `#include`s `native_split.h`, which is **not** in this
tree. Compiling the stock codec header as-is will fail until P2 extracts encode
without the D3D12 network. The HIP headers (`native_hip_network.h` → bridge →
`hip_reference_network.h`) do not need split.

## Patches applied in this tree

1. `NativeLabRoot()` no longer falls back to `D:\\DLSSNR-Lab`. Missing assets throw.

## Required follow-up (P2)

1. Process-global `getenv` / geometry statics → instance config (first version
   still freezes paper_white=1, strength=(1,1), CODEC_SRGB=0, graph=false).
2. C ABI + no C++ exceptions across `LmxxfNrRuntime.dll`.
3. Extract encode/decode so `native_game_codec.h` does not need `native_split.h`.
