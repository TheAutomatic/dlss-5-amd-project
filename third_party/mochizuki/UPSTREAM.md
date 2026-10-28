# Mochizuki Vulkan NR core

- Source: https://github.com/mochizuki0323/DLSSNR-AMD
- Product pin: `c46a5edec8c89ca1c7645cde886335c345eebef0` on the contribution fork TheAutomatic/DLSSNR-AMD, branch `codex/rdna3-aco-port`. This is not an official mochizuki0323 release. It starts from official `cc27501dc116bdcfbb43612e497c5ca12497574a` and adds the RDNA3/RDNA3.5 FP16 WMMA network, gfx11 PAL import, per-target ACO bundles, and runtime FP16 twins. The vendored core can choose that target while a network is constructed. The product adapter, shader package and installer do not yet. See [the paused migration progress](../../docs/rdna3-migration-progress.md).
- Previous pins, kept only as history: Windows 0.0.6 `191b97b54175d50c7ec5992b54322dd3eb0ef8ac`, Windows `a9b13dd4cdb5f4bf43db358492d72e6e5e2acd67`,  Linux-only v0.0.4 tag `9e4574e1812d7d0632d6db02721a20c9617be434`, and the old Windows v0.0.3 baseline `82560c4fbfaac347fc5e22c22025191402ae916b`. Do not mix those trees back in.
- License: MIT for this core; the original LICENSE is retained. The product adapter under `dlssnr/backend/mochizuki_runtime/` comes from MatheusFerreiraS/neural-amd-opti `283fdb0a1df9e9c4c78de0d2d848e775cf6ae8ae` (GPL-3.0). The combined runtime is distributed under GPL-3.0, with the MIT notice retained.
- Closure: `windows/src/core`, `windows/shaders` (rdna3 and rdna4), `windows/build` (recipe, unroll, quad-quant, `arch/rdna3.sh`, `arch/rdna4.sh`), `windows/data/aco` flat records plus `rdna3` and `rdna4` records, shaders, shells and aliases, `linux/shaders` (rdna3 and rdna4), `linux/build/build_network.py`, `linux/build/arch/rdna3.sh`, `linux/build/arch/rdna4.sh`, and `linux/package/model-tools`. The closure also includes `linux/data/int4/rdna4`, Windows INT4 ACO records, the INT4 weight generator, RenoDX core, shaders and their MIT notices. Patch 12 adds weights-only installation extraction; the weight parser, unpackers and model hash lists remain unchanged.
- Not imported: `windows/src/pe`, official installers, the Vulkan loader, the ReShade add-on, OptiScaler-NR package scripts, DX9/DX10/DX11 hosts, upstream Linux host and micro-test tools. Those were read for the device-identity and PAL behavior the adapter reuses. INT4 and RenoDX processing use this product's runtime and Ins menu; upstream Present/menu/screenshot hooks are not imported. RDNA3 does not receive the RDNA4 INT4 record set.

The source pin and product feature patches are machine-readable in
[tools/mochizuki-sync/manifest.json](../../tools/mochizuki-sync/manifest.json).
[Replay verification](../../tools/mochizuki-sync/README.md) reproduces 1029 pinned files and nine patch-created files (conversion shaders, motion rescale, model profile and exposure continuity) from the pinned Git objects plus the rebased patches, without overwriting
vendor. Patches whose files the contribution already contained were dropped. Where several product patches touched one file, the surviving later patch carries that file's full product delta, including thread-local ACO ownership, inline compute, recording retention and timing harvest. [Extension decisions](../../docs/architecture/mochizuki-upstream-extensions.md)
record their consumers, upstream suitability and missing validation.

## Official behaviour taken as-is

These local diffs were removed because the official tree now covers them:

- HDR encode and transfer hue. The previous Windows `runtime_encode.comp` already matched official, so the continuing "HDR port" is gone with the shader replacement.
- Driver 26.9.2 / 32.0.32015 cooperative-matrix unroll and ViT tail. Official `UNROLLED` covers `fswin_t`, `ffwd3_t`, `attn` and `vit_attn`. The local import of neural-amd-opti `be7bf0a` is not reapplied. Do not reuse old SPIR-V as ACO shells.
- `NR_EDGE_BODIES=0`. Official `pipelines.json` sets the edge defines it wants, including `NR_EDGE_BODIES=1` and `NR_EDGE_FORM=2` on some pipelines. `rdna4.sh` adds `-DNR_TCHAIN_TILES=1`. The manifest allow-list accepts `tchain`.
- Enlargement. `Controls::transfer` is 0 matched residual, 1 edge-aware, 2 classic. `classic_scaler` is 0 bilinear, 1 Catmull-Rom (official default), 2 Lanczos3, 3 FSR1. The push constant is the official 48-byte `TransferPush`. The old `enlarge_mode` field and 40-byte push are gone.
- Native store. `store_native` writes the answer back into the kept frame for the formats official lists and skips `full_out` plus the alpha pass. `NR_KEEP_RGBA32F=1` would force the old RGBA32F keep; this product does not set it.
- Build progress. Official atomics `g_build_stage`, `g_build_pipes_done` and `g_build_pipes_total` replace the local progress callback. The host overlays stage names while its own progress flag is active, and clears stage 0 around the runtime constructor so the later "Saving shader cache" text stays. The core is not reset at the end; official leaves stage 5.
- Split GPU timing averages. `last_gpu_ms`, `average_gpu_ms` and the `average_network_ms` EMA stay official.

