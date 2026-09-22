[中文](README.md) | **English**

# OptiScaler AMD pre-SR — 1.9.0

Connects **AMD Neural Rendering** (DLSS5 on AMD) into **OptiScaler**, enabling **pure DLSS / XeSS games** to run neural denoising on AMD GPUs; upscaling is handled by **FFX/FSR**.

This project is forked from **Matheus** and upstream community projects, maintaining and evolving the codebase with ongoing deep optimizations.

**Project Homepage: [github.com/TheAutomatic/dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)**

---

## Table of Contents
- [📢 1.9.0 Changelog](#-190-changelog)
- [1. Standing on the Shoulders of Giants](#1-standing-on-the-shoulders-of-giants)
- [2. Installation Guide](#2-installation-guide)
  - └─► [Optional: 3x+ Frame Generation](#optional-3x-frame-generation)
- [3. Dual-Backend Architecture & Benchmarks](#3-dual-backend-architecture--benchmarks)
- [4. In-Game Settings & Controls](#4-in-game-settings--controls)
- [5. Troubleshooting, Logs & Uninstallation](#5-troubleshooting-logs--uninstallation)
- [6. Attributions & Licenses](#6-attributions--licenses)

---

## 📢 1.9.0 Changelog

Version 1.9.0 is a **major architectural milestone upgrade**. We officially introduce the open-source [**`lmxxf` HIP Neural Rendering backend**](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting).

### 🚀 Key Highlights

1. **Brand-New `lmxxf` Neural Rendering Backend**
   - **Open-Source Compute Core**: In addition to maintaining full compatibility with the existing `danielblnc` backend, we integrate the open-source HIP neural rendering core.
   - **Same-Frame Queue Execution**: Seamlessly embeds input recording, HIP asynchronous inference, and output barrier synchronization within the game's primary command queue before upscaling (Pre-SR). Compared to upstream standalone runs, this theoretically enables true same-frame DLSS5 neural rendering in modern Unreal Engine titles and games like *Where Winds Meet* that have complex post-upscale GPU activity.
   - **DLSS / XeSS Input Interception**: Leverages OptiScaler's proxy architecture to intercept native DLSS/XeSS inputs (Color / Motion Vectors / Depth) and route them into the neural denoiser before passing them to FFX/FSR, bringing DLSS5 to games without native FSR support.
   - **Dual-Backend Compatibility & Coexistence**: Full backward compatibility. Users can choose either backend during installation or install both side-by-side. Switch between them anytime in `OptiScaler.ini` via `NrBackend=lmxxf` or `NrBackend=daniel`.
   - **Memory & Stability Hardening**: Optimizes `fast_prefix` mode to bypass the redundant 201MB noise buffer allocation, reducing host memory footprint and startup overhead. Enhances GPU LUID matching in Fake NVAPI to prevent cross-adapter crashes in multi-GPU or spoofed environments.
   - **⚠️ Resolution Recommendation**: The current `lmxxf` model architecture is optimized for **pre-upscale render resolution ≤ 1080p**:
     - **4K Output**: Recommended to use **FSR Performance** (1080p render) or Ultra Performance (720p render).
     - **1440p (2K) Output**: Recommended to use **FSR Quality / Balanced / Performance** (all render at or below 1080p).
     - **1080p Output**: Supports **Native 1080p** or any FSR scaling mode.

2. **Installer & Uninstaller Overhaul**
   - **Dual-Backend Detection**: The installer (`Setup.bat` / `tools/install-amd-presr.ps1`) automatically scans for `danielblnc` or `lmxxf` components. If both are present, an interactive menu allows selecting which one to install, or deploying both for easy switching.
   - **Safe Overwrite & Coexistence**: Cleanly updates existing files when choosing the same backend; allows side-by-side coexistence when switching.
   - **Uninstaller Safety**: `Uninstall_OptiScaler_NR.bat` features path traversal guards and preserves user model weights (`native-game-tiled-assets/` and `dlssnr_on_amd_weights.bin`) by default to avoid re-downloading large assets.

3. **Menu (Ins Menu) Polish & Real-Time Parameter Sliders**
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
| **[danielblnc / DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)** | Core AMD Neural Rendering runtime (0.3.0 / 0.3.1) | Calls standard runtime without core modifications; adds D3D12 state protection for 0.3.1 1-pixel draw wait |
| **[lmxxf / dlss5-on-amd-9070xt-porting](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting)** | Reversed 71-block network ported to open-source AMD HIP kernels | **Integrated into OptiScaler universal proxy framework to support more DLSS / XeSS games**; implemented same-frame queue execution; developed standardized C-ABI standalone runtime (`LmxxfNrRuntime`); added real-time detail/color tuning sliders |
| **[RenoDX / clshortfuse](https://github.com/clshortfuse/renodx)** | Open-source HDR / Color grading addon | Source of color composition algorithms in `dlssnr.hlsl` |

---

## 2. Installation Guide

<details>
<summary><strong>📦 Click to expand: Package Contents</strong></summary>

| File / Directory | Purpose |
|---|---|
| `OptiScaler.dll` | Main binary (renamed during installation to your chosen proxy name) |
| `OptiScaler.ini` | Core configuration file (contains `[DlssNr]` dual-backend options) |
| `OptiScaler\` | Core dependencies (FFX, XeSS, Agility SDK, plugins) |
| `Setup.bat` / `Setup.ps1` | Interactive installer (**Double-click `Setup.bat`**) |
| `Uninstall_OptiScaler_NR.bat` / `.ps1` | Safe uninstaller (automatically placed in game directory) |
| `tools\` | Internal build and verification utilities |
| `Licenses\` | Third-party open-source licenses |
| `README.md` / `README.en.md` | Documentation (Chinese / English) |

> **Note**: To comply with upstream licenses and distribution policies, this package **does not bundle** NVIDIA proprietary binaries, danielblnc installer tools, or unauthorized model weights.

</details>

---

### Step 1: Prepare Backend Files

Prepare either backend (or both for side-by-side coexistence):

#### Option A: [Prepare `lmxxf` Backend Files](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting)
- `LmxxfNrRuntime.dll` (from project release or [upstream lmxxf repository](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting));
- Module folder `lmxxf-modules\` (with 71 `.hsaco` files and `SHA256SUMS`);
- Shader folder `shaders\` (with `native_codec_encode.hlsl`);
- Weights folder `native-game-tiled-assets\`;
- Place these in the same extracted folder as `Setup.bat`.

#### Option B: [Prepare `danielblnc` Backend Files](https://github.com/danielblnc/DLSS-NR-on-AMD/releases)
- `dlssnr_on_amd_setup.exe` and `nvngx_dlssnr.dll` (from [danielblnc Releases](https://github.com/danielblnc/DLSS-NR-on-AMD/releases); installer generates weights automatically);
- Or pre-generated `version.dll` and `dlssnr_on_amd_weights.bin`;
- Place in the same extracted folder as `Setup.bat`.

---

### Step 2: Run the Installer (Recommended)

1. Extract this release to any temporary folder;
2. Place your backend files alongside `Setup.bat`;
3. **Ensure the game is not running**;
4. **Double-click `Setup.bat`**:
   - Select your game's executable directory (e.g. `...\Binaries\Win64\`);
   - If both backends are detected, choose which to install or install both;
   - Select your proxy DLL name (default `dxgi.dll`, recommended; `winmm.dll`, `d3d12.dll` also supported; **do not use `dinput8.dll`**);
   - The installer sets up proxies, clears conflicting duplicate files, and configures `OptiScaler.ini`.

---

### Step 3: Manual Installation

If you prefer manual file placement:
1. Rename `OptiScaler.dll` to your proxy name (e.g. `dxgi.dll`) and copy it to the game directory;
2. Copy `OptiScaler.ini` and the `OptiScaler\` folder into the game directory;
3. **Deploy Backend Files**:
   - **For `lmxxf`**: Copy `LmxxfNrRuntime.dll`, `lmxxf-modules\`, `shaders\`, and `native-game-tiled-assets\` into the game directory;
   - **For `danielblnc`**: Duplicate `version.dll` into `dlssnr_amd_pass1.dll`, `dlssnr_amd_pass2.dll`, `dlssnr_amd_pass3.dll`; copy `dlssnr_on_amd_weights.bin` into the game directory (**do not leave a file named `version.dll`** to prevent double injection);
4. In `OptiScaler.ini`, set `Enabled = true` under `[DlssNr]` and set `NrBackend = lmxxf` or `NrBackend = daniel`.

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

## 3. Dual-Backend Architecture & Benchmarks

This project supports two distinct AMD Neural Rendering backend technologies:

```
                          ┌──► [lmxxf Backend]   ──► Open-source HIP / Same-frame queue / Deep tuning
Game DLSS/XeSS Inputs ──► OptiScaler ──┤
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
| **Where Winds Meet (WWM)** | 19.05–19.25 ms, **Frequent skips** | 21.78–21.89 ms, **0 skipped** |

- **Recommendations**:
  - **3 slots** is the ideal sweet spot for most titles;
  - Heavy scenes like *Where Winds Meet* on max settings benefit from **≥ 3 slots**;
  - VRAM cost is minimal: each slot is one FP16 render-resolution texture (~29 MB at 1440p render; ~66 MB at native 4K).

### 2. `lmxxf` Backend: Open-Source HIP Compute & Same-Frame Queue Execution

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
  - `NR slots`: Parallel buffer count (2–5, default 3);
  - `Every-frame`: Enforces denoising on every frame;
  - `New wait mode`: 0.3.1 state freeze/restore wait mode toggle.

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
