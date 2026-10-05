[中文](README.md) | **English** | [Español](README.es.md)

Optional experimental **SR → NR** is available for Daniel, lmxxf and Mochizuki. Select
**Processing order** in Ins; **NR → SR** remains the default. Post-SR processing uses
the upscaled resolution and may cost more GPU time and VRAM. Save Settings persists
`[DlssNr] RunBeforeSR=false`; restart if the status requests recording hooks.
DX12 and the DX11/Vulkan-to-DX12 bridges are covered; native RR/Vulkan are unchanged.
See [usage, limits and acceptance checks](docs/post-sr-nr.md) (Chinese).

The Mochizuki backend supports Windows / RDNA4. See [installation, model requirements, controls and validation scope](docs/mochizuki.md). NVIDIA DLLs and model weights are not included. Local game tests are available; other games and scenarios still need validation.


# OptScaler(NR) 1.10.3

### What’s new in 1.10.3

- The Ins menu adds **Language**: English (default) and Simplified Chinese. **Save Settings** persists `[Menu] Language=en` or `zh-CN`. The Chinese font is embedded.
- Optional lmxxf **Temporal history** defaults to off and currently supports **one NR pass before SR**, with valid motion/depth guides. It may reduce flicker, at additional GPU cost and with possible ghosting. While enabled, **ViT adaptive reuse** and its four controls are disabled; turning history off restores the saved preference. See [implementation and limits](docs/architecture/lmxxf-native-history.md).
- Corrected post-SR NR dimensions and scratch reuse across retained recordings, fixed native 4K history dispatch bounds, and added an NR-only XeFG multiplier setting.
- Selectively ported OptiScaler NVAPI/DXGI/HUD capture stability fixes while preserving the three-backend lifecycle. Reviewed upstream `97e99b4c`; this is not a full merge. See [scope](docs/architecture/optiscaler-upstream.md).

**Special Thanks**: Thank you to all Bilibili users for your testing and feedback.

Connects **AMD Neural Rendering** (DLSS5 on AMD) into **OptiScaler**, enabling **pure DLSS / XeSS games** to run neural denoising on AMD GPUs; upscaling is handled by **FFX/FSR**.

This project is forked from **Matheus** and upstream community projects, maintaining and evolving the codebase with ongoing deep optimizations.

**Project Homepage: [github.com/TheAutomatic/dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)**

## Changelog