## Residual core patches

The cancellation pointer and new output policy flags are opt-in; the compiler path
keeps the official default. Timing validation changes diagnostics. The current local
ACO thread-local configuration changes the official process-wide semantics and must
be redesigned as an explicit scoped override before proposing it upstream; the
official PE's configure-before-background-build route must keep working.

1. Build cancel, only when `NR_NO_MAIN` is set (`nr_runtime.cpp` defines it before including `nr_graph.cpp`). `nr_runtime.hpp` holds `build_cancel` and `BuildCancelScope`. The pipeline loop saves the cache and throws `NR build cancelled` before the next pipeline. Official has no cancel. Upstream direction: a cancel flag in the official graph if one is added.
2. `RuntimeConfig::compact_transfer` and `separate_answer`, both default false. After official `store_native` is computed, `separate_answer` forces it off, and `compact_rgba` reuses the RGBA32F keep when compact is requested and `store_native`, `keep_native`, 8-bit rounding and native compose are all off. The product host sets compact on (`MochizukiCompactTransfer=true`) and sets `separate_answer` when the user turns compact off.
3. `RuntimeConfig::first_pass_output`, default false. A second `runtime_transfer` dispatch keeps pass 0 in a full-frame RGBA32F image, using the official 48-byte push. Create refuses native compose, mask and `max_passes < 2`. The host export stays opt-in.
4. Timing serial. The timestamp pool masks deltas with `timestampValidBits`. A sample is accepted only when the slot has passes and every network delta is inside the total. `gpu_timing_serial()` advances only then. `last_network_ms()` and `last_other_ms()` expose that sample. The official EMA remains.
5. `--glslang` on both `windows/build/build_network.py` and `linux/build/build_network.py`. The default path is still the official relative `toolchain/glslang/bin/glslang`. The product builder passes the pinned Windows glslang.
6. ACO builder state (`06-aco-builder-state.patch`): thread-local bundle selection, `own_directory()` to ignore external selection and diagnostic mixing, import count, shared cache identity, typed Vulkan errors and propagation of fatal cached-import errors. The product configures both device and builder threads. This adapts Overclockers' PR #5 design; it is not a drop-in replacement for the official process-wide header. Upstream preparation must separate generic error fixes from an opt-in isolation API and review device-specific cache identity.
7. Unused `up_shown` (`07-drop-unused-up-shown.patch`). Below full model scale, official allocates a second full-frame RGBA32F image and binds it to `runtime_upscale` and `runtime_transfer`. Those shaders declare the binding and never read or write it. The patch removes the image and the binding. `up_result` remains the classic enlargement target.
8. RADV driver compile policy (`29-radv-driver-compile-policy.patch`): a nonempty Linux bundle can select its graph/SPIR-V without importing PAL records, compacting Windows's extra 8-byte arena device-address push constants (`act_lo`/`act_hi`) out of `gemm*` and `vitattn` so Linux SPIR-V reads its `remap` and `tc_rec` fields at the offsets compiled into the Linux shaders. The adapter selects this only from the actual device's RADV driver ID, carries it across the background build key and clears builder-local configuration on exit. Required selected-model kernels, temporal variants and host shader constants are validated before pipeline compilation. Missing assets or compilation failure bypass NR rather than compiling the Windows network shaders; INT4 remains unavailable in this mode.
9. Recording-resource retention (`30-recording-resource-retention.patch`): optional Windows `Runtime::begin_recording_resources()` / `end_recording_resources()` scopes return a `shared_ptr<void>` that owns fresh direct/frame/depth descriptor pools and image views, per-recording timestamp query pools, plus shared ownership of feature histories referenced or released inside the scope. Retained command lists keep that handle through all executions and until replay is impossible; later recordings cannot reset or reuse its pools or destroy its feature images. Total/network/stage timing remains nonblocking; pending samples hold weak references rather than extending recipe lifetime. End the scope on failure too. The adapter retains participating runtimes (including a `carry_history` source) and caller images separately and releases scope handles before those runtimes. Unscoped callers keep the original rings, retirement counts and recorded commands. This adds no submission, wait, device ownership, pin change or public C ABI field.
10. RenoDX recording-resource retention (`31-renodx-recording-resource-retention.patch`): Windows `renodx::Stage` offers matching begin/end scopes. The returned handle owns fresh borrowed views and pools, scope-private cached descriptor-set pools and shared references to stage images (including dynamically retired look histories and motion/exposure images). Optional stage profiler queries also belong to the recipe; pending samples use weak references. Later recordings cannot recycle retained resources. Release the handle after completion and replay invalidation, before the Stage; caller-image lifetime, scratch serialization and temporal-state commit/discard policy remain adapter responsibilities. Unscoped ring/cache/retirement behaviour and the source pin remain unchanged.
11. Explicit recording timing harvest (`32-recording-timing-harvest.patch`): `Runtime::harvest_recording_timings()` nonblockingly reads pending recipe queries while the caller still pins their resource scope. The direct adapter calls it after the real consumer-tail certificate and before collection; relying only on the next recording loses samples when Reset destroys the weakly referenced query owner first.

