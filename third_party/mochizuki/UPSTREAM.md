# Mochizuki Vulkan NR core

- Source: https://github.com/mochizuki0323/DLSSNR-AMD
- Source snapshot: `9e4574e1812d7d0632d6db02721a20c9617be434` (v0.0.4).
- Windows baseline: v0.0.3 (`82560c4fbfaac347fc5e22c22025191402ae916b`); upstream v0.0.4 is a Linux-only release.
- License: MIT; original LICENSE retained here.
- Closure: Windows core, Windows shader sources/recipe/generators, Linux model extraction tools.
- Not imported: the official vkd3d/ReShade hosts, installers, Vulkan loader or model weights.

## v0.0.4 directed integration (2026-10-07)

Reviewed the complete 337-path v0.0.3-to-v0.0.4 diff, including Linux calibration data,
generators and hosts outside this closure. The four changed vendored Windows files
only correct comments; there is no new Windows inference implementation in that tag.
The Windows ReShade host's fifth changed file is also comment-only and is not vendored.
Menu version and weight preparation remain on the official Windows v0.0.3 baseline.

Ported the Linux HDR hue fix from `linux/shaders/passes/runtime_encode.comp` and the
`soft_knee` function in `linux/shaders/passes/runtime_transfer.comp` to their Windows
counterparts. After the luminance knee, RGB is scaled by its peak if a channel exceeds
one, preserving RGB ratios instead of clipping channels separately. The reduced-model
transfer uses the same rule. No descriptor layouts, controls, weights or history rules change.
The standalone `tests/mochizuki/run.cmd hdr-shader` test executes both shipped shaders
on GPU, covering saturated colours, ordinary brightness, white point, alpha, bounds,
and full/reduced model resolution. It is also included in the Mochizuki GPU entrypoint.

| Upstream module | Disposition and follow-up |
|---|---|
| INT4 mixed: Linux core, shaders, recipes, calibration tables, weight converter and Vulkan layer | Deferred. `nrvk::Kernel::create_iu4` captures driver binaries and rewrites WMMA opcodes via `VK_KHR_pipeline_binary`; this is not a portable GLSL precision switch. Requires separate Windows driver/output validation, model preparation and package contracts, and INI-owned controls before integration. No calibration data or extra model is shipped here. |
| Pipeline robustness override | Not applicable to this bridge. Upstream removes inherited DXVK/vkd3d-proton robust access; `Session::InitVk` creates a private Vulkan device without those enabled features. Do not inherit the Linux path's assumption that physical support alone permits per-pipeline overrides on a borrowed device. |
| ReShade enlargement (edge-aware/classic/Catmull-Rom/Lanczos/FSR1) and native-frame in-place I/O | Deferred. These add shader bindings, push constants, buffers and per-recording image views. Our retained recordings can be replayed after later frames, so an upstream fixed frame-ring retirement policy cannot replace the host's completion-based leases. Requires corresponding controls, ownership adaptation and replay/format tests. Existing matched-residual enlargement remains. |
| Network/route split timings | Deferred. Upstream changes query stride/slot interpretation; the adapter's completion-based timing serial, public info ABI and menu would need coordinated updates. Existing timings remain unchanged. |
| 32-bit staging/weight-memory reduction and host-memory retry | Deferred. This product is x64, and has its own budget/backoff/cancellation and retained-network logic. A future bounded-upload change should measure peak memory and build latency and validate failure cleanup; do not import the unrelated INT4 builder with it. |
| Linux installer, Steam layer options, OptiScaler-NR 0.8.91 binary patches, DX10 ReShade routing, INI/hotkey/log controls | Not imported. These configure a different host and API route. Our three-backend host, native D3D12 bridge, authoritative INI/menu and bounded logging remain in control. |

The model extraction scripts and 599-entry source model did not change. The source
snapshot above records the reviewed version; it does **not** claim Windows INT4 or
all v0.0.4 Linux features are available.

## Retained local adaptations

`windows/src/core/nr_runtime.hpp` and `nr_graph.cpp` carry the pipeline-build cancellation patch from
[MatheusFerreiraS/neural-amd-opti](https://github.com/MatheusFerreiraS/neural-amd-opti)
at `283fdb0a1df9e9c4c78de0d2d848e775cf6ae8ae` (GPL-3.0).
It checks a host cancellation flag between pipeline builds and preserves the compiled cache before stopping.
The native D3D12/Vulkan adapter and pipeline prewarm implementation under the product's
`dlssnr/backend/mochizuki_runtime/` originate from that same GPL-3.0 snapshot and are adapted locally.
The combined runtime is distributed under GPL-3.0, with this MIT core's notice retained.

Local adapter changes replace the reference's single mutable job with stable v2 recording
leases, retain geometry/network resources through replay and invalidation, and order the
next producer after the actual D3D12 consumer tail. Controls use the exact current package
contract and an explicit asset directory. Logging is opt-in and bounded. The core adds a
completed timestamp-read serial accessor in `nr_runtime.hpp/.cpp` so UI polls do not
duplicate GPU samples. No inference or shader math is changed by that accessor.

The shader recipe starts from upstream defaults and adapts MatheusFerreiraS's `NR_EDGE_BODIES=0` recipe on the eight
pipelines that otherwise duplicate the Swin body for image-edge masks. A local RX 9070 XT fresh-executable
1080p/R11G11B10 test reduced network construction from 133.7 s to 48.6 s; its output was byte-identical.
This is startup evidence, not a claim of game FPS gains or all-game image equivalence.
The core also reports build phases and completed main-network pipelines through a thread-local host
callback. It changes no inference math. The host displays progress without a guessed total-time percentage.
The build generator accepts an explicit shader compiler path; build tools and intermediate
output live under exports, not this source tree.

The Windows shader build also carries the cooperative-matrix driver workaround from
[MatheusFerreiraS/neural-amd-opti be7bf0a](https://github.com/MatheusFerreiraS/neural-amd-opti/commit/be7bf0a3d542b96894b61f42f2c3a389592952c8).
Only `unroll_glsl.py`, the four-source unroll selection in `build_network.py`, and
the partial-key-chunk changes in `vit_attn_vt_chunk.glsl` are imported. The local
`--glslang` option remains. Fixed-bound int/uint cooperative-matrix array loops,
including continue and subgroup-32 matrix length loops, are expanded before glslang.
The ViT tail avoids a loaded/zero matrix select and masks padding in scalar f16 values.
This targets Windows driver 32.0.32015; it also corrects the original partial-chunk
output on older drivers. Old output hashes at affected resolutions are not correctness
references. The commit's async, input, menu and runtime changes are not imported.
Rebuild with `tools/build/build-mochizuki-runtime.cmd` without `--skip-shaders`.
Do not ship local pipeline caches or prewarm manifests; cached prewarm modules are
validated against the installed SPIR-V bytes before use.

Build inputs are pinned: Vulkan-Headers `e3b1eec08173d6b825cd3ac88c885a63b621504a` (1.4.357),
glslang 16.5.0. Host compile constants are read from `windows/build/arch/rdna4.sh`, the same source the
shader recipe documents. See tools/build/build-mochizuki-runtime.cmd.

The 599-entry `dlssnr.bin` model must be extracted from a user's own `nvngx_dlssnr.dll` 310.8.0;
neither that DLL nor the extracted model is part of this repository or public package.
