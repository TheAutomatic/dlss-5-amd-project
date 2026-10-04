[中文](README.md) | **English** | [Español](README.es.md)

This development branch adds the mochizuki backend for Windows / RDNA4. See [installation, model requirements, controls and validation scope](docs/mochizuki.md). NVIDIA DLLs and model weights are not included. Game acceptance is pending.


# OptiScaler AMD pre-SR — 1.10.0

Connects **AMD Neural Rendering** (DLSS5 on AMD) into **OptiScaler**, enabling **pure DLSS / XeSS games** to run neural denoising on AMD GPUs; upscaling is handled by **FFX/FSR**.

This project is forked from **Matheus** and upstream community projects, maintaining and evolving the codebase with ongoing deep optimizations.

**Project Homepage: [github.com/TheAutomatic/dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)**

## Changelog

See the release 1.10.0 notes for details.

### 1.10.0 configuration and installation

lmxxf GPU modules use the upstream LLVM23 / RowOpts mixed compiler recipe: five modules per architecture use LLVM23.1.2 and the other 29 use COMGR. All 68 modules for both architectures are included; players do not need WSL or a compiler. Numerical and runtime regression checks passed on RX9070XT; game performance for this version still awaits acceptance, with no promised fixed speedup.

lmxxf now uses 0.40. Defaults: all 71 blocks (`DLSS5_SKIP_BLOCKS=none`), fast numerics on (`DLSS5_FAST_NUMERIC=true`), one pass (`DLSS5_MULTI_PASS=1`), and free resolution on (`DLSS5_NETWORK_FREE_RES=true`). Free resolution follows the actual input and falls back to network tiers outside its processing budget; 1440p/4K inputs can substantially increase GPU time and memory. Two or three passes strengthen the style at roughly N times the network work and disable adaptive ViT reuse. Keep `DLSS5_MULTI_PASS_SKIP_BLOCKS=none` unless deliberately trading appearance for speed. Changes rebuild the network next frame and may briefly stall.

The Ins graph navigates **Input → Model → Output**: resolution/exposure, then style/passes/ViT, then blending/stabilization/appearance. Game input and downstream upscaling provide context. Compatibility, kernels and diagnostics remain separate advanced groups. Save Settings writes the INI; each page Reset affects that page.

Setup reports detected weights before backend selection. lmxxf needs a folder containing `block0-ffn.f16/.f32`; Daniel needs extracted weights; Mochizuki needs `dlssnr-amd/dlssnr.bin`. With a supported `nvngx_dlssnr.dll` 310.8.0, Setup can extract the Mochizuki model using Python 3.10+. Missing source files receive setup instructions. Invalid models are preserved and must be moved aside before regeneration. Ins shows missing dependencies beside Enable NR.

Upgrade by overwriting the complete package through Setup. Keeping the INI preserves explicit old settings. Menu/INI remains authoritative; upstream default/custom/native files and external environment only fill keys the host did not set. The 1.9.0 section below is historical; its old resolution limit does not apply to this version.

### lmxxf config map (ini / Ins menu)

Cross-layer keys use the same `DLSS5_*` names as upstream. **Ins labels are never written to the ini.** Priority: menu/ini > `native-game-flags.txt` / environment > defaults. `DLSS5_STRENGTH` is the same pair as Detail/Colour strength; when the host sends those fields the menu wins — no second slider.

| Menu location | Key | Notes |
|---|---|---|
| Output | `TransferStrength` / `ColourStrength` | Detail / colour mix (Colour 0–1 keeps game colour) |
| Compatibility & Scheduling | `DLSS5_FIT_LARGE` | High resolution; fit large Color into the network (incl. >1080p) |
| Input | `DLSS5_NETWORK_HEIGHT` | **Network tier** auto (default) or 720 / 900 / 1080 |
| Input | `LmxxfAutoExposure`, paper white | Meter when no usable exposure texture |
| Advanced Kernels | `DLSS5_HIP_WAVE_OWNED`, … | Kernels / shared pool / byte stream |
| ViT / image reuse → Reuse tuning | `DLSS5_VIT_ADAPTIVE`, … | Static-frame ViT reuse (tunable) |
| Compatibility & Scheduling / Diagnostics | `DLSS5_HIP_PDL`, debug view, enhanced barriers, early wrap | Diagnostics |

### New controls in 1.10.0

These settings belong to `[DlssNr]` in `OptiScaler.ini`. Click **Save Settings** after changing them in Ins. See GitHub Releases for publication status.