`generate_device_chain.py` is not run. The core translation unit does not include `nr_device_chain_sizes.hpp`. `quad_quant_glsl.py` is unchanged and still used by the official Windows recipe.

## Product ACO and device

ACO is off unless `MochizukiAcoMode` is true. The host pushes the current INI into the session before `PrepareSession`, because the Vulkan device is created there. `own_directory()` always runs. `configure()` runs only when `aco/records`, `aco/shaders`, `aco/shaders/temporal`, `shell-aliases.txt` and the Windows `dlssnr-amd/shaders` tree are present, and every non-`nobda` network SPIR-V under `aco/shaders` and `aco/shaders/temporal` (`g_*`, `temporal_pre_fp32*`, `temporal_post_fp32*`) has a record whose `spv_hash` equals `pal::hash` of that SPIR-V (`records/%016llx.nrp`, offset basis `1469598103934665603`). A shell is the same name or its alias in `aco/shell` (if present), `dlssnr-amd/shaders` or `shaders/temporal`; otherwise the ACO SPIR-V is its own template. The cache is `dlssnr-amd/aco-cache`, created at runtime and not shipped.

`configure()` is thread-local and clears that thread's bundle, reason and import count. The product calls `own_directory()` on the session thread before device creation and again on the builder thread. Device enablement stays process-wide. A missing bundle, a hash mismatch, a driver that `Features::prepare` rejects, or an import that throws or imports nothing discards that graph and builds one native network on the builder thread. If `vkCreateDevice` rejects the optional pipeline-binary chain and the result is neither device loss nor out of memory, the product clears that thread's bundle and creates the device again without those extensions. Device loss and out of memory keep their original classification. GetInfo names the next game start only when ACO is requested on a device created without pipeline binaries. Requested, device capability, the candidate and the installed network stay separate. Effective is published in `Install` with the network and follows that candidate's mode when the request changed during the build. A candidate that does not install leaves the current effective state. The installed network keeps its pipelines until that publish.

RADV is the exception to native-network fallback: after either owned-device creation or vkd3d adoption, the actual physical-device driver ID selects Linux SPIR-V independently of the stored ACO INI value. PAL imports and `aco_effective` remain off; GetInfo reports `RADV driver-compiled Linux SPIR-V (no PAL import)`. Records, shell aliases and pipeline-binary support are not prerequisites for this mode. The Windows AMD proprietary path above is unchanged.

The product device enables `VK_EXT_pipeline_robustness` when the physical device lists the extension and the feature is on. `adopt()` reads that feature from the physical device, and `Kernel::create` then chains a robustness info. `Features::prepare` and `mark` run only after a bundle was configured. `MZ_PROBE_PIPELINE_BINARY` remains a test probe, not the ACO path.

The builder writes Linux SPIR-V to `dlssnr-amd/aco/shaders` and copies the official records and aliases beside it. Generation failure fails the build. Package and uninstall ownership is the exact relative list in `tools/install/mochizuki-owned.txt` (833 paths, including default/INT4 ACO records, INT4 recovery data and RenoDX shaders). The packager copies only those paths and ships the pin as `mochizuki-owned.txt`. It does not copy `dlssnr.bin`, `pipeline.cache`, `aco-cache`, `test.spv` or a file whose name merely resembles a package file. Uninstall deletes a shader or ACO file only when its path is on that pin, and removes a directory only when it is empty. A missing pin deletes no shader or ACO file. Unknown files stay. Linked paths stay. `dlssnr.bin`, `dlssnr-int4.bin` and `aco-cache` stay.

