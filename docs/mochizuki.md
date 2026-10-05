# Mochizuki backend (local integration)

This backend runs mochizuki0323's Vulkan network on the game's AMD adapter while the
game stays on native D3D12. This integration targets Windows and RDNA4. Other GPU
architectures and individual games are not validated by this integration.
Recordings with unsupported state (including enhanced barriers or an active render
pass) pass through without NR; this backend does not inherit lmxxf's experimental
enhanced-barrier override.

The network core and shaders come from the
[official repository](https://github.com/mochizuki0323/DLSSNR-AMD).
The runtime adapts native D3D12/Vulkan interop and pipeline prewarming code from
[MatheusFerreiraS/neural-amd-opti](https://github.com/MatheusFerreiraS/neural-amd-opti).
This product implements backend selection, host recording ownership, configuration,
menus and packaging, and adapts the runtime for retained recordings, replay and
completion-based resource release. Its build-progress display and execution-history
handling are described below. Exact reused components, source pins, licenses and
local changes are in [the source record](../third_party/mochizuki/UPSTREAM.md).

## Installation

1. Install a current AMD display driver with Vulkan support. HIP is not used by this backend.
2. Extract the complete OptiScaler package. It contains `MochizukiNrRuntime.dll` and
   `dlssnr-amd/shaders/`; both must match the host in that package.
3. Supply your own `nvngx_dlssnr.dll` **310.8.0**. Its SHA-256 is
   `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e`.
   The DLL and extracted weights are **not included** in our package.
4. With Python 3.10 or newer installed, put that DLL beside `Setup.bat` and run
   `Setup.bat` to select Mochizuki and extract the detected model.
   `Mochizuki-Model.bat` remains available separately; you can drag a DLL/ZIP onto it.
   Extraction only reads data and validates every model entry; it does not load the DLL.
   The result is `dlssnr-amd/dlssnr.bin`. A model extracted with the official tools is
   also usable at this path.
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

## Controls

All keys below are in `[DlssNr]`, prefixed `Mochizuki`, independent of Daniel and
lmxxf settings. The complete defaults are in the shipped `OptiScaler.ini`.

| Group | Controls and behavior |
|---|---|
| Pass 1 | Model Intensity, Style (Standard/Natural/Cinematic), LocalTone, LocalStructure, SkinStructure (-1 follows Structure), AutomaticMask |
| Output adjustment | ApplyModel, DetailStrength, ColourStrength, MaxRatio highlight guard |
| Quality | ModelScale (0.25–1), Passes (1–3). Both affect GPU cost and can rebuild the network; edits commit when editing finishes |
| Temporal history | Temporal and HistoryStrength. Requires supported, unjittered motion vectors; unavailable vectors disable history |
| Preprocessing | Preprocess, PreprocessExposure (Off/Auto/Fixed), PreprocessBiasEv, PreprocessCurve (None/Neutral/Reinhard/Filmic/GT/ACES/AgX), PreprocessContrast and PreprocessSaturation. The transform changes the model input and is reversed from its answer |
| Advanced | WhitePoint for linear input, LinearInput (Auto/Linear/Encoded), MaxPasses (0 grows capacity as needed and retains it on pass reduction), DynamicResolution (Exact/Auto bucket/Always bucket) |
| Pass 2/3 | Explicit override plus that pass's style, intensity, tone, structure, skin structure and mask. With override off, inherit pass 1 but use zero LocalTone |

The NR menu uses a clickable pre-SR flow chart. **Prepare NR input** contains model
resolution, preprocessing and advanced input encoding/white point/DRS. **NR model**
contains pass count, Pass 1 and enabled Pass 2/3, temporal history and prebuild
capacity. **Apply NR edit** contains output adjustment and the shared intensity and
stabilizer. The model page opens first; page buttons remain available when the chart
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

Overall Intensity and Residual Stabilizer remain shared output effects. Intensity 0
or ApplyModel off still incurs network work; disable NR to avoid it. Group reset only
resets that group. Reset NR settings resets mochizuki and shared effects/timing,
preserving other backend controls, backend selection and hotkeys. Reset this page
resets only that page's mochizuki fields; on the model page this includes hidden
later-pass overrides. Shared effects have their own reset.

Ins and Page Up/Down can show completed Vulkan network GPU measurements. They do
not include the D3D12 copies, upscaler, frame generation or whole-frame latency.
The status shows median/p95; detailed telemetry shows last/mean/max. Display and
timing log options default off; summaries are limited to one per ten seconds.
`mochizuki_nr.log` respects file logging and stops appending at 4 MiB.

The bridge does not sample the game's exposure texture on the CPU. WhitePoint is
the manual linear-input setting; optional preprocessing has its own histogram
meter. Daniel/lmxxf exposure controls do not configure this backend.

## Build and verification

`tools/build/build-mochizuki-runtime.cmd` builds the pinned core, adapter and shaders.
It downloads pinned Vulkan headers and a hash-verified glslang compiler into
`exports/mochizuki-toolchain`; no machine-wide Vulkan SDK is needed.
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

For a focused 1080p R11G11B10 startup test, use
`tests/mochizuki/run.cmd startup <asset-root> [output.raw]`. The asset root contains
`dlssnr-amd/shaders` and the user model. A fresh asset directory alone does not make
the AMD driver cache cold; use a fresh test executable name for that comparison and
record the actual cache conditions. The test checks progress, then executes and
reads back the network output. The eight edge-specialized shader bodies are disabled
to reduce first-build cost, following the patch documented in the upstream notice.

Uninstall removes the runtime and known shipped shaders. It preserves the user's
model and local pipeline cache. To reclaim those, remove `dlssnr-amd` after closing
the game and confirming it is no longer needed.

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
