[中文](README.md) | **English**

# OptiScaler AMD pre-SR — 1.9.0

Hooking **AMD Neural Rendering** (DLSS5 on AMD) into **OptiScaler**, enabling **pure DLSS / XeSS games** to run neural denoising on AMD GPUs; super-resolution is still handled by **FFX/FSR**.

This project is forked from **Matheus** and upstream repositories, maintained and evolved from their foundation.

**Project Home: [github.com/TheAutomatic/dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)**

---

## Table of Contents
1. [Dual-Backend Architecture Overview](#1-dual-backend-architecture-overview)
2. [Installation & Setup](#2-installation--setup)
3. [In-Game Ins Menu Controls](#3-in-game-ins-menu-controls)
4. [FAQ, Troubleshooting & Uninstallation](#4-faq-troubleshooting--uninstallation)
5. [Attribution & Licenses](#5-attribution--licenses)

---

## 1. Dual-Backend Architecture Overview

This project serves as the **scheduling and bridging layer** for AMD neural rendering. The bridge itself introduces negligible execution overhead (measured at **0.01–0.03 ms**), with virtually no performance penalty.
In version 1.9.0, we provide full support for two prominent AMD neural rendering backends:

> **Pipeline**: Game DLSS/XeSS inputs → This project's Pre-SR scheduling and bridge → Neural denoising core (`lmxxf` / `danielblnc`) → FFX/FSR super-resolution output.

### Backend Comparison

Both backends are built upon the ViT (Vision Transformer) architecture and support AMD RX 7000 / 9000 series GPUs (RDNA3 / RDNA4):

| Feature | `lmxxf` Backend | `danielblnc` Backend |
|---|---|---|
| **Core Source** | [lmxxf / dlss5-on-amd-9070xt-porting](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) (Open-source HIP compute core) | [danielblnc / DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD) (0.3.1 / 0.3.0 runtime) |
| **GPU Support** | AMD RX 7000 / 9000 series (RDNA3 / RDNA4) | AMD RX 7000 / 9000 series (RDNA3 / RDNA4) |
| **Model Architecture** | ViT neural denoising network | ViT neural denoising network |
| **Runtime Files** | `LmxxfNrRuntime.dll` + `lmxxf-modules/` + `shaders/` | `dlssnr_amd_pass1.dll` ~ `pass3.dll` |
| **Model Weights** | `native-game-tiled-assets/` (tiled weight directory) | `dlssnr_on_amd_weights.bin` (monolithic binary) |
| **Scheduling** | Fine-grained 3-stage same-frame micro-scheduling (recording → HIP inference → barrier sync) | Multi-slot pipelined scheduling (default 3 slots) with idle-wait elimination |
| **Dynamic Tuning** | Real-time in-game sliders for `Detail strength`, `Colour strength`, and `Debug view` | Pass Presets / Styles / WhitePoint / AmdSlots |
| **Resolution Guidance**| Recommended render resolution before upscaling **≤ 1080p** (e.g., 4K Performance, 2K Quality) | Standard render resolutions |

### Backend Coexistence Mechanism

The two backends use completely distinct filenames with **zero filename collisions**:
- `lmxxf` files: `LmxxfNrRuntime.dll`, `lmxxf-modules\`, `shaders\`, `native-game-tiled-assets\`
- `danielblnc` files: `dlssnr_amd_pass1.dll`, `dlssnr_amd_pass2.dll`, `dlssnr_amd_pass3.dll`, `dlssnr_on_amd_weights.bin`

Both backends can **coexist in the same game directory simultaneously**. The active backend is controlled by `OptiScaler.ini`:
```ini
[DlssNr]
Enabled = true
RunBeforeSR = true
NrBackend = lmxxf   ; Options: lmxxf or daniel
```
To switch backends at any time, simply edit `NrBackend` in `OptiScaler.ini` or re-run `Setup.bat`.

---

## 2. Installation & Setup

### Package Contents

| File / Folder | Purpose |
|---|---|
| `OptiScaler.dll` | Core module (renamed to your chosen proxy during installation, e.g., `dxgi.dll`) |
| `OptiScaler.ini` | Configuration template containing `[DlssNr]` and backend settings |
| `OptiScaler\` | Dependency directory for FFX, XeSS, Agility, etc. |
| `Setup.bat` / `Setup.ps1` | Interactive installer script (**double-click `Setup.bat`**) |
| `Uninstall_OptiScaler_NR.bat` / `.ps1` | Dedicated uninstaller script; copied into the game directory by Setup |
| `LmxxfNrRuntime.dll` | lmxxf neural rendering runtime core library |
| `third_party\lmxxf\modules\` or `lmxxf-modules\` | lmxxf compute kernels (.hsaco) |
| `third_party\lmxxf\shaders\` or `shaders\` | lmxxf image processing shaders |
| `native-game-tiled-assets\` | (Included in full pack) lmxxf model weights directory |
| `Licenses\` | Third-party upstream open-source licenses |

---

### Step 1: Prepare Files

Prepare the required files based on your desired backend and place them in the same directory as `Setup.bat`:

#### Option A: Using `lmxxf` Backend
- If using the **Full Pack** release with pre-packaged weights, all lmxxf components are included.
- If using a lightweight package, ensure the `native-game-tiled-assets` weight folder is placed in the package directory or your game directory.

#### Option B: Using `danielblnc` Backend
Supply the original author's files (not bundled due to license constraints):
- Place `dlssnr_on_amd_setup.exe` ([Original Project Releases](https://github.com/danielblnc/DLSS-NR-on-AMD/releases) 0.3.1 or 0.3.0) and `nvngx_dlssnr.dll` next to `Setup.bat`;
- Or provide pre-generated `version.dll` and `dlssnr_on_amd_weights.bin`.

#### Option C: Dual-Backend Coexistence
Provide both sets of files. The installer will detect both and offer an option to install both simultaneously.

---

### Step 2: Run the Installer (Recommended)

1. **Ensure the game is closed.**
2. **Double-click `Setup.bat`**.
3. In the folder browser dialog, select the **directory containing the game executable (.exe)**.
4. Choose the proxy injection DLL (default `dxgi.dll`; also supports `winmm.dll`, `d3d12.dll`, etc.; **`dinput8.dll` is not supported**).
5. **Backend Detection & Mode Selection**:
   - If files for both backends are detected, you will be prompted:
     1. Install `lmxxf` backend
     2. Install `danielblnc` backend
     3. **Install both backends (coexisting, switchable via ini)**
   - If only one backend's files are found, the installer automatically selects that backend.
6. If installing `danielblnc` and weights are missing, the installer will automatically launch the original setup tool to generate them.
7. The installer configures `OptiScaler.ini` and completes setup.

> **Clean Reinstall / Overwrite**: Running `Setup.bat` again cleanly updates existing installations without requiring manual uninstallation. Selecting the same backend refreshes files; selecting a different backend or both preserves coexistence.

---

### Step 3: Manual Installation (Advanced Users)

If you prefer manual deployment:
1. Copy `OptiScaler.dll` into the game directory and rename it to your proxy choice (e.g., `dxgi.dll`).
2. Copy `OptiScaler.ini` and the `OptiScaler\` folder into the game directory.
3. **Deploy Backend Files**:
   - For `lmxxf`: Copy `LmxxfNrRuntime.dll`, `lmxxf-modules\`, `shaders\`, and `native-game-tiled-assets\` into the game directory.
   - For `danielblnc`: Make three copies of the original `version.dll` named `dlssnr_amd_pass1.dll`, `dlssnr_amd_pass2.dll`, `dlssnr_amd_pass3.dll`, and copy `dlssnr_on_amd_weights.bin` into the game directory. **Do not leave a file named `version.dll` from the original author in the game directory** to prevent proxy conflicts.
4. In the game directory's `OptiScaler.ini`, set:
   ```ini
   [DlssNr]
   Enabled = true
   RunBeforeSR = true
   NrBackend = lmxxf   ; or daniel
   ```

---

## 3. In-Game Ins Menu Controls

1. Launch the game.
2. Press the **Insert (Ins)** key on your keyboard to open the OptiScaler in-game overlay menu.
3. Locate **DLSS Neural Rendering** and check **Enable NR**.
   - The status line will display the active runtime: `AMD NR runtime: lmxxf` or `0.3.x`.
4. Denoising takes effect immediately: **DLSS inputs → Neural Denoise → FFX/FSR Super-Resolution**.

### `lmxxf` Backend Settings
- **Detail strength**: Stepless slider to fine-tune high-frequency detail and luminance.
- **Colour strength**: Stepless slider to balance color saturation and tone.
- **Debug view**: Real-time visual debugging overlays (original, processed, difference views).
- **Resolution Guidance**: Recommended render resolution before upscaling **≤ 1080p**:
  - **4K Output**: Recommended with **FSR Performance** (1080p render) or Ultra Performance (720p).
  - **2K (1440p) Output**: Recommended with **FSR Quality / Balanced / Performance**.
  - **1080p Output**: Supported on Native 1080p or any upscaling mode.

### `danielblnc` Backend Settings
- **NR slots**: Multi-slot scheduling (2–5 slots, default **3**). Eliminates idle GPU stalls between consecutive frames.
- **Pass Preset & Style**: Adjust denoising style and presets across passes.
- **Every-frame**: Toggle whether to enforce denoising on every single frame.

---

## 4. FAQ, Troubleshooting & Uninstallation

### Uninstallation
1. Navigate to the **game executable directory**.
2. Double-click **`Uninstall_OptiScaler_NR.bat`**.
3. The uninstaller will ask whether to keep old backup directories, show all planned removals, and ask for `Y/N` confirmation.
4. **Safety Guarantee**: The uninstaller **never deletes** your model weights (`native-game-tiled-assets/` and `dlssnr_on_amd_weights.bin`) or `nvngx_dlssnr.dll`.

### Logs & Diagnostics
If the menu fails to open or denoising does not engage, check the log files in the game directory (or `_storage_` on XBOX PC):
- `OptiScaler.log`: Main OptiScaler log.
- `amd_bridge.log`: AMD neural rendering bridge log.
- `amd_presr.log`: Pre-SR scheduling log.
- `dlssnr_on_amd.log`: Daniel backend runtime log.

**Troubleshooting Steps**:
1. **Menu does not appear**:
   - Try a different proxy name such as `winmm.dll` or `d3d12.dll`.
   - Ensure there is no leftover original `version.dll` conflicting in the game directory.
2. **Missing weights / EnqueueHip returns UNAVAILABLE**:
   - For `lmxxf`: Ensure `native-game-tiled-assets` is present in the game directory.
   - For `daniel`: Ensure `dlssnr_on_amd_weights.bin` is present in the game directory.

---

## 5. Attribution & Licenses

Lineage and technical foundation:
[OptiScaler](https://github.com/optiscaler/OptiScaler) → [Dagherbou](https://github.com/Dagherbou/OptiScaler_DLSSNR) → [wilsjo2](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) → [Matheus](https://github.com/MatheusGViana/dlss-5-amd-project) → **This Repository**.

- [**OptiScaler**](https://github.com/optiscaler/OptiScaler) (GPL-3.0) — Universal upscaling and neural rendering proxy framework.
- [**Dagherbou / OptiScaler_DLSSNR**](https://github.com/Dagherbou/OptiScaler_DLSSNR) (GPL-3.0) — Initial integration of DLSS-NR into OptiScaler.
- [**wilsjo2 / OptiScaler-DLSSNR-PreSR-Multipass**](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) — Pre-SR execution and Multi-Pass architecture.
- [**Matheus / dlss-5-amd-project**](https://github.com/MatheusGViana/dlss-5-amd-project) — AMD Pre-SR bridge.
- [**danielblnc / DLSS-NR-on-AMD**](https://github.com/danielblnc/DLSS-NR-on-AMD) — AMD neural rendering 0.3.1 / 0.3.0 runtime core.
- [**lmxxf / dlss5-on-amd-9070xt-porting**](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) — Open-source HIP neural rendering runtime and compute core.
- [**RenoDX / clshortfuse**](https://github.com/clshortfuse/renodx) (MIT) — `dlssnr.hlsl` color compositing algorithms.

This package does not redistribute proprietary NVIDIA binaries, closed-source installer executables, or unauthorized assets. Please adhere to all upstream licenses.