## Optional FP16 compute conversions

Patch 13 adds private sampled FP16 input and storage FP16 output conversions for
equal-extent frames. They are allocated only for an optional second-family or
compute-only host, and only where the official direct input/store cannot suffice.
Graphics recordings still use the original blits. Compute input uses texelFetch
into RGBA32F; compute output stores RGBA32F into RGBA16F and copies to the caller.
No UNORM conversion, new tone mapping, model arithmetic, ABI or ACO record changes.
The product budgets both optional images conservatively before a network build.
The two shaders are compiled in both recipes and included in the ownership pin.
Reduced model scale, motion needing a blit and first-pass export retain segments.

## Pins

- Vulkan-Headers `e3b1eec08173d6b825cd3ac88c885a63b621504a` (1.4.357).
- Windows glslang 16.5.0, archive SHA-256 `06b71298b750268c127f2ee7ae0ef7525e2068120c6c8a3a08b2f58ca6f325ce`.
- The official Linux glslang tarball SHA-256 is `b9b1f96acb898a62251b171f7695efcecfc206a530299054071919b06820f657` (same 16.5.0, different archive). This build uses the Windows pin for both shader trees. The loaded ACO corpus is `dlssnr-amd/aco/shaders` (network and temporal). A 4-of-56 comparison against Windows `dlssnr-amd/shaders` is not that corpus: those Windows shaders are the native shell and runtime source. Preflight accepts the bundle only when every hashed ACO network SPIR-V matches its record contents. Device import is a separate result from `tests/mochizuki/run.cmd aco` on the selected adapter.

The 599-entry `dlssnr.bin` comes from the user's own 310.8.0 or 310.9.16 DLL;
the 255-entry `dlssnr-small.bin` requires 310.9.16. The descriptor, resource-aware
unpacker and hash lists follow the new pin. Neither DLL nor extracted model is in
this repository. See [0.0.6 integration](../../docs/architecture/mochizuki-006-integration.md)
for product selection, validation scope and remaining GPU checks.

## Measured

On 2026-10-11, `tests/mochizuki/run.cmd aco` selected AMD Radeon RX 9070 XT (vendor 0x1002, device 0x7550). `exports/mochizuki-runtime/mochizuki_nr.log` recorded 58/58 record matches, Vulkan driver 26.8.1 (LLPC), driverVersion 0x0080018b, and `ACO network ready: 29 imported pipelines`. The graph builder counted 24 kernels. The test printed `ACO_FRAME_OK 256x256`, `ACO_SWITCH_NATIVE_OK`, `ACO_SWITCH_ACO_OK` and `ACO_CONTRACT_OK effective=1`. The same command then printed `ACO_DEVICE_FALLBACK_OK` for a forced pipeline-binary device rejection, `ACO_BUDGET_KEEP_OK effective=1` when the second frame-buffer admission was refused, and `ACO_FALLBACK_OK` for the missing-bundle, missing-alias and corrupt-record fixtures. The same adapter then ran `tests/mochizuki/run.cmd aco-rapid`: a native candidate that installed after ACO was requested again printed `ACO_RAPID_REENABLE requested=1 effective=0`, and the following ACO network printed `ACO_RAPID_OK`. The machine driver was left at 32.0.31041.1004.

## Decoder-tail synchronization correction (2026-10-12)

