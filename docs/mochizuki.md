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
completion-based resource release. Exact reused components, source pins, licenses and
local changes are in [the source record](../third_party/mochizuki/UPSTREAM.md).

## Installation

1. Install a current AMD display driver with Vulkan support. HIP is not used by this backend.
2. Extract the complete OptiScaler package. It contains `MochizukiNrRuntime.dll` and
   `dlssnr-amd/shaders/`; both must match the host in that package.
3. Supply your own `nvngx_dlssnr.dll` **310.8.0**. Its SHA-256 is
   `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e`.
   The DLL and extracted weights are **not included** in our package.
4. With Python 3.10 or newer installed, put that DLL beside `Setup.bat` and run
   `Mochizuki-Model.bat`. You can also drag its DLL/ZIP onto that batch file.
   Extraction only reads data and validates every model entry; it does not load the DLL.
   The result is `dlssnr-amd/dlssnr.bin`. A model extracted with the official tools is
   also usable at this path.
5. Run `Setup.bat`, select mochizuki alone or install all available backends. Choose
   the active backend. Ins exposes the same selection; with `NrConvenience=0`, save
   and restart after changing it. Unselected backends are not initialized.

The first network build can take minutes. Ins displays **building the network**;
frames pass through until it is ready. Changing resolution, model scale or pass
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
| Image | Model Intensity, Style (Standard/Natural/Cinematic), LocalTone, LocalStructure, SkinStructure (-1 follows Structure), AutomaticMask, DetailStrength, ColourStrength, MaxRatio highlight guard, WhitePoint for linear input |
| Quality | ModelScale (0.25–1), Passes (1–3). Both affect GPU cost and can rebuild the network |
| Temporal history | Temporal and HistoryStrength. Requires supported, unjittered motion vectors; unavailable vectors disable history |
| Preprocessing | Preprocess, PreprocessExposure (Off/Auto/Fixed), PreprocessBiasEv, PreprocessCurve (None/Neutral/Reinhard/Filmic/GT/ACES/AgX), PreprocessContrast and PreprocessSaturation. The transform changes the model input and is reversed from its answer |
| Advanced | ApplyModel, LinearInput (Auto/Linear/Encoded), MaxPasses (0 follows current passes), DynamicResolution (Exact/Auto bucket/Always bucket) |
| Pass 2/3 | Explicit override plus that pass's style, intensity, tone, structure, skin structure and mask. With override off, inherit pass 1 but use zero LocalTone |

Auto DRS buckets changing input subrects to reduce repeated network builds. The
network repeats the subrect edge into the unused bucket and resets history when the
valid extent changes. Unsupported blit formats fall back to exact extents.

Overall Intensity and Residual Stabilizer remain shared output effects. Intensity 0
or ApplyModel off still incurs network work; disable NR to avoid it. Group reset only
resets that group. Reset NR settings resets mochizuki and shared effects/timing,
preserving other backend controls, backend selection and hotkeys.

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
ordering, geometry retention, delayed collection and cancellation. Both are wired
into the corresponding `tests/run-all.cmd` tiers. Local and Actions use the same
builder and ABI test; a local package is still pending actual game acceptance.

Uninstall removes the runtime and known shipped shaders. It preserves the user's
model and local pipeline cache. To reclaim those, remove `dlssnr-amd` after closing
the game and confirming it is no longer needed.
