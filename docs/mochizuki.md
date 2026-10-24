# Mochizuki backend (local integration)

This backend runs mochizuki0323's Vulkan network on the game's AMD adapter while the
game uses the D3D12 interface. Native Windows uses the existing interop bridge;
the Proton path records into the adopted vkd3d game's current Vulkan command buffer.
Windows RDNA4 remains the established target; Proton acceptance is pending below.
Other GPU architectures and individual games are not validated by this integration.
Recordings with unsupported state (including enhanced barriers or an active render
pass) pass through without NR on the ordinary split path. Experimental `NrInline=true`
adds a separate same-list path, including validated whole-resource enhanced-layout
interop when `LmxxfAllowEnhancedBarriers=true`; partial/pending or unknown layouts
remain unsupported. See [inline execution](architecture/nr-inline-recording.md).

The network core and shaders come from the
[official repository](https://github.com/mochizuki0323/DLSSNR-AMD).
The runtime adapts native D3D12/Vulkan interop and pipeline prewarming code from
[MatheusFerreiraS/neural-amd-opti](https://github.com/MatheusFerreiraS/neural-amd-opti).
This product implements backend selection, host recording ownership, configuration,
menus and packaging, and adapts the runtime for retained recordings, replay and
completion-based resource release. Its build-progress display and execution-history
handling are described below. Exact reused components, source pins, licenses and
local changes are in [the source record](../third_party/mochizuki/UPSTREAM.md).

The vendored core follows official Windows source `cc27501dc116bdcfbb43612e497c5ca12497574a`
(0.0.7, 2026-10-19). The 310.9.16 model is the default; the
310.8.0 original remains selectable. INT4 mixed and RenoDX processing are integrated
through this host's existing Ins menu and `[DlssNr]` settings. ACO defaults on;
INT4 mixed and RenoDX default off. The official DX9/DX10/DX11 hosts, ReShade,
Present hooks, End menu and screenshot capture are outside this Windows D3D12
integration. See the [0.0.7 adoption boundaries](architecture/mochizuki-upstream-extensions.md#windows-007-adoption-boundary) and the [historical 0.0.6 review](architecture/mochizuki-006-integration.md).

## Proton direct recording (review v7)

The reference protocol is the Linux PE `run_after` path at official source pin
`cc27501dc116bdcfbb43612e497c5ca12497574a`, not a second Vulkan device or a
side-queue approximation. The host resolves the borrowed native game command list
and queries `MochizukiNrGetDirectCommandBuffer` **before splitting**. An adopted
vkd3d device must supply its current nonzero `VkCommandBuffer`; missing interop is
an error. Native Windows returns success with zero and keeps the existing bridge.
The direct frame uses `MOCHIZUKI_NR_FRAME_FLAG_DIRECT_RECORDING` (`1u << 16`),
the exact-size current frame ABI (including depth state, extent and inversion),
and `MochizukiNrRecordDirect` only after preparation and lease attachment.
It does not use `RecordInputs`, `RecordOutputs`, `Split`, `EnqueueHip` or the
inline side worker. Host and runtime must be updated together from one package.

The pinned protocol brackets network recording with `ALL_COMMANDS` barriers,
restores imported image layouts, then invalidates native compute state with
`SetPipelineState(nullptr)`, `SetComputeRootSignature(nullptr)` and a partial
1×1 ClearUAV rectangle on a 2×1 resource. Proxy bookkeeping must agree without replaying
cached compute state over that native invalidation. Retained-list ownership,
invalidation and completion-based resource release still apply. Runtime
Begin/End execution notifications describe only the actual game submission;
completion uses the real game-tail fence, value and Signal result, never a
synthetic worker completion or an Evaluate-time completion assumption.

The load-bearing safety property is recipe ownership: patch 30 gives each direct
recording its own descriptor pools, image views, timestamp queries and retained
feature-history pins. Patch 31 adds equivalent optional recording-resource scopes
to the RenoDX stage, so multipass stage descriptors cannot wrap a recording-count
ring while an older game list is retained. Both scope handles are captured on
success and failure, remain owned through every submitted execution, and are
released only after invalidation plus the actual game-tail certificate.
Game-queue waits serialize shared activation/history GPU use; recording count
alone never certifies descriptor or image retirement on this path.

Failed direct recording does not cancel the game's submission batch: its attached
lease retains partial native commands and all recipe resources until Reset and,
if submitted, the real tail completion. Such executions never certify NR output.
Motion-vector rescaling defines a one-texel replicated edge ring before linear
blitting a DRS valid rectangle inside a larger allocation.

Core timestamps are harvested from the certified recipe's resource scope for direct
executions, including retained-command-list replays, before collection can release
its query pools (patch 32). The recording-time pending queue alone is one-shot and
cannot supply replay timings. The host requires the current-package
`MochizukiNrPollDirectCompletions(context, timingLog)` export and calls it from the
normal per-frame `PollRelease` seam under `RecordingMutex`, independently of a new
recording or recipe invalidation. It then refreshes the cached menu/overlay info at
the existing 500 ms cadence. `GetInfo` remains an atomic-only snapshot read; polling
does not record or submit GPU work, change recipe ownership, or wait for the GPU.
Before admitting another execution of a retained recipe, the runtime also harvests
its completed predecessor before replay can reset the recipe's timestamp queries.
Reads require every query's availability bit; unavailable or invalid samples do not
refresh the display. RenoDX direct recipes preserve their timestamp mask and
callback count across harvests, so replays retain the same timing boundary.
Harvest serials belong to each network generation; replacing the network or
changing pass capacity retains the session's last-known timing window and normal
freshness expiry. It does not reset the display to zero while the new generation
starts producing samples.
If timestamp support or query-pool creation is unavailable, both menu and overlay
show plain N/A rather than misclassifying it as waiting or an expired sample;
the fixed two-line menu geometry is preserved.

Direct execution diagnostics distinguish Begin admissions from successfully
submitted executions and real-tail-certified completions. An admission canceled
before submission must not advance the submitted/completed ordinals. The recorded
history-consumption bit and network-dispatch count belong to each recipe and are
published when that recipe is submitted, including replay; they describe executed
work rather than the most recently recorded list. These observations do not change
the fixed temporal parity or reset commands embedded in a retained recipe.


Known divergence risk: retained-recipe replay temporal continuity is pending
real-machine acceptance. Like the official recording model, CPU history parity
advances when commands are recorded; an old recipe replays its fixed parity.
Discard/invalidation requests a reset for the next recording, not a rewrite of
already closed commands. Test repeated Evaluate with discarded and reordered
lists, retained-list replay across resize, and repeated NR off/on cycling.


This transport change retains the product deltas: both selectable models, host
controls and saved INI preferences, multipass and first-pass output, exposure
routing, and Plan A ownership/history behavior. It does not replace the product
with upstream's overlay or controls. Implementation/source assertions are not
evidence of successful Proton rendering.

### Pending Proton acceptance matrix

All items below remain **pending hardware/game acceptance**, not passed checks:

| Stage | Required observation |
|---|---|
| First frame | Small model, one pass: first real processed frame, then sustained rendering with increasing completed frame/timing counts |
| Models and passes | Original model; multipass including first-pass output; temporal history and reset; Natural/Cinematic grade versus Standard |
| Images and lifetime | Supported input/output formats; resize/DRS; repeated Evaluate with discarded/reordered lists and retained-list replay across resize; repeated NR off/on; clean exit; resource release only after real completion |
| Game coexistence | FSR coexistence; game PSO/root-signature restoration after NR; no stale history or corruption on resumed rendering |
| Exposure | Tester chooses source 2 or 3 explicitly; record that choice and preserve the user's INI rather than rewriting it for acceptance |
| Compilation | Report cold and warm network build time separately, each with peak process RSS; do not substitute steady-state GPU timing |
| Distribution | Exercise the full dist package, recording real host/runtime hashes, model identities, shader/ACO assets, source pin, Proton/vkd3d version, driver and GPU; no placeholder package identities |

Acceptance must distinguish network construction from a rendered first frame,
and actual game-tail completion from merely recorded commands. Native Windows
bridge behavior requires a separate regression run; Proton success cannot stand
in for it.


Ins → Mochizuki → **Network model** selects **310.9.16 (new)** or
**310.8.0 (original)**. `[DlssNr] MochizukiNetworkModel=1` selects the new model;
`0` selects the original. Save Settings to persist the choice. Switching builds
the selected network in the background; the menu reports the installed model.
Both work with native compilation, ACO and RenoDX. The new model needs
`dlssnr-amd/dlssnr-small.bin`; without it, the original model runs with a log/status
message. INT4 requires the original model and active ACO. Switching models retains
the INT4 preference; Ctrl+F11 changes that preference without changing the model.

Natural and Cinematic retain the vendored core's enabled post-network style grade;
Standard is not graded. There is no product menu, INI or controls-ABI switch for
this grade. The runtime requires the complete style-grade shader set and rejects
an incomplete package instead of silently changing the picture. Host and runtime
must come from the same complete package; control-block size mismatches are rejected.

## INT4 mixed and RenoDX

Ctrl+F11 switches the default/INT4 request without changing ACO. Save Settings
persists it. The upstream End overlay is not installed; End keeps the host NR toggle.

Under Ins → Mochizuki, **INT4 mixed** (`MochizukiInt4Mixed`) sits directly below ACO. On the Model page, enable
**RenoDX processing** (`MochizukiRenoDX`). Mode changes rebuild the network in the
background; effective status is shown in the menu. INT4 requires ACO and matching
generated weights. If ACO import is unavailable, the complete default network is
used and INT4 is shown as inactive. Native INT4 is not implemented upstream.

RenoDX has separate network response, pass stack, composition, resolution, look,
encoding and guide controls (`MochizukiReno*`). Native picture settings are retained
while their replacement controls are shown. RenoDX runs 1–4 passes normally, up to
30 when unlocked; passes 5 onward use pass 4's controls. Game exposure uses the
host's validated readback (one frame late), with the upstream content meter when
unavailable. Motion uses the existing host bridge. Depth and Present-only inputs
are not available on this route. RenoDX timing covers the completed whole stage
and sums the network callbacks across every executed pass, not only the last pass.
Those callbacks include core preparation and composition as well as network kernels;
they are not identical to the standard route's kernel-loop timing boundary.
Person protection is disabled in RenoDX mode because the native first-pass contract
is unavailable; the original settings are preserved.

`MochizukiNoiseEveryFrame` advances the upstream noise seed when enabled (default
false). Ins and `OptiScaler.ini` own all these settings; upstream INI/environment
variables cannot replace the selected model or picture options.

## Installation

1. Install a current AMD display driver with Vulkan support. HIP is not used by this backend.
2. Extract the complete OptiScaler package. It contains `MochizukiNrRuntime.dll`,
   `dlssnr-amd/shaders/` and `dlssnr-amd/aco/` (records, shell aliases, and the ACO
   network, runtime and temporal shaders), `dlssnr-amd/int4/`, RenoDX shaders and the INT4 model tools. The runtime and those assets must match the host in that package.
   ACO is on by default (`MochizukiAcoMode=true`); a change applies on the next game start.
   A missing or rejected bundle, or a device/driver without ACO, keeps the driver's compiler.
3. Supply your own `nvngx_dlssnr.dll` **310.9.16** (SHA-256
   `2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f`)
   to extract both models. **310.8.0** (SHA-256
   `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e`)
   provides only the original model.
   The DLL and extracted weights are **not included** in our package.
4. With Python 3.10 or newer installed, put that DLL beside `Setup.bat` and run
   `Setup.bat` to select Mochizuki and extract the detected model.
   `Mochizuki-Model.bat` remains available separately; you can drag a DLL/ZIP onto it.
   Extraction only reads data and validates every model entry; it does not load the DLL.
   The results are `dlssnr-amd/dlssnr.bin` (original) and, with 310.9.16,
   `dlssnr-amd/dlssnr-small.bin` (new). Models extracted with the official tools
   are also usable at these paths. Setup and the standalone model tool derive
   `dlssnr-int4.bin` from it, verify the upstream SHA-256, and only then replace
   an outdated INT4 model. Updates reuse a matching installed INT4 hash without
   converting again when `dlssnr.bin` also matches the canonical Original SHA-256.
   Other source packs still undergo conversion and output-pin validation. All three
   model packs persist in the game directory independently of the extracted package;
   uninstall preserves them. Launcher status and model hints appear only for checked backends.
   If no working Python 3.10+ is found, Setup and the standalone model tool explain
   how to open Microsoft Store (Windows Store), search for **Python**, and install
   version 3.10 or newer. Close and reopen the installer afterward. They also try
   `python3` and `py` when `python` is unavailable; a Windows execution alias alone
   is not accepted as a working interpreter. Existing extracted models need no Python.
5. Run `Setup.bat`, select mochizuki alone or install all available backends. Choose
   the active backend. Ins exposes the same selection; with `NrConvenience=0`, save
   and restart after changing it. Unselected backends are not initialized.

The first network build can take minutes. Ins displays **building the network**.
while building, a noninteractive panel also appears at the bottom right, even with
Ins and the FPS overlay closed. It shows the phase, elapsed time and completed
main-network shaders. Other phases use an indeterminate activity bar; shader counts
are not an estimate of total remaining time. A step without an update for 30 seconds
shows its waiting time. The panel disappears when building ends or NR is disabled.
Build logs report at most about once per ten seconds plus phase completion.
Frames pass through until it is ready. Changing resolution, model scale or pass
capacity can require another build. Cache files belong to this machine and driver
and are not distributed in the package. NR off releases the active session; closed
command lists keep resources until they are invalidated and GPU work completes.

To extract from a source checkout:

```powershell
python -X utf8 tools/install/mochizuki-model.py <user-DLL-or-ZIP> <output>/dlssnr-amd/dlssnr.bin --work work/scratch
```

Installation uses `inspect_nr.py --extract-weights`: the existing PE/resource and
weight parser emits the manifest, weight container and layer payloads, without
scanning CUDA ELF modules or writing DLL/cubin copies. The vendor shell installer
uses the same mode. Developer analysis remains available through `inspect_nr.py`
(inventory) or `--extract` (DLL, weights and cubins). No extraction code loads the DLL.
The source SHA256 pins, 599-entry original and 255-entry new model lists, per-entry
SHA256 verification and packed format follow upstream. Both requested models
validate before the product installer replaces either destination. Patch 12
records the weights-only vendor extension.
`tests\mochizuki\run.cmd model` covers mode separation, skipped scanning, DLL/ZIP,
malformed weights, path safety and failed-source preservation without a GPU.

Validated on 2026-10-13 with the supported 310.8.0 DLL: full analysis extraction,
weights-only installation from DLL, weights-only installation from ZIP, and the
existing tested model all produced 147,756,560 identical bytes, SHA256
`2b41c888cf4155b8958c665ba64018ab0bd25c85fc71a2b6db86d0d04d1f7fbd`.
All 599 entries passed the existing verifier; changing one extracted tensor was
rejected before a model was written. The 12-patch/173-file upstream replay passed.
The shell entry point received LF-normalized syntax validation on Windows; a
Linux installation was not run.

## ReShade compatibility

ReShade as `dxgi.dll` with OptiScaler as `winmm.dll` has been validated in
Where Winds Meet (DX12) with ReShade 6.8. The list-identity fix alone was insufficient:
ReShade's queue exposes its device wrapper, while its fences expose the native
device. Comparing those pointers stopped the session at `EndRecordingExecution`.
The runtime now also accepts the device identity proven by a synchronization fence
created by that same session. Foreign devices and unresolved submissions remain
rejected; synchronization and resource-retirement rules are unchanged.

`network ready` means construction completed, not that NR rendered a frame.
Confirm an increasing frame count and nonzero timings alongside the image effect.
Submission failures preserve their originating-thread error and replace stale
building status; bounded logs trace the first two recordings of each session.
This validation does not cover every game, ReShade add-on or loading arrangement.

## Controls

All keys below are in `[DlssNr]`, prefixed `Mochizuki`, independent of Daniel and
lmxxf settings. The complete defaults are in the shipped `OptiScaler.ini`.

| Group | Controls and behavior |
|---|---|
| Model response | Model Intensity, Style (official Style 0–2), LocalTone, LocalStructure, SkinStructure (-1 follows Structure), AutomaticMask |
| After the model | ApplyModel (Transfer), DetailStrength (TransferStrength), ColourStrength, MaxRatio highlight guard, enlargement and ClassicScaler |
| Resolution / Model passes | ModelScale (WorkingScale, 0.25–1), Passes (1–3). Allow up to 30 layers raises that control to 1–30; it does not select 30 by itself. Each count runs the model that many times in one network, one history image per layer, then one composition. Layers after 3 inherit pass 1 with local tone 0. The selected count rebuilds; the VRAM budget can refuse it |
| Temporal history | Temporal and HistoryStrength. NR → SR drops jittered motion. Mochizuki SR → NR passes the game vectors through and the runtime scales the render-resolution motion region |
| Before the model | Preprocess, PreprocessExposure (Off/Auto/Fixed), PreprocessBiasEv, PreprocessCurve (None/Neutral/Reinhard/Filmic/GT/ACES/AgX), PreprocessContrast and PreprocessSaturation. The transform changes the model input and is reversed from its answer |
| Advanced | WhitePoint for linear input (the manual level, which trims the game's exposure when the host supplies one and the runtime's own GPU meter otherwise), LinearInput (Auto/Linear/Encoded), MaxPasses (0 grows capacity as needed and retains it on pass reduction), DynamicResolution (Exact/Auto bucket/Always bucket) |
| Pass 2/3 | Explicit override plus that pass's style, intensity, tone, structure, skin structure and mask. With override off, inherit pass 1 but use zero LocalTone |

The NR menu uses a clickable pre-SR flow chart. **Prepare NR input** contains model
resolution (WorkingScale), preprocessing and advanced input encoding/white point/DRS. **NR model**
contains pass count, model response and enabled Pass 2/3, temporal history and prebuild
capacity. **Apply NR edit** contains the after-the-model correction and the shared intensity and
stabilizer. Processing order, ACO and INT4 checkboxes sit above the pages. The model page opens first; page buttons remain available when the chart
is collapsed. Compatibility and diagnostics remain separate tools below the page.
Disabling a pass hides its controls without deleting its settings. Model resolution
displays as a percentage; resolution, pass count and prebuild-pass edits commit on
release or completion of keyboard/text editing, avoiding rebuilds during dragging.

In automatic capacity mode (`MochizukiMaxPasses=0`), changing 2→1→2 runs one or
two passes of the same network. Lowering the active count keeps the larger allocation;
it reduces GPU work but does not reclaim that network's extra memory. An explicit
prebuild count requests exact capacity, at least the active count, and can rebuild
to a smaller network. Resolution, scale, encoding and preprocessing changes still
have their own rebuild requirements.

If a capacity expansion is refused by the process VRAM budget or its build fails,
a compatible existing network and its frame buffers continue running. The status
reports requested passes, effective passes, capacity and the waiting reason. Budget
refusals retry at most once per ten seconds; out-of-memory builds retain their
bounded backoff. Required candidate buffers must be allocated before installation;
failure discards the candidate. Without a compatible ready network, frames pass
through without NR. The budget is a Windows process allowance, not a physical VRAM
free-space reading, and requested passes cannot be guaranteed under memory pressure.

Auto DRS buckets changing input subrects to reduce repeated network builds. The
network repeats the subrect edge into the unused bucket and resets history when the
valid extent changes. Unsupported blit formats fall back to exact extents.

When DRS is enabled but the current input still uses exact extents (for example,
whole allocations in Auto mode), a different size must remain requested for 300 ms
before replacing a ready network. During this wait, incompatible frames pass
through with their original colour; a return to the ready network's size resumes
NR immediately and cancels the pending resize. Sustained changes still rebuild.
Startup, same-size setting changes and actual DRS bucket growth do not incur this
extra wait. This prevents short size excursions from triggering two unnecessary
builds; it does not remove the cost of a genuine resolution change.

**Compatibility & Scheduling → Early command-list wrap** controls the shared
submission hook (`LmxxfEarlyExeWrap` in the INI, also used by lmxxf). Auto admits
early EXE calls in Unreal/Forza and early UnityPlayer.dll calls in Aniimo.exe.
Other Unity games stay on the normal post-swapchain policy unless explicitly
forced on. Force on admits both EXE and loaded UnityPlayer.dll callers; force off
disables both early paths. Save and restart after changing this setting. Forced
early wrapping can break initialization in untested games; it is not a general
Unity compatibility guarantee. The hook still accepts only DIRECT command lists
and requires a ready submission path. This addresses retained boot lists missing
NR, independently of the transient-resolution handling above.

Overall Intensity and Residual Stabilizer remain shared output effects. Intensity 0
or ApplyModel off still incurs network work; disable NR to avoid it. Group reset only
resets that group. Reset NR settings resets mochizuki and shared effects/timing,
preserving other backend controls, backend selection and hotkeys. Reset this page
resets only that page's mochizuki fields; on the model page this includes hidden
later-pass overrides. Shared effects have their own reset.

Ins and Page Up/Down show completed **Vulkan core total** GPU measurements.
The compact Mochizuki label is NR GPU. Diagnostics splits the same sample window
into network and other core processing; their means sum to the total mean.
On the standard route, the network scope is the core network kernel loop, including
fused temporal pre/post shaders. RenoDX instead sums the complete `record_engine`
callbacks for all executed passes, including preparation, history work and composition
inside those callbacks. The menu labels this scope **RenoDX network callbacks**, not
pure network kernels. Other processing is total minus the route's network scope;
the RenoDX remainder therefore excludes work already counted inside its callbacks.
Adapter buffer/image copies, D3D12 copies, shared output effects, upscaling, frame
generation and whole-frame latency are outside both measurements. Equivalent FPS
is `1000 / GPU ms`, not game Present FPS.

Total and route-specific network median/p95 values, and the total/network/other
means, cover the same window of up to 120 completed samples; individual quantiles
are not additive. RenoDX reserves one
eight-slot query-ring entry per execution, with room for up to 30 network callback
pairs in that entry. A stage with no network callback still produces a valid
total/other sample and zero network time. Pending entries are not reused before
Vulkan completion, and polling does not duplicate samples or refresh their age.
Changing between standard and RenoDX routes clears the reported timing window.
Display and timing log options default off; summaries are limited to one per ten
seconds and include accepted sample counts plus current-core certified-harvest and
unavailable/invalid-read counters for field supply-cadence diagnosis. `NrTimingLog`
also gates runtime replay execution/harvest diagnostics through the poll export's
`timingLog` argument; toggles propagate on the normal poll even without recording.
It enables the runtime log sink alongside `LogToFile` and `NrFlickerDiag`, without
requiring either of those options. Execution/harvest markers distinguish submitted
and certified ordinals from admissions and report timing supply for retained
recipes; they do not establish temporal-history correctness. Query reads never
wait for the GPU. `mochizuki_nr.log` rotates to `mochizuki_nr.1.log` at 4 MiB.

Local verification on 2026-10-20 after removal of the product style-grade toggle:
the matching runtime and Release host compiled successfully (existing local shader
assets reused). `tests/mochizuki/run.cmd abi` passed, including rejection of both
undersized and oversized controls and the CPU query-ring tests. The existing
`stage-timing-gpu` smoke passed with 0, 1 and 30 RenoDX callbacks, completed-sample
coherence, unchanged polling age and route-switch reset; `split-retain` passed
retained recording/replay checks. These are correctness smokes, not a game FPS or
performance comparison. No game appearance comparison was performed.

Under **Apply NR edit → After the model**, **Model enlargement**
(`MochizukiEnlargeMode`) offers matched residual (0, default), edge-aware (1) or
classic (2). It applies only below 100% model scale. Edge-aware uses a 4×4
neighbourhood guided by the original pixel's brightness. Classic uses
**Classic scaler** (`MochizukiClassicScaler`): bilinear (0), Catmull-Rom (1, default),
Lanczos3 (2) or FSR1 (3). Edge-aware and classic cost more GPU time and need
scene-by-scene comparison. Below full model scale the runtime does not allocate
a second full-frame image for the enlargement pass; classic enlargement writes
`up_result` only.

**Reuse output buffer** (`MochizukiCompactTransfer=true`, default) is a product
policy on top of the official native store. Official `store_native` already writes
listed native formats back into the kept frame. When that path does not apply and
the frame is kept as RGBA32F, the same private image is reused as the answer.
Turning the option off forces a separate answer image and rebuilds the network.
U8 output keeps its rounding path. Model scale 100% is unchanged. The control is
owned by the INI/menu and does not follow `NR_*` environment variables.

**ACO mode** (`MochizukiAcoMode=true`, default) asks for AMD pipeline binaries.
The menu checkbox is **ACO fast mode (experimental)**, next to Processing order.
ACO is a standalone switch: group, page and NR reset buttons preserve both its
current-session value and any choice saved for the next launch.
The extension is chosen when the Vulkan device is created. Effective stays off
until Install publishes the network that will run; a candidate discarded by its
frame-buffer check leaves the installed network's effective state unchanged.
Changing the request while a candidate is still building leaves that candidate
alone. The effective state is the mode of the network Install publishes.
If the driver rejects the optional pipeline-binary device for a reason other than
device loss or out of memory, the same device is created without those extensions.
Turning ACO on after a device created without binaries waits for the next game start.
Each build thread selects its own bundle, so a later build can import ACO or return
to the driver's compiler while an earlier recording keeps the pipelines it already has.
The runtime configures ACO only when `dlssnr-amd/aco` is complete and every network
SPIR-V under `aco/shaders` (including temporal) has a record whose contents match
that SPIR-V. Windows `dlssnr-amd/shaders` supplies the native shell and runtime
passes. A missing bundle, a record mismatch, or an import that produces no
pipelines discards that graph and builds one complete native network.
Diagnostics show requested, effective and the reason. `NR_ACO_DIR` does not
select the bundle after `own_directory()`.

**Proton/vkd3d transport:** the host prepares supported Mochizuki-required Vulkan
extensions/features during the game-facing D3D12 creation hooks, independent of
the selected NR backend or its current configuration. `ScopedGameD3D12Device`
distinguishes game translation from private `ScopedCreatingD3DDevice` probes;
`ScopedSkipVulkanHooks` always excludes private devices. Raw Vulkan game creation
is not augmented by this path. Both instance and physical device must support
Vulkan 1.3. Failed capability merging or augmented creation falls back to the
original game request. This allows choosing Mochizuki later without trying to
enable features on an already-created game device.
Adoption uses the host's successful per-`VkDevice` creation record when available;
only devices without a host record fall back to vkd3d's extension/feature reports,
including both core-version and individual promoted feature structures. The core
does not require `shaderSubgroupExtendedTypes`.

On the adopted path, frame buffers borrow vkd3d's `VkBuffer` and allocation offset;
only their D3D12 resource owner is released. Producer submissions hold the game's
vkd3d queue after its D3D12 signal has been flushed; background build submissions
hold the build queue and stop on a failed lock. Separate imported timeline fences
order D3D12 capture, Vulkan work and D3D12 consumption. Buffers return to D3D12
`COMMON` at the copy boundaries and Vulkan copies include the borrowed offset.
Completion-based recording retirement and builder joining precede resource/device
release; the game's borrowed Vulkan device is never destroyed by the runtime.

The own-device route now imports and binds a shared buffer before building a
network. Under Wine the probe is 64 KiB; Windows retains the 16 MiB minimum needed
by its shared-handle workaround, also retained for real frame buffers. Probe and
import diagnostics identify the buffer, requested/actual byte sizes,
`D3D12_RESOURCE` handle type, buffer/handle/intersection `memoryTypeBits`,
`allocationSize` and failing API/result. Unavailable fields are zero when failure
occurs before their query. Fixed Proton external-import failures, including
winevulkan's host-memory error during the tiny probe, report external interop unsupported
and are remembered for the adapter for this process, rather than recompiling the
same network. Native Windows out-of-memory classification/backoff is unchanged;
D3D12 allocation OOM, Vulkan device-memory pressure and larger allocations on a
successfully probed path retain their memory-pressure classification.
`tests\mochizuki\run.cmd transport-policy` covers probe sizing and this platform-
specific failure boundary without a GPU and is included in `abi`. Proton first-frame,
rebuild/exit and Windows transport/OOM regression checks still require GPU runs.

**RADV/Mesa:** the actual Vulkan physical-device driver selects the Linux graph and
`dlssnr-amd/aco/shaders` independently of the saved ACO checkbox. This applies to
both the owned device and borrowed vkd3d device. RADV compiles those SPIR-V shaders
normally; no PAL records, shell aliases or pipeline-binary extension are required.
The selected model's kernels, temporal variants, accumulation marker and shader
constants must match the host. Missing or invalid assets and compile failures
report an NR build failure/bypass: RADV never retries the Windows network shaders.
INT4 remains unavailable and the default model path runs with an explanatory note.
Diagnostics distinguish `RADV driver-compiled Linux SPIR-V (no PAL import)` from
PAL imports; `aco_effective` means actual PAL import only. The stored INI request
is unchanged. `tests\mochizuki\run.cmd compile-policy` covers policy propagation
on a real builder thread, cleanup, assets and failure veto without a GPU; it also
runs in the `abi` tier.
On 2026-10-11, `tests/mochizuki/run.cmd aco` used AMD Radeon RX 9070 XT
(vendor 0x1002, device 0x7550, Vulkan 26.8.1 LLPC, driverVersion 0x0080018b).
The runtime log recorded 58/58 record matches and 29 imported pipelines. The
graph builder counted 24 kernels. The test printed `ACO_FRAME_OK` for one
256x256 frame, then `ACO_SWITCH_NATIVE_OK`, `ACO_SWITCH_ACO_OK` and
`ACO_CONTRACT_OK effective=1`. A forced pipeline-binary device rejection completed
one native frame (`ACO_DEVICE_FALLBACK_OK`). Refusing the second frame-buffer
admission kept the installed ACO state (`ACO_BUDGET_KEEP_OK effective=1`).
`tests/mochizuki/run.cmd aco-rapid` on the same adapter printed
`ACO_RAPID_REENABLE requested=1 effective=0` when a native candidate installed
after ACO had been requested again, then `ACO_RAPID_OK`.
Missing, alias-less and corrupt bundles each completed one native frame
(`ACO_FALLBACK_OK`). At that snapshot, package and uninstall ownership used the exact 794-path pin
`tools/install/mochizuki-owned.txt`. No game scene was compared.

`MochizukiWhitePointSource` (0 off, default; 1 game; 2 meter; 3 auto) picks the exposure sources.
With 0 the white point is the manual setting exactly as before. Modes 1 and 3 hand the runtime the game's
1x1 exposure texture (`MochizukiNrSetExposureTexture`): its texel is copied to a readback in the
game's own list and picked up one frame later behind the recording's completion fence, and the
level is `texel * DLSS_Exposure_Scale / DLSS_Pre_Exposure` - the expression the lmxxf codec
evaluates on the GPU. Without a texture, a pre-exposure other than 1 gives `1 / pre-exposure`
(`MochizukiNrSetExposure`). Modes 2 and 3 let the runtime meter a linear frame (mode 2 ignores the
game's exposure; mode 3 only when the game supplies neither) - every Monster Hunter Wilds frame so far, where the
DLSS-NR exposure scan adopts no candidate: one 16x16-thread dispatch over a 64x64 grid, a
highlight-clipped mean (arithmetic mean of min(luma, 16x log-average)), read one frame later
through the same fence, blended in the log domain with a 1/16 EV deadband and clamped to +-6 EV.
The meter is skipped for display-encoded frames, where the white point is not applied. `WhitePoint` then trims the
metered level instead of setting an absolute one: at the reference luminance (mean 0.2073) the
hand-tuned value keeps its meaning, elsewhere it scales with the scene. A frame with nothing
measurable leaves the last measurement in place, and after a second without one the manual
white point is back in charge with a line in the log; it resumes on its own once it measures
again. Daniel/lmxxf exposure controls do not
configure this backend, and preprocessing keeps its own histogram meter.

The meter classifies finite samples by exponent bits (`(asuint(l) & 0x7f800000u) != 0x7f800000u`)
for Wine d3dcompiler compatibility (E5005 on `isnan`), preserving Windows finite/zero/denormal acceptance.
`tests\mochizuki\run.cmd exposure-recording` covers NaN/±Inf parity with Windows intrinsics,
plus normal, mixed-invalid and all-invalid meter dispatches.

## Build and verification

`tools/build/build-mochizuki-runtime.cmd` builds the pinned core, adapter, Windows
shaders and the ACO payload. The ACO step runs `linux/build/build_network.py` with
the same pinned glslang into `dlssnr-amd/aco/shaders`, then copies
`windows/data/aco/records` and `shell-aliases.txt`. A failed shader generation fails
the build. The builder downloads pinned Vulkan headers and a hash-verified glslang
16.5.0 compiler into `exports/mochizuki-toolchain`; no machine-wide Vulkan SDK is needed.
`tools/build/mochizuki-manifest.py exports/mochizuki-runtime` verifies source and
artifact hashes. The packager runs this same check.

`tests/mochizuki/run.cmd abi` validates the current package contract without a GPU.
`tests/mochizuki/run.cmd gpu` requires an AMD GPU and the user's extracted model
under `exports/mochizuki-runtime/dlssnr-amd/`. It exercises output, replay, cross-queue
ordering, geometry retention, delayed collection, cancellation, execution-order
history/reset, RGB9E5/sRGB outputs and changing DRS subrects. Both are wired
into the corresponding `tests/run-all.cmd` tiers. Local and Actions use the same
builder and ABI test; a local package is still pending actual game acceptance.

`tests/mochizuki/run.cmd pass-switch normal|budget|oom|frame-budget` runs focused
GPU output comparisons for capacity reuse, expansion, budget/OOM recovery,
candidate-buffer refusal, retained recordings and incompatible geometry/format.
The normal case also compares two/three-pass output with independent fresh sessions.
The other modes use test-only failure injection; they do not change product defaults.

`tests/mochizuki/run.cmd hdr-shader` needs the built shaders and AMD GPU, but no model.
It executes the real input/transfer shaders and checks HDR RGB ratios, ordinary
brightness, alpha, valid dispatch bounds and full/reduced model resolution.
The `gpu` entrypoint also runs it.

`tests/mochizuki/run.cmd composition` compares separate/reused output buffers on
actual consecutive temporal frames, with one/three passes, preprocessing, both
enlargement modes and FP16/sRGB. It checks replay of retained recordings after
switching buffers. Warmup precedes history comparison because a cold driver's
first dispatch can exceed the runtime's 250 ms continuity limit. This test is
also included in `gpu`.

`tests/mochizuki/run.cmd profile <output.raw> <width> <height> <scale> <passes> <preprocess> <compact> <enlarge>`
uses deterministic R11G11B10 input, 16 warmup executions and 64 completed timing
samples, and saves the final pixels. It excludes history (no motion input), CPU
build time and adapter copies; use composition/GPU tests for temporal correctness.
This is a warm GPU microbenchmark, not a game FPS measurement.

For a focused 1080p R11G11B10 startup test, use
`tests/mochizuki/run.cmd startup <asset-root> [output.raw]`. The asset root contains
`dlssnr-amd/shaders` and the user model. A fresh asset directory alone does not make
the AMD driver cache cold; use a fresh test executable name for that comparison and
record the actual cache conditions. The test checks progress, then executes and
reads back the network output. Shader defines follow the official `rdna4.sh` recipe,
including `NR_TCHAIN_TILES`. The old `NR_EDGE_BODIES=0` override is not applied.

Uninstall removes the runtime and the package-owned files under `dlssnr-amd/shaders`
and `dlssnr-amd/aco` (known shader names, `shell-aliases.txt`, and `records/*.nrp`).
Files it does not recognize stay, including a shader added beside the package.
It does not follow a linked directory. It preserves `dlssnr.bin` and `dlssnr-amd/aco-cache`,
and still removes the driver `pipeline.cache` and prewarm manifest it already deleted.
Empty directories are removed after those files. To reclaim the model or the ACO cache,
remove those after closing the game.

## Session destruction and queue ownership

Destroy rejects sessions with live recordings or an executing token. An invalidated
recording can be collected only after its submitted D3D12 consumer tails complete.
Once all recordings are collected, destruction joins the builder and waits for the
private Vulkan work, without draining later unrelated work on the game queues.
Lost-device resources remain quarantined. This avoids making NR off/backend
switching depend on game work that may need the switching thread to continue.

`tests/mochizuki/run.cmd destroy-tail` reproduces this boundary by completing and
collecting NR work, then blocking both game queues behind an unrelated fence.
Destroy must return before the helper releases that fence. The full GPU suite
includes this check alongside live-recording and cancellation coverage.

## ACO decoder-tail dependency (2026-10-12)

The C64 persistent decoder → fused C32 boundary keeps its synchronization while
the other official tile-counter chains remain enabled. This adopts Overclockers'
`60f9202f` dependency fix; see [source attribution](../third_party/mochizuki/UPSTREAM.md#decoder-tail-synchronization-correction-2026-10-12).
The ACO machine-code setting still selects the compiler. There is no additional
product scheduling switch or default-policy change in the INI/menu/controls ABI.

The scheduling oracle is a separate test build, never a release runtime:

```powershell
tools/build/build-mochizuki-runtime.cmd --skip-shaders --test-full-barriers
tests/mochizuki/run.cmd aco-temporal --case 9
tests/mochizuki/run.cmd aco-inline
```

The reference build is restricted to `exports/mochizuki-test-reference`, records
its source/DLL proof, and produces no production packaging manifest. The fixture
compares actual frame bytes against production using the same graph and assets,
with controlled diagnostic environment and mandatory actual ACO import. Case
selectors cannot report success after selecting no cases. Synthetic frame streams
are removed after equality; logs/receipts remain under `exports/mochizuki-temporal`.
The default `aco-temporal` entry contains 45 GPU cases; use selected cases for
routine local checks. It is not the full CI suite. Full CI belongs to release
Actions, and this machine-specific GPU fixture needs a suitable device.

Local cases 1–26, 42 and 45 passed; the remainder was intentionally skipped after
the user narrowed local verification. Inline ACO passed 120 fresh recordings and
retained replay with 100% observed temporal history. No game/FPS result is claimed.

## ACO 本地缓存边界

导入后产生的驱动管线缓存位于 `%LOCALAPPDATA%/OptScaler-NR/Mochizuki/aco-cache/`，
不再写入游戏或模型目录；目录不可用时禁用磁盘缓存。旧 `aco-cache` 不迁移或导入。
包内固定的 ACO records 仍是构建输入，两者用途不同。
缓存里的 FNV64 仅检测损坏，不认证写入者；不要把第三方缓存当作可信模型资产导入。
相同用户权限的进程仍可修改本地缓存，修改后的二进制可能交给 Vulkan 驱动。