Former `09-aco-decoder-tail-barrier.patch` adapted the graph fix by **Overclockers** from
[project PR #5](https://github.com/TheAutomatic/dlss-5-amd-project/pull/5), commit
[`60f9202fd6bc0384d1d4615658cee2ddca9b3004`](https://github.com/TheAutomatic/dlss-5-amd-project/commit/60f9202fd6bc0384d1d4615658cee2ddca9b3004).
The official 0.0.5 graph now includes this fix, so patch 09 has been removed.
The C64 persistent decoder to fused C32 pair is no longer admitted by `tchain_pair`.
The existing inter-dispatch barrier and arena lifetime boundary are therefore
retained together. Earlier decoder/encoder/C512/ViT chains keep their official
schedule. This is a dependency correction, not a product synchronization control.

Moving-input, continuous temporal byte comparison and retained-recording tests
are adapted from the same contribution. Local test adaptations cover 30-layer
capacity/selection, reduced-resolution classic enlargement and pass-0 output.
The all-barrier oracle is built into a dedicated test-only DLL from the same
source/graph/arena/records, changing only the runtime barrier skip in a private
generated translation unit. It has no production build manifest, menu or ABI flag.

Local RX 9070 XT validation passed cases 1–26, 42 and 45 of the 45-case fixture:
128 static/moving frames at 256/512, FP16/sRGB, one/three passes and preprocessing,
16 moving frames at 2258x1270, two eight-frame odd-size cases at 1129x635,
30-layer capacity with pass 15 selected, and reduced-resolution classic pass-0.
Every completed case matched the oracle byte-for-byte, remained active rather
than passing through, and replayed its retained recording. Full local matrix was
stopped at user request; remaining cases and complete CI were not run locally.
ACO inline also passed 120 fresh 256x256 recordings with stable queue identity,
deferred collection, cross-queue replay and 100% temporal history consumption.
These are fixture results, not new game acceptance or FPS measurements.

## Not claimed

No game scene was part of those historical checks. Official timing numbers are not reused as local results. INT4 was off in those checks and remains off by default. Historical 0.0.6 integration and validation scope are recorded in [the integration review](../../docs/architecture/mochizuki-006-integration.md); [the 0.0.5 review](../../docs/architecture/mochizuki-005-integration.md) retains the earlier evidence. The same-repo draft's explicit barrier switch was not added.

## Windows 0.0.7 product scope

Natural and Cinematic now use the original post-network style grade; Standard does not run that extra grade. Both native-driver and ACO builds carry the new shaders. The default ACO corpus has 62 records and INT4 has 75, matched against the generated SPIR-V; the exact install/uninstall pin owns all 833 generated paths. The two ordinary models still support driver compilation with ACO disabled. INT4 remains Original-only and requires effective ACO; model and compiler selection use the existing product controls.

Patch 24 is removed because the original now has the same nonnegative HDR reference. The original also removes the unused up_shown allocation; patch 07 retains only the local binding/layout changes. The local pack_img destruction fix remains in patch 11. Pass-capacity reuse, failure service, extent settling, recording completion and exposure continuity remain product requirements.

Patch 27 adds optional construction policy, preserving the original defaults. The product fixes motion_without_depth to 1 because it supplies no game depth, disables cross-runtime pipeline registry reuse to preserve independent ACO import accounting and whole-graph fallback, and disables unused Stage Facts GPU readback. Required grade shaders must exist before the product publishes a usable network; incomplete assets report a full-package reinstall instead of quietly rendering the old grade. Public host/runtime C ABI layouts are unchanged.

Patch 28 (`private-pipeline-pin-lifetime`) makes arena-level pipeline pins conditional on registry sharing. Private Kernel objects retain their own strong SharedPipeline ownership; retained recordings keep their original runtime/kernels alive. Registry/deferred ownership is unchanged. The adapter now uses nondeferred same-Session split `share_for` for exact-semantic compatible geometry rebuilds, preserving private imports and whole-native fallback. The source pin is unchanged; see the bounded construction evidence and exclusions in `docs/architecture/mochizuki-007-remaining-assessment.md`. No different-plan slot/cross-model activation or stale image-heap reuse is enabled.

The new GraphCore/GraphArenas ownership is present, but the product does not yet opt into cross-model activation sharing, deferred image heaps or RAM-offload scheduling. Its existing DRS buckets, settle policy and resource proofs remain. Core motion_point is left off: the host's existing LINEAR motion rescale must be reconciled with point sampling before changing input semantics. These are explicit deferrals, not claims that every original 0.0.7 optimization is active.

Local RX 9070 XT / 32.0.31041.1004 evidence includes all five supported model/compiler combinations across three styles with moving input and retained cross-queue replay. Six native-core comparisons (two models, three styles) matched the fixed original 0.0.7 source byte-for-byte at 256x256. Focused checks cover capacity/budget/OOM recovery, split retention, destroy tails, exposure recording, ACO rejection/corruption fallback, INT4/RenoDX, first-pass and composition. Missing grade assets were rejected with the full-package message. These results are offline/host evidence; they do not establish new game FPS, 4K memory bounds, other-driver support or a complete Inline validation matrix.
A same-process memory probe repeated the existing Original/INT4/RenoDX switching fixture three times, retaining 36 recordings per cycle before invalidation, collection and session destruction. Both the already-patched 0.0.6 baseline and the 0.0.7 candidate returned to 34,131,968 bytes of process-local DXGI usage after each cycle (fixture D3D12 resources remained alive). Measured peaks were 2,020,130,816 and 2,020,392,960 bytes respectively. This checks that the existing recovery behavior was retained in this workload; it does not demonstrate the deferred original image-heap/offload mechanisms or a game-wide leak fix.