See the [Releases page](https://github.com/TheAutomatic/dlss-5-amd-project/releases) for detailed changelogs.

---

## Table of Contents
- [1. Standing on the Shoulders of Giants](#1-standing-on-the-shoulders-of-giants)
- [2. Installation Guide](#2-installation-guide)
  - ├─► [⚡ Quick Start Installation](#quick-start)
  - ├─► [Full Installation Details & Advanced Options](#full-installation-details--advanced-options)
  - └─► [Optional: 3x+ Frame Generation](#optional-3x-frame-generation)
- [3. Three Backends & Historical Benchmarks](#3-three-backends--historical-benchmarks)
- [4. In-Game Settings & Controls](#4-in-game-settings--controls)
- [5. Troubleshooting, Logs & Uninstallation](#5-troubleshooting-logs--uninstallation)
- [6. Attributions & Licenses](#6-attributions--licenses)

---

## 1. Standing on the Shoulders of Giants

This project is built upon the collective achievements of pioneering developers in the open-source graphics community:

| Upstream / Pioneer | Their Contribution | What This Project Added |
|---|---|---|
| **[OptiScaler](https://github.com/optiscaler/OptiScaler)** | Universal upscaling proxy framework (DLSS / FFX / XeSS) | Serves as the host and injection layer, providing hooking and GUI controls |
| **[Dagherbou / OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR)** → **[wilsjo2 / PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)** | First integrated DLSS-NR into OptiScaler; architected Pre-SR Multi-Pass pipeline | Inherits their OptiScaler codebase foundation and Pre-SR dispatch structure |
| **[Matheus / dlss-5-amd-project](https://github.com/MatheusGViana/dlss-5-amd-project)** | Bridged Pre-SR to AMD runtime: DLSS Input → AMD NR → FFX; integrated [RenoDX](https://github.com/clshortfuse/renodx) OkLab and two-branch tone mapping for specular highlight preservation | Pioneered **Multi-slot scheduling**, eliminating **8.7 ms/frame** of idle GPU stalls; adapted 0.3.1; restored D3D12 state freeze/restore; enhanced XBOX PC compatibility. **Bridge overhead measured at just 0.01–0.03 ms** |
| **[danielblnc / DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)** | Core AMD Neural Rendering runtime (0.3.0–0.6.0) | Calls standard runtime without core modifications; adds D3D12 state protection for 0.3.1+ 1-pixel draw wait |
| **[lmxxf / dlss5-on-amd-9070xt-porting](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting)** | Reversed 71-block network ported to open-source AMD HIP kernels | **Integrated into OptiScaler universal proxy framework to support more DLSS / XeSS games**; implemented same-frame queue execution; developed standardized C-ABI standalone runtime (`LmxxfNrRuntime`); added real-time detail/color tuning sliders |
| **[Mochizuki / DLSSNR-AMD](https://github.com/mochizuki0323/DLSSNR-AMD)** | Fully open-source Vulkan / RDNA4 neural rendering network core and SPIR-V shaders | Integrated into multi-backend architecture with overhauled lifecycle safety: eliminated stalls on unrelated game queues and memory leaks; integrated shader cancellation and driver 26.9.2+ fixes (provided by [@MatheusFerreiraS](https://github.com/MatheusFerreiraS)) |

RX 6000 (RDNA2) cards using danielblnc 0.6.0 require the AMD HIP 7.2 runtime.

---

## 2. Installation Guide

### <span id="quick-start"></span>⚡ Quick Start Installation (All Three Backends)

1. **Download & Extract**: Download the latest release `.zip` from the [Releases page](https://github.com/TheAutomatic/dlss-5-amd-project/releases) and extract it to any folder.
2. **Collect Required External Files** (copy them into the same extracted folder as `Setup.bat`, i.e., **1 folder + 1 DLL + 1 EXE**):
   - `nvngx_dlssnr.dll` (NVIDIA DLSS-NR native library, **version 310.8.0 is strictly required**);
   - [`native-game-tiled-assets` folder](https://gofile.io/d/RyvcrDxz) (lmxxf weights directory containing model files);
   - [`dlssnr_on_amd_setup.exe`](https://github.com/danielblnc/DLSS-NR-on-AMD/releases) (Setup/extraction executable for danielblnc).
   > 💡 **Tip**: If you plan to use the **Mochizuki** backend, please make sure **Python 3.10+** is installed from the Microsoft Store beforehand.
3. **Run Setup & Select Game Directory**:
   - Double-click `Setup.bat` and select the directory where the **actual game executable** is located (e.g. Unreal Engine games typically use `...\<GameName>\Binaries\Win64\`, not the platform launcher or shortcut folder);
   - Follow prompts to select your proxy DLL (typically `dxgi.dll`; try `winmm.dll` or other injection methods if unavailable) and desired backend. When updating, **an overwrite installation is recommended**.
4. **Note: Skip step 2 when upgrading**:
   - If you have previously installed this project and backend weights in a game, **you can skip step 2 when upgrading**; the installer will automatically detect existing weights and files from the game folder and copy them back into the setup directory for reuse.

---

### Full Installation Details & Advanced Options

<details>
<summary><strong>📖 Click to expand: Full Installation Details & Advanced Options (Contents, Backend Setup, Overwrite/Rollback & Manual Deployment)</strong></summary>

<details>
<summary><strong>📦 Click to expand: Package Contents</strong></summary>

| File / Directory | Purpose |
|---|---|
| `OptiScaler.dll` | Main binary (renamed during installation to your chosen proxy name) |
| `OptiScaler.ini` | Core configuration file (contains `[DlssNr]` three-backend options) |
| `OptiScaler\` | Core dependencies (FFX, XeSS, Agility SDK, plugins) |
| `LmxxfNrRuntime.dll` | lmxxf backend runtime (open-source HIP neural rendering) |
| `MochizukiNrRuntime.dll` / `dlssnr-amd/shaders/` / `Mochizuki-Model.bat` | Mochizuki runtime, shaders and extraction tool; model supplied separately |
| `lmxxf-modules\` | lmxxf dual-architecture compute modules (38 `.hsaco` each for `gfx1200` / `gfx1201`, with `SHA256SUMS` manifests) |
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

#### Step 1: Prepare Backend Files (Detailed)

Prepare either backend (or both for side-by-side coexistence):

##### Option A: [Prepare `lmxxf` Backend Files](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) or [Click Here](https://gofile.io/d/RyvcrDxz) to download weights
- `LmxxfNrRuntime.dll` from this complete project package (do not substitute the upstream ABI1 runtime);
- Module folder `lmxxf-modules\` (official dual-architecture layout containing `gfx1200` [9060 series, experimental] and `gfx1201` [9070 series, production] subfolders, with 38 `.hsaco` compute modules each, leaf manifests, and root `SHA256SUMS` for a total of 76 modules; automatically matched by the runtime based on D3D12/HIP GPU architecture; the installer validates the complete bundle and supports overwriting older flat installs);
- Shader folder `shaders\` (with `native_codec_encode.hlsl`);
- Weights folder `native-game-tiled-assets\` (can be downloaded [here](https://gofile.io/d/RyvcrDxz));
- Place these in the same extracted folder as `Setup.bat`.

To upgrade, run the new package's `Setup.bat` and select the game folder. When OptiScaler is detected, Setup recommends uninstalling first to avoid conflicts between the new files, module layout, and old settings. Choose **Y (Recommended)** to run the new uninstaller automatically and continue installing, or **N** to overwrite the existing installation. Uninstall resets OptiScaler settings but keeps weights and existing backups. Normal overwrite does not create backup copies of old DLLs, INI files, or complete module folders. The old module tree is kept temporarily for rollback and removed after a successful switch. User-added `.hsaco` files and other content that cannot enter the new module layout are saved separately under `backup-amd-presr-*/lmxxf-modules`; compatible user files remain in place.


##### Option B: [Prepare `danielblnc` Backend Files](https://github.com/danielblnc/DLSS-NR-on-AMD/releases)
- `dlssnr_on_amd_setup.exe` and `nvngx_dlssnr.dll` (from [danielblnc Releases](https://github.com/danielblnc/DLSS-NR-on-AMD/releases); installer generates weights automatically);
- Or pre-generated `version.dll` and `dlssnr_on_amd_weights.bin`;
- Place in the same extracted folder as `Setup.bat`.

---

##### Option C: Prepare Mochizuki (Windows / RDNA4)

The complete package includes `MochizukiNrRuntime.dll` and `dlssnr-amd/shaders/`. Supply your own `nvngx_dlssnr.dll` **310.8.0** (version 310.8.0 is strictly required, SHA256: `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e`) beside `Setup.bat`, install Python **3.10+**, and select Mochizuki in Setup to extract and validate the model. An existing model at `dlssnr-amd/dlssnr.bin` can be reused without extraction. See [Mochizuki installation](docs/mochizuki.md) for the source hash and standalone extraction tool.

Without the source DLL or model, Setup reports `MODEL SETUP REQUIRED`; the runtime alone is insufficient. Other DLL versions are not extracted automatically. Move an invalid model aside before rerunning Setup; it is not silently overwritten.

The first network build can take minutes. The bottom-right panel shows build progress while frames pass through unchanged. Resolution, model scale or pass capacity changes can trigger another build. Mochizuki has independent preprocessing, three styles, 1–3 passes (default 1), and temporal controls. Its `Mochizuki*` INI keys do not change other backends. For troubleshooting, check Ins dependency/build status first, then `OptiScaler.log`.

#### Step 2: Run the Installer (Detailed)

1. Extract this release to any temporary folder;
2. Place your backend files alongside `Setup.bat`;
3. **Ensure the game is not running**;
4. **Double-click `Setup.bat`**:
   - Select your game's executable directory (e.g. `...\Binaries\Win64\`);
   - If OptiScaler is already installed, choose **Y** to uninstall automatically before installing (recommended), or **N** to overwrite;
   - Select your proxy DLL name (default `dxgi.dll`, recommended; `winmm.dll`, `d3d12.dll` also supported; **do not use `dinput8.dll`**; please install corresponding crash-fix patches beforehand for Capcom RE Engine titles);
   - Review the detected backends and weights, install one or all available backends, then choose the active backend;
   - The installer sets up proxies, clears conflicting duplicate files, and configures `OptiScaler.ini`.

---

#### Step 3: Manual Installation

If you prefer manual file placement:
1. Rename `OptiScaler.dll` to your proxy name (e.g. `dxgi.dll`) and copy it to the game directory;
2. Copy `OptiScaler.ini` and the `OptiScaler\` folder into the game directory;
3. **Deploy Backend Files**:
   - **For `lmxxf`**: Copy `LmxxfNrRuntime.dll`, `lmxxf-modules\`, `shaders\`, and `native-game-tiled-assets\` into the game directory;
   - **For `danielblnc`**: Duplicate `version.dll` into `dlssnr_amd_pass1.dll`, `dlssnr_amd_pass2.dll`, `dlssnr_amd_pass3.dll`; copy `dlssnr_on_amd_weights.bin` into the game directory (**do not leave a file named `version.dll`** to prevent double injection);
   - **For `mochizuki`**: Copy `MochizukiNrRuntime.dll`, `dlssnr-amd/shaders/` and your `dlssnr-amd/dlssnr.bin`;
4. In `OptiScaler.ini`, set `Enabled = true` under `[DlssNr]` and set `NrBackend = lmxxf`, `NrBackend = daniel` or `NrBackend = mochizuki`.

</details>

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

**Hot switching** (menu **Allow backend hot switching**, or `OptiScaler.ini` `[DlssNr]`):

| Key | Default | Description |
|---|---|---|
| `NrConvenience` | `1` | `1`: Pre-arms submission hooks when lmxxf or mochizuki is installed, enabling live in-game hot switching across all three backends (daniel ↔ lmxxf ↔ mochizuki); `0`: loads only the active backend; changing backends requires restarting the game. Takes effect on next restart. |

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

> **Where are lmxxf & Mochizuki logs?**  
> Unlike `danielblnc` which writes to a separate `dlssnr_on_amd.log`, both `lmxxf` and `mochizuki` pipe all initialization, telemetry, shader builds, and error messages directly into **`OptiScaler.log`** (and `amd_bridge.log`). There is no need to search for separate log files.

#### `lmxxf` Backend Diagnostics
- **Status displays `waiting` or NR does not activate**:
  - Open `OptiScaler.log` and search for `Lmxxf`;
  - Verify that `LmxxfNrRuntime.dll` exists in the game directory;
  - Verify that `lmxxf-modules\` exists and contains `SHA256SUMS` along with all compute modules;
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

#### `mochizuki` Backend Diagnostics
- **Status indicates missing dependencies or NR does not activate**:
  - Open `OptiScaler.log` and search for `mochizuki`, or check the Ins menu status for missing dependencies;
  - Verify that `MochizukiNrRuntime.dll` exists in the game directory;
  - Verify that `dlssnr-amd/shaders/` and `dlssnr-amd/dlssnr.bin` are present and valid;
  - The first network build may take several minutes; progress is shown in the bottom-right overlay, with the original frame displayed until compilation finishes.
- **Environment Checks**:
  - Ensure an AMD driver with Vulkan support is installed; ensure Python 3.10+ from Microsoft Store was present during initial model extraction.

#### Microsoft Store / XBOX PC Notes
Due to Windows filesystem virtualization, certain Store/Game Pass titles create a **`_storage_`** folder next to the executable. Check this folder if logs or outputs do not appear in the primary game directory.

### 3. Issue Reporting Format
When reporting issues, please include:
1. Proxy DLL name used (e.g. `dxgi.dll`);
2. Selected backend (`lmxxf`, `daniel`, or `mochizuki`);
3. GPU model, OS version, and AMD driver version;
4. Game title, output resolution, and FSR mode;
5. Relevant `.log` files listed above.

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
- [**Mochizuki / DLSSNR-AMD**](https://github.com/mochizuki0323/DLSSNR-AMD) — **MIT License**: Fully open-source Vulkan / RDNA4 neural rendering network core and SPIR-V shaders;
- [**RenoDX / clshortfuse**](https://github.com/clshortfuse/renodx) — **MIT License**: Color compositing algorithms in `dlssnr.hlsl`;
- [**This Project (TheAutomatic / dlss-5-amd-project)**](https://github.com/TheAutomatic/dlss-5-amd-project) — **GPL-3.0 License**: Multi-slot scheduling, same-frame queue execution, C-ABI runtime creation and upstream PR, 0.3.1 state freeze/restore, dual-backend coexistence, and smart installer.

This distribution contains no NVIDIA proprietary binaries, danielblnc installer tools, or unauthorized model weights. Please respect all upstream licenses.