| Menu | Key and default | Behaviour |
|---|---|---|
| lmxxf style | `DLSS5_STYLE=1` | 0 / 1 / 2; independent of Daniel's `Style`. Rebuilds the network on the next frame and may briefly stall |
| Compact 1080 network | `DLSS5_NETWORK_1080_ROWS=1152` | On selects 1088, only for the 1080 tier. May run faster but changes the image, especially the bottom edge; rebuilds the network |
| Additional colour formats | `DLSS5_FORMAT_FALLBACK=true` | Accepts additional GPU-supported colour formats through private FP16 output; save and restart the game |
| Overall Intensity | `NrOverallIntensity=1.0` | All three backends, 0–2. 0 shows the original, 1 keeps the full result, >1 amplifies the correction. NR still runs at 0; disable NR to save model work |
| Residual Stabilizer | `NrStabilizerEnabled=false` | All three backends; uses motion and depth to reduce correction flicker. May soften moving detail or cause trails; adds GPU work and memory |
| History blend / Residual threshold | `NrStabilizerAlpha=0.8` / `NrStabilizerThreshold=4.0` | Ranges 0–0.95 / 0–16. Either zero bypasses stabilization. Start with defaults; reduce if trailing appears |
| Measure NR performance | `NrTimingEnabled=false` | Asynchronous lmxxf NR GPU, Encode and Decode measurements; Ins also exposes CPU stages and dropped samples |
| Write timing summary to log | `NrTimingLog=false` | Requires measurement and file logging; at most one summary every five seconds |

**Page Up** opens the FPS overlay; **Page Down** changes detail. Just FPS omits NR timings; other styles show NR GPU, while detailed styles add encode/decode and output-effect times when samples exist. Codec/effect statistics cover the last 120 valid samples; NR GPU uses a five-sample median after warm-up and does not change PDL. `N/A` means unavailable and `stale` means outdated. NR GPU includes the network output copy. Do not add stages together as whole-frame latency. Measurement adds no GPU waits, but has some overhead that still needs game testing. Daniel's internal network timing is not integrated into this panel.

The stabilizer needs valid motion/depth; unsafe paths bypass it and show an Ins status. Enabling shared output effects for the first time in a pure-backend session may require saving and restarting. Overall Intensity blends the final output; it is separate from model `Intensity`, Detail and Colour.

ViT reuse retains this product's performance-oriented **16 / 1 / 50 / 1** defaults (period/global/local/image), compared with upstream **4 / 0.22 / 1 / 0.35**. Looser thresholds can retain stale detail or trails. `DLSS5_HIP_INPUT_POLL` and `DLSS5_IO_FUSE` are not integrated into the product path; adding ini keys does not enable them.

NR toggle defaults to **End**; ViT reuse has no F8 shortcut. Reset controls affect their own group; Reset NR settings preserves the other backend, backend selection, hot-switch preference and keybinds. Install the complete package with matching host and runtime.

### Ins window layout

Drag an edge to resize. Narrow windows use one column; content scrolls independently of the footer.
**Window** selects Free or a screen corner. Dragging the title bar releases the anchor; viewport changes keep the window within the game display area.
**Menu Scale** controls text/control scale separately. **… → Reset window layout** restores automatic size, centering and Free mode without resetting NR or UI scale.
**Save Settings** persists `[Menu] WindowWidth/WindowHeight` (logical pixels before scale; `auto` chooses the size) and `WindowAnchor` (0=Free, 1/2=top left/right, 3/4=bottom left/right). Free position is retained for this session only.
All three backends share this layout. The Page Up overlay keeps its independent position setting.


---

## Table of Contents
- [📢 1.9.0 Changelog](#-190-changelog)
- [1. Standing on the Shoulders of Giants](#1-standing-on-the-shoulders-of-giants)
- [2. Installation Guide](#2-installation-guide)
  - └─► [Optional: 3x+ Frame Generation](#optional-3x-frame-generation)
- [3. Three Backends & Historical Benchmarks](#3-three-backends--historical-benchmarks)
- [4. In-Game Settings & Controls](#4-in-game-settings--controls)
- [5. Troubleshooting, Logs & Uninstallation](#5-troubleshooting-logs--uninstallation)
- [6. Attributions & Licenses](#6-attributions--licenses)

---

## 📢 1.9.0 Changelog

Version 1.9.0 is a **major architectural milestone upgrade**. We officially introduce the open-source [**`lmxxf` HIP Neural Rendering backend**](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) and resolve critical multi-queue and command list split compatibility hurdles in modern Unreal Engine 5 titles.

### 🚀 Key Highlights

1. **Unreal Engine 5 (UE5) Compatibility Fixes for lmxxf (*Neverness to Everness*, *Palworld*, etc.) (1.9.0.3)**
   - **Render Queue Binding (*Neverness to Everness*)**: Correctly binds to the game's actual Direct rendering queue executing DLSS-NR commands, avoiding crashes and session invalidation caused by viewport render queue vs. Swapchain present queue mismatch (`QueueContract: targetQueue != sessionQueue`).
   - **Queue Safety Guard**: Adds COM identity checks during command list execution callbacks to skip HIP evaluation gracefully when unexpected command lists are dispatched.
   - **GPU Draining on Migration**: Flushes the GPU prior to queue migration to reduce VRAM leak risks from session recreation.
   - **Command List Split & Startup Fixes (*Palworld*)**: Hardens split eligibility checks, adjusts default log level to 2 (Information) to remove startup hashing delays, and introduces log rate-limiting.

2. **New `lmxxf` Neural Rendering Backend**
   - **Open-Source Compute Core**: In addition to maintaining compatibility with the existing `danielblnc` backend, integrates the open-source HIP neural rendering core.
   - **Same-Frame Queue Execution**: Embeds input recording, HIP asynchronous inference, and output barrier synchronization within the game's primary command queue before upscaling (Pre-SR).
   - **DLSS / XeSS Proxy Support for lmxxf**: Enables the `lmxxf` backend to intercept DLSS and XeSS inputs before upscaling (Pre-SR), allowing games without native FSR to use the lmxxf denoiser.
   - **Dual-Backend Support**: Seamlessly supports both `lmxxf` and `danielblnc` backends. Switch between them anytime in `OptiScaler.ini` via `NrBackend=lmxxf` or `NrBackend=daniel`.
   - **Memory & Stability Hardening**: Optimizes `fast_prefix` mode to bypass the redundant 201MB noise buffer allocation, reducing host memory footprint and startup overhead. Enhances GPU LUID matching in Fake NVAPI to prevent cross-adapter crashes in multi-GPU or spoofed environments.
   - **⚠️ Resolution Recommendation**: The current `lmxxf` model architecture is optimized for **pre-upscale render resolution ≤ 1080p**:
     - **4K Output**: Recommended to use **FSR Performance** (1080p render) or Ultra Performance (720p render).
     - **1440p (2K) Output**: Recommended to use **FSR Quality / Balanced / Performance** (all render at or below 1080p).
     - **1080p Output**: Supports **Native 1080p** or any FSR scaling mode.

3. **Installer Update**
   - Fixed installer interaction logic, supporting dual-backend selection and safe coexistence/overwrites.

4. **Menu (Ins Menu) Polish & Real-Time Parameter Sliders**
   - **Context-Aware Menu**: Automatically hides Daniel-specific options (e.g. slots, passes, new wait) when in `lmxxf` mode to eliminate confusion.
   - **Layout Fixes**: Resolves layout clumping between `Enable NR` and `AMD processing`, restoring clear vertical structure.
   - **Live Sliders**: Introduces continuous sliders for `Detail strength` and `Colour strength`, along with a real-time `Debug view` channel selector for live visual diagnostics.

---

## 1. Standing on the Shoulders of Giants

This project is built upon the collective achievements of pioneering developers in the open-source graphics community:

| Upstream / Pioneer | Their Contribution | What This Project Added |
|---|---|---|
| **[OptiScaler](https://github.com/optiscaler/OptiScaler)** | Universal upscaling proxy framework (DLSS / FFX / XeSS) | Serves as the host and injection layer, providing hooking and GUI controls |
| **[Dagherbou / OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR)** → **[wilsjo2 / PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)** | First integrated DLSS-NR into OptiScaler; architected Pre-SR Multi-Pass pipeline | Inherits their OptiScaler codebase foundation and Pre-SR dispatch structure |
| **[Matheus / dlss-5-amd-project](https://github.com/MatheusGViana/dlss-5-amd-project)** | Bridged Pre-SR to AMD runtime: DLSS Input → AMD NR → FFX | Pioneered **Multi-slot scheduling**, eliminating **8.7 ms/frame** of idle GPU stalls; adapted 0.3.1; restored D3D12 state freeze/restore; enhanced XBOX PC compatibility. **Bridge overhead measured at just 0.01–0.03 ms** |
| **[danielblnc / DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)** | Core AMD Neural Rendering runtime (0.3.0–0.6.0) | Calls standard runtime without core modifications; adds D3D12 state protection for 0.3.1+ 1-pixel draw wait |
| **[lmxxf / dlss5-on-amd-9070xt-porting](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting)** | Reversed 71-block network ported to open-source AMD HIP kernels | **Integrated into OptiScaler universal proxy framework to support more DLSS / XeSS games**; implemented same-frame queue execution; developed standardized C-ABI standalone runtime (`LmxxfNrRuntime`); added real-time detail/color tuning sliders |
| **[RenoDX / clshortfuse](https://github.com/clshortfuse/renodx)** | Open-source HDR / Color grading addon | Source of color composition algorithms in `dlssnr.hlsl` |

RX 6000 (RDNA2) cards using danielblnc 0.6.0 require the AMD HIP 7.2 runtime.

---

## 2. Installation Guide

<details>
<summary><strong>📦 Click to expand: Package Contents</strong></summary>

| File / Directory | Purpose |
|---|---|
| `OptiScaler.dll` | Main binary (renamed during installation to your chosen proxy name) |
| `OptiScaler.ini` | Core configuration file (contains `[DlssNr]` three-backend options) |
| `OptiScaler\` | Core dependencies (FFX, XeSS, Agility SDK, plugins) |
| `LmxxfNrRuntime.dll` | lmxxf backend runtime (open-source HIP neural rendering) |
| `MochizukiNrRuntime.dll` / `dlssnr-amd/shaders/` / `Mochizuki-Model.bat` | Mochizuki runtime, shaders and extraction tool; model supplied separately |
| `lmxxf-modules\` | lmxxf dual-architecture compute modules (34 `.hsaco` each for `gfx1200` / `gfx1201`, with `SHA256SUMS` manifests) |
| `shaders\` | lmxxf codec shaders (`native_codec_encode.hlsl` and others) |
| `experimental_lighting\` | Precompiled shaders for the experimental lighting pass (`GatherCS.cso` / `ResolveCS.cso`) |
| `Setup.bat` / `Setup.ps1` | Interactive installer (**Double-click `Setup.bat`**) |
| `Uninstall_OptiScaler_NR.bat` / `.ps1` | Safe uninstaller (automatically placed in game directory) |
| `lmxxf-module-package.ps1` | Module validation helper shared by the installer and uninstaller (keep it beside `Setup.ps1`) |
| `Licenses\` | Third-party open-source licenses |
| `README.md` / `README.en.md` / `README.es.md` | Documentation (Chinese / English / Spanish) |
| `VERSION` | Package version |
| `SHA256SUMS.txt` | SHA256 of every file in the package (check with `sha256sum -c SHA256SUMS.txt`) |

> **Note**: To comply with upstream licenses and distribution policies, this package **does not bundle** NVIDIA proprietary binaries, danielblnc installer tools, or unauthorized model weights.

</details>

---

### Step 1: Prepare Backend Files

Prepare either backend (or both for side-by-side coexistence):

#### Option A: [Prepare `lmxxf` Backend Files](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) or [Click Here](https://gofile.io/d/RyvcrDxz) to download weights
- `LmxxfNrRuntime.dll` from this complete project package (do not substitute the upstream ABI1 runtime);
- Module folder `lmxxf-modules\` (official dual-architecture layout containing `gfx1200` [9060 series, experimental] and `gfx1201` [9070 series, production] subfolders, with 34 `.hsaco` compute modules each, leaf manifests, and root `SHA256SUMS` for a total of 68 modules; automatically matched by the runtime based on D3D12/HIP GPU architecture; the installer validates the complete bundle and supports overwriting older flat installs);
- Shader folder `shaders\` (with `native_codec_encode.hlsl`);
- Weights folder `native-game-tiled-assets\` (can be downloaded [here](https://gofile.io/d/RyvcrDxz));
- Place these in the same extracted folder as `Setup.bat`.

To upgrade, run the new package's `Setup.bat` and select the game folder. When OptiScaler is detected, Setup recommends uninstalling first to avoid conflicts between the new files, module layout, and old settings. Choose **Y (Recommended)** to run the new uninstaller automatically and continue installing, or **N** to overwrite the existing installation. Uninstall resets OptiScaler settings but keeps weights and existing backups. An overwrite upgrade backs up the old module folder; extra `.hsaco` files are preserved under `backup-amd-presr-*/lmxxf-modules` at the path shown when installation finishes, while other compatible user files remain in place.


#### Option B: [Prepare `danielblnc` Backend Files](https://github.com/danielblnc/DLSS-NR-on-AMD/releases)
- `dlssnr_on_amd_setup.exe` and `nvngx_dlssnr.dll` (from [danielblnc Releases](https://github.com/danielblnc/DLSS-NR-on-AMD/releases); installer generates weights automatically);
- Or pre-generated `version.dll` and `dlssnr_on_amd_weights.bin`;
- Place in the same extracted folder as `Setup.bat`.

---

#### Option C: Prepare Mochizuki (Windows / RDNA4)

The complete package includes `MochizukiNrRuntime.dll` and `dlssnr-amd/shaders/`. Supply your own `nvngx_dlssnr.dll` **310.8.0** beside `Setup.bat`, install Python **3.10+**, and select Mochizuki in Setup to extract and validate the model. An existing model at `dlssnr-amd/dlssnr.bin` can be reused without extraction. See [Mochizuki installation](docs/mochizuki.md) for the source hash and standalone extraction tool.

Without the source DLL or model, Setup reports `MODEL SETUP REQUIRED`; the runtime alone is insufficient. Other DLL versions are not extracted automatically. Move an invalid model aside before rerunning Setup; it is not silently overwritten.

The first network build can take minutes. The bottom-right panel shows build progress while frames pass through unchanged. Resolution, model scale or pass capacity changes can trigger another build. Mochizuki has independent preprocessing, three styles, 1–3 passes (default 1), and temporal controls. Its `Mochizuki*` INI keys do not change other backends. For troubleshooting, check Ins dependency/build status first, then `OptiScaler.log`.

### Step 2: Run the Installer (Recommended)

1. Extract this release to any temporary folder;
2. Place your backend files alongside `Setup.bat`;
3. **Ensure the game is not running**;
4. **Double-click `Setup.bat`**:
   - Select your game's executable directory (e.g. `...\Binaries\Win64\`);
   - If OptiScaler is already installed, choose **Y** to uninstall automatically before installing (recommended), or **N** to overwrite;
   - Select your proxy DLL name (default `dxgi.dll`, recommended; `winmm.dll`, `d3d12.dll` also supported; **do not use `dinput8.dll`**);
   - Review the detected backends and weights, install one or all available backends, then choose the active backend;
   - The installer sets up proxies, clears conflicting duplicate files, and configures `OptiScaler.ini`.

---

### Step 3: Manual Installation

If you prefer manual file placement:
1. Rename `OptiScaler.dll` to your proxy name (e.g. `dxgi.dll`) and copy it to the game directory;
2. Copy `OptiScaler.ini` and the `OptiScaler\` folder into the game directory;
3. **Deploy Backend Files**:
   - **For `lmxxf`**: Copy `LmxxfNrRuntime.dll`, `lmxxf-modules\`, `shaders\`, and `native-game-tiled-assets\` into the game directory;
   - **For `danielblnc`**: Duplicate `version.dll` into `dlssnr_amd_pass1.dll`, `dlssnr_amd_pass2.dll`, `dlssnr_amd_pass3.dll`; copy `dlssnr_on_amd_weights.bin` into the game directory (**do not leave a file named `version.dll`** to prevent double injection);
   - **For `mochizuki`**: Copy `MochizukiNrRuntime.dll`, `dlssnr-amd/shaders/` and your `dlssnr-amd/dlssnr.bin`;
4. In `OptiScaler.ini`, set `Enabled = true` under `[DlssNr]` and set `NrBackend = lmxxf`, `NrBackend = daniel` or `NrBackend = mochizuki`.

---

### Optional: 3x+ Frame Generation

<details>
<summary><strong>👉 Click to expand: 3x+ Frame Generation (Arturs DLSS Enabler / Intel XeFG)</strong></summary>

These options are independent of DLSSNR. Required files are not bundled; obtain them separately.
**Note**: Game restarts are required when changing INI settings. Keep `[FrameGen] External=false`. **Do not enable both simultaneously**.

---

#### Option 1: Arturs (DLSS Enabler)
1. Obtain `dlss-enabler-headless.dll` from the official author:
   [artur-graniszewski/DLSS-Enabler Releases](https://github.com/artur-graniszewski/DLSS-Enabler/releases) or [Nexus Mods 757](https://www.nexusmods.com/site/mods/757)
2. Place `dlss-enabler-headless.dll` into the **`OptiScaler\`** subfolder in the game directory;
3. If the game has **native DLSSG**, configure in `OptiScaler.ini`:
   ```ini
   [FrameGen]
   External=false
   Enabled=true
   FGInput=nvngxfg
   FGOutput=auto
   FGNvngxReplacement=Arturs
   ```
   If the game only has upscaling without DLSSG, use `FGInput=upscaler` + `FGOutput=dlssg`;
4. Check `OptiScaler.log` for `Artur's initialized`.

---

#### Option 2: Intel XeFG (XeMFG DP4A Unlocker Multi-Frame Generation)
1. Place `XeFGUnlock.asi` and `XeFGUnlock.ini` into `OptiScaler\plugins\` (alongside `libxess_fg.dll`);
2. Configure `OptiScaler.ini` in the game root:
   ```ini
   [Plugins]
   LoadAsiPlugins=true

   [FrameGen]
   External=false
   Enabled=true
   FGInput=dlssg
   FGOutput=xefg

   [XeFG]
   InterpolationCount=1
   ```
   - `InterpolationCount`: `1` for 2x, `2` for 3x, etc.;
3. Test with 2x first before increasing multipliers. Press **Page Up** for FPS overlay and **Page Down** for detailed stats.

</details>

---

## 3. Three Backends & Historical Benchmarks

The three backends are lmxxf (HIP), Mochizuki (Vulkan), and Daniel. The benchmarks below are historical and do not measure 1.10.0 or Mochizuki:

```
                          ┌──► [lmxxf Backend]   ──► Open-source HIP / Same-frame queue / Deep tuning
Game DLSS/XeSS Inputs ──► OptiScaler ──┤
                          ├──► [Mochizuki] ──► Vulkan / D3D12 interop
                          └──► [daniel Backend] ──► Multi-slot scheduling / 0.3.1 compat / Universal
                                      │
                                      ▼
                            FFX / FSR Upscaling ──► Final Game Output
```

### 1. `danielblnc` Backend: Multi-Slot Scheduling (NR on Every Frame)

Denoising (DLSS5) is inserted directly into the frame rendering pipeline: a frame must finish denoising before passing to upscaling. In single-slot setups, each frame must wait for the preceding frame's denoising to complete, causing severe GPU idle stalls (**MsGPUWait ~8.7 ms/frame** in PresentMon). Under heavy load, frames are forced to skip denoising entirely, causing visible shimmering or blur.

This project introduced **Multi-Slot Scheduling**: allocating independent parallel buffers (slots) so each frame can proceed without waiting for the previous frame's GPU completion.

#### Benchmark (Onimusha-type workload, 4K FSR Ultra Performance = 720p render; locked 60 fps comparison)

| Configuration | Median Frame Time | Approx FPS | MsGPUWait (GPU Stall) | Per-Frame NR Status |
|---|---:|---:|---:|---|
| **Single-slot · per-frame NR (old baseline)** | 29.82 ms | **33.5** | **8.69 ms** | Blocked by previous frame |
| **Our Multi-slot default** | 22.45 ms | **44.5** (**+33%**) | **≈ 0 ms** | **NR on virtually every frame** |
| Upstream 0.3 native (baseline) | 22.35 ms | 44.8 | 0 ms | Native pipeline does not drop frames |

- **Key Takeaway**: Delivers a **+33%** throughput increase (33.5 → 44.5 FPS) by optimizing pipeline scheduling rather than compromising denoising quality; the neural network computation itself remains unchanged (~12–13 ms @ 720p).

#### Slot Count Recommendations (NR slots: 2–5, default 3)

| Test Scene (4K FSR Ultra Performance, 720p render) | 2 Slots | 3 Slots |
|---|---:|---:|
| **Onimusha** | 19.50 ms, **0 skipped** | 19.49 ms, **0 skipped** |
| **Where Winds Meet** | 19.05–19.25 ms, **Frequent skips** | 21.78–21.89 ms, **0 skipped** |

- **Recommendations**:
  - **3 slots** is the ideal sweet spot for most titles;
  - Heavy scenes like *Where Winds Meet* on max settings benefit from **≥ 3 slots**;
  - VRAM cost is minimal: each slot is one FP16 render-resolution texture (~29 MB at 1440p render; ~66 MB at native 4K).

### 2. `lmxxf` Backend: Open-Source HIP Compute & Same-Frame Queue Execution

- **Dual-Architecture Support & Auto-Selection**:
  - **AMD Radeon RX 9070 / 9070 XT (`gfx1201`)**: Standard verified production architecture with 24 tuned compute modules;
  - **AMD Radeon RX 9060 (`gfx1200`)**: Experimental support, verified through COMGR 3.0 compilation; real-device smoke test and PDL speedup pending hardware verification;
  - **Adaptive Architecture & Strict Verification**: Automatically selects matching arch subfolder based on D3D12 queue binding and HIP device LUID, with SHA-256 integrity verification and PDL twin symbol preflight;
- **Open Source & Hardware Optimized**: All 71 ViT neural network modules are implemented in HIP, tuned for modern RDNA architectures with LDS workgroup fences and C32 CU mode;
- **Same-Frame Queue Execution**: OptiScaler schedules input recording, HIP inference, and barrier synchronization on the main queue before command list close, eliminating external cross-process synchronization delays;
- **Dynamic Parameter Controls**: Real-time continuous sliders for detail/brightness enhancement and color calibration directly in the Ins menu.

---

## 4. In-Game Settings & Controls

1. Launch the game and enter 3D rendering.
2. Press **Insert (Ins)** to open the OptiScaler overlay menu.
3. Locate the **DLSS Neural Rendering** section and check **Enable NR**.
   - The status line indicates the active runtime:
     - `AMD NR runtime: lmxxf` for lmxxf backend;
     - `AMD NR runtime: 0.3.x` for danielblnc backend.
4. Active pipeline: **DLSS Inputs → Neural Denoising → FFX/FSR Upscaling**.

### Backend Controls
- **`lmxxf` Specific**:
  - `Detail strength`: Continuous slider for detail and brightness enhancement (default 1.0);
  - `Colour strength`: Continuous slider for color saturation and balance (default 1.0);
  - `Debug view`: Live visualization of inputs, network output, and difference buffers.
- **`danielblnc` Specific**:
  - `NR slots`, `Every-frame`, `New wait mode`, `Inline same-frame wait`;
  - `Quality`: Reference (default, NVIDIA-exact) / Fast;
  - **Display**: `Tone curve` / `Tone lift`;
  - **Queue (experimental)**: `HIP high-priority queue`;
  - **Compatibility & Scheduling / Diagnostics**: extra `dlssnr_on_amd.ini` keys.

**Priority:** Ins session > `OptiScaler.ini` `[DlssNr]` (after Save) > `dlssnr_on_amd.ini` / env > defaults.  
Ins labels are not written to ini; **Save Settings** persists menu values to both inis.  
Unlisted daniel keys (`OverlayKey`, `PollSpacing`, ...) stay in `dlssnr_on_amd.ini`; `OverlayKey` binds only daniel's own overlay.  
Advanced process env (no Ins toggle): `DLSSNR_NO_REG`, `DLSSNR_CHAIN`, `DLSSNR_NOBLEND`, `DLSSNR_NO_REPACK`, `DLSSNR_WBLOG`.

---

## 5. Troubleshooting, Logs & Uninstallation

### 1. Uninstallation
1. Open the **game directory**;
2. Run **`Uninstall_OptiScaler_NR.bat`**;
3. Review the proposed deletion list, choose whether to keep backup folders, and confirm with `Y`;
4. **Preserved Weights**: The script is designed to preserve user weight files (`native-game-tiled-assets/` and `dlssnr_on_amd_weights.bin`) and `nvngx_dlssnr.dll` by default, avoiding repeated multi-gigabyte downloads.

### 2. Log Locations & Diagnostics

Inspect the following logs in the game directory (or `_storage_` for Microsoft Store / XBOX PC games):
- `OptiScaler.log`: Main initialization, hooking, and backend creation log;
- `amd_bridge.log`: AMD bridge layer log;
- `amd_presr.log`: Pre-SR dispatch log;
- `dlssnr_on_amd.log`: danielblnc runtime log.

> **Where are lmxxf logs?**  
> Unlike `danielblnc` which writes to a separate `dlssnr_on_amd.log`, the `lmxxf` backend and its C-ABI runtime pipe all initialization, telemetry, and error messages directly into **`OptiScaler.log`** (and `amd_bridge.log`). There is no need to search for separate log files.

#### `lmxxf` Backend Diagnostics
- **Status displays `waiting` or NR does not activate**:
  - Open `OptiScaler.log` and search for `Lmxxf`;
  - Verify that `LmxxfNrRuntime.dll` exists in the game directory;
  - Verify that `lmxxf-modules\` exists and contains `SHA256SUMS` along with all 71 `.hsaco` compute modules;
  - Verify that `shaders\` exists and contains `native_codec_encode.hlsl`.
- **Missing weights error**:
  - Ensure the `native-game-tiled-assets\` directory is present in the game directory.
- **Resolution exceeding limits**:
  - Current lmxxf model slices support render resolutions **≤ 1080p**. If playing at 4K, select FSR Performance (1080p render) or Ultra Performance (720p render); 4K Quality (1440p render) exceeds the model slice limits.

#### `danielblnc` Backend Diagnostics
- **Status does not show `AMD NR runtime: 0.3.x`**:
  - Ensure `dlssnr_amd_pass1.dll` (and pass2/pass3) and `dlssnr_on_amd_weights.bin` exist;
  - Ensure there is no conflicting `version.dll` left in the game directory;
  - Check `dlssnr_on_amd.log` for runtime initialization errors.

#### Microsoft Store / XBOX PC Notes
Due to Windows filesystem virtualization, certain Store/Game Pass titles create a **`_storage_`** folder next to the executable. Check this folder if logs or outputs do not appear in the primary game directory.

### 3. Issue Reporting Format
When reporting issues, please include:
1. Proxy DLL name used (e.g. `dxgi.dll`);
2. Selected backend (`lmxxf` or `daniel`);
3. GPU model, OS version, and AMD driver version;
4. Game title, output resolution, and FSR mode;
5. Relevant `.log` files listed above.

### 4. Known Issues
- **UE5 (Palworld, Neverness to Everness, and others)**: Older builds rejected every query and left game command lists created before the first swapchain unwrapped, causing the `lmxxf` backend to return original color. The current source permits completed queries and wraps lists created by the game executable earlier. D3D12 tests pass; neural rendering and image stability still need validation in the games.

---

## 6. Attributions & Licenses

Codebase heritage (top to bottom):  
[OptiScaler](https://github.com/optiscaler/OptiScaler) → [Dagherbou](https://github.com/Dagherbou/OptiScaler_DLSSNR) → [wilsjo2](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) → [Matheus](https://github.com/MatheusGViana/dlss-5-amd-project) → [**This Repository (TheAutomatic / dlss-5-amd-project)**](https://github.com/TheAutomatic/dlss-5-amd-project).

- [**OptiScaler**](https://github.com/optiscaler/OptiScaler) — **GPL-3.0 License**: Universal upscaling proxy framework;
- [**Dagherbou / OptiScaler_DLSSNR**](https://github.com/Dagherbou/OptiScaler_DLSSNR) — **GPL-3.0 License**: Initial DLSS-NR integration;
- [**wilsjo2 / OptiScaler-DLSSNR-PreSR-Multipass**](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) — **GPL-3.0 License**: Pre-SR and Multi-Pass architecture;
- [**Matheus / dlss-5-amd-project**](https://github.com/MatheusGViana/dlss-5-amd-project) — **GPL-3.0 License**: AMD Pre-SR bridge;
- [**danielblnc / DLSS-NR-on-AMD**](https://github.com/danielblnc/DLSS-NR-on-AMD) — **Custom Non-Commercial / All Rights Reserved**: Author retains all rights; redistribution prohibited; integrated via external detection;
- [**lmxxf / dlss5-on-amd-9070xt-porting**](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) — **MIT License**: Open-source HIP neural rendering core and 71-block network recovery;
- [**RenoDX / clshortfuse**](https://github.com/clshortfuse/renodx) — **MIT License**: Color compositing algorithms in `dlssnr.hlsl`;
- [**This Project (TheAutomatic / dlss-5-amd-project)**](https://github.com/TheAutomatic/dlss-5-amd-project) — **GPL-3.0 License**: Multi-slot scheduling, same-frame queue execution, C-ABI runtime creation and upstream PR, 0.3.1 state freeze/restore, dual-backend coexistence, and smart installer.

This distribution contains no NVIDIA proprietary binaries, danielblnc installer tools, or unauthorized model weights. Please respect all upstream licenses.

## Known issues (1.9.2-alpha)

- **Large inputs with `lmxxf`:** `[DlssNr] DLSS5_FIT_LARGE` is enabled by default and fits larger Color inputs to the network dimensions. Palworld's former multi-second stalls came from a bug that rebuilt the chain every frame; it is fixed. As of 2026-09-28, the maintainer has found no remaining FitLarge problem after that fix. The old measurements are no longer grounds for recommending a lower resolution or disabling FitLarge. When explicitly set to `false`, input width is limited to 2560, height to 1080, and pixel count to 1920×1080 (for example 2024×848); 2560×1080 is rejected.
- **Cyberpunk 2077 colour:** The old network-hue mixing path that could turn neon brown has been changed. Colour strength 0–1 now preserves the game's original chroma; values above 1 blend toward network colour. The old recommendation to force it to 0 no longer applies. This rule applies to all games.
- **PDL:** Chained launch is on by default. If the driver has no `hipExtModuleLaunchKernel`, set `DLSS5_HIP_PDL=false` and restart.
