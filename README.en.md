[中文](README.md) | **English** | [Español](README.es.md)

# OptScaler(NR)

> [!IMPORTANT]
> **🎉 v2.0 Full Architecture Upgrade**: This project has been upgraded to the **"Official Unmodified OptiScaler + Dual Standalone `.asi` Plugins (`dlssnr-amd.asi` + `openrr-amd.asi`) + One-Click GUI Launcher"** architecture!
> Looking for the legacy `1.x` documentation (the custom 3-backend `lmxxf` / `danielblnc` / `mochizuki` fork)? Click here: **[Legacy English README](docs/legacy-readme/README.en.md)**.

**Special thanks** to everyone who tested builds and reported feedback.

This repository provides a graphical launcher (**`OptScalerNR.Launcher.exe`**) that automatically scans Steam / Epic / GOG / Xbox game libraries, analyzes game `.exe` PE imports to recommend the optimal injection proxy DLL, extracts and synthesizes model weights offline in one click, and lets you freely select and deploy **official unmodified OptiScaler** alongside two standalone AMD neural rendering plugins—**DLSS Neural Rendering (`dlssnr-amd.asi`)** and **DLSS Ray Reconstruction (`openrr-amd.asi`)**—complete with shared INI profiles and style share codes.

**Project Home: [github.com/TheAutomatic/dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)**

---

## Table of Contents
- [1. Core Architecture & Upstream Credits](#1-core-architecture--upstream-credits)
- [2. ⚡ Quick Installation Guide (Windows & Linux)](#2--quick-installation-guide-windows--linux)
  - ├─► [Windows GUI Launcher Installation (Recommended)](#windows-gui-launcher-installation-recommended)
  - └─► [Linux / SteamOS (Proton) Script Installation](#linux--steamos-proton-script-installation)
- [3. Dual `.asi` Plugin Capabilities](#3-dual-asi-plugin-capabilities)
- [4. In-Game Hotkeys & Menu Controls](#4-in-game-hotkeys--menu-controls)
- [5. Compatibility Repair, Log Packing & Uninstallation](#5-compatibility-repair-log-packing--uninstallation)
- [6. Free Software Notice, Attributions & Licenses](#6-free-software-notice-attributions--licenses)

---

## 1. Core Architecture & Upstream Credits

In `2.0`, we no longer modify or rebuild the `OptiScaler.dll` host. Instead, we use a clean decoupled architecture of **official upstream host + standalone `.asi` plugins**:

| Component | Upstream & Source | Role & Features |
|---|---|---|
| **[OptiScaler](https://github.com/optiscaler/OptiScaler)**<br>(Official Unmodified Host) | Official upstream release (packaged unmodified and verified by SHA-256) | Handles universal upscaling conversion (DLSS/XeSS/FSR → FSR 3.1 / FSR 4 / XeSS) and frame generation (DLSSG → FSR3 FG / XeFG), and loads `OptiScaler\plugins\*.asi` via its built-in ASI Loader |
| **`dlssnr-amd.asi`**<br>(DLSS Neural Rendering Plugin) | **[mochizuki0323 / DLSSNR-AMD](https://gitlab.com/mochizuki0323/DLSSNR-AMD)**<br>**[TheAutomatic / DLSSNR-AMD](https://github.com/TheAutomatic/DLSSNR-AMD)** | Runs DLSS Neural Rendering on AMD **RX 9000 (RDNA4)** and **RX 7000 / 700M / 800M (RDNA3)** GPUs; supports **D3D12 Direct-ELF native recording** and **Vulkan ACO machine-code acceleration**, with built-in RenoDX and wilsjo2 styles |
| **`openrr-amd.asi`**<br>(DLSS Ray Reconstruction Plugin) | **[Storm / OpenRR](https://github.com/OpenNeuralRendering/OpenRR)**<br>**[TheAutomatic / OpenRR-ASI](https://github.com/TheAutomatic/OpenRR-ASI)** | Runs NVIDIA's **DLSS Ray Reconstruction (DLSS-RR)** network natively on AMD **RX 9000 (RDNA4)** GPUs; supports **D3D12 Direct-ELF native recording** and **Vulkan**, coexisting seamlessly with `dlssnr-amd.asi` |

---

## 2. ⚡ Quick Installation Guide (Windows & Linux)

### Windows GUI Launcher Installation (Recommended)

1. **Download & Extract**: Download the latest `.zip` archive from the [Releases page](https://github.com/TheAutomatic/dlss-5-amd-project/releases) and extract all files.
2. **Open Launcher**: Double-click **`OptScalerNR.Launcher.exe`** in the extracted folder to open **`OptScaler NR` Launcher**.
3. **Select Game & Injection Proxy**:
   - Pick a game from the left-hand library list (or click **`+`** at the top right to manually select the game's actual rendering `.exe`; for Unreal Engine games, pick `<Game>\Binaries\Win64\*-Shipping.exe`);
   - The launcher automatically analyzes the game's PE imports and pre-selects the recommended proxy DLL (typically `dxgi.dll` or `winmm.dll`).
4. **Select Which `.asi` Plugins to Install**:
   - **[☑] `dlssnr-amd.asi` (DLSS Neural Rendering · Mochizuki)**:
     - On first use, if prompted for a model source, click **"Import model source file…"** and select your `nvngx_dlssnr.dll` (`310.9.16` [both small & original models] or `310.8.0` [original model]). The launcher copies it into the launcher folder and extracts/generates the model weights during installation; later installs for other games reuse them automatically.
   - **[☑] `openrr-amd.asi` (OpenRR Ray Reconstruction · RX 9000)**:
     - When checked, click **"Import nvngx_dlssd.dll…"** on the card once and select `nvngx_dlssd.dll` version **`310.9.1.0`**, which is kept in the launcher folder for all games.
   - **[☐] Install OptiPatcher (Optional)**: Pre-selected based on Wiki recommendations to unlock native DLSS options in 270+ games.
5. **One-Click Install**: Click the red **"Install / Update"** button at the top right!

---

### Linux / SteamOS (Proton) Script Installation

The package includes [`install.sh`](install.sh) for Linux / Steam Deck (Proton):

```bash
# Interactive install (scans game EXE imports to recommend proxy, then asks whether to install dlssnr-amd.asi, openrr-amd.asi, and OptiPatcher.asi)
bash install.sh "/path/to/game/folder"

# Or non-interactive one-liner
bash install.sh "/path/to/game/folder" install --proxy dxgi.dll --nr on --openrr on --dll /path/to/nvngx_dlssnr.dll --dlssd /path/to/nvngx_dlssd.dll

# Uninstall / Pack diagnostic logs
bash install.sh "/path/to/game/folder" remove
bash install.sh "/path/to/game/folder" logs
```
After installation, add the printed Steam **Launch Options** to the game in Steam:
```text
WINEDLLOVERRIDES="dxgi=n,b" %command%
```

---

## 3. Dual `.asi` Plugin Capabilities

### 1. `dlssnr-amd.asi` (Mochizuki · DLSS Neural Rendering)
- **Supported GPUs**:
  - **AMD Radeon RX 9000 Series (RDNA4)**: Supports all 4 networks (`CC_SMALL_FP8`, `CC_SMALL_FP8_EXPERIMENTAL`, `CC_SILVER_AARDWOLD`, `CC_SILVER_AARDWOLD_OPTIMIZED` with INT4 layers).
  - **AMD Radeon RX 7000 Series & Radeon 700M / 800M / 8000S (RDNA3)**: Supports `CC_SMALL_FP8` and `CC_SILVER_AARDWOLD` via pre-compiled RDNA3 ACO machine code.
- **Dual Backends**: Supports **D3D12 Direct-ELF** (executes directly on the game's native D3D12 command list via the AMD driver with zero cross-API copy) and **Vulkan bridge / native Vulkan**.
- **Dual Processing Styles**: Supports **RenoDX style** (default) and **wilsjo2 style**, with one-click **style share code** import/export in the launcher.

### 2. `openrr-amd.asi` (OpenRR · DLSS Ray Reconstruction)
- **Supported GPUs**: **AMD Radeon RX 9000 Series (RDNA4)** (requires `nvngx_dlssd.dll` `310.9.1.0`).
- **Core Capability**: Intercepts `NVSDK_NGX_Feature_RayReconstruction` (DLSS Ray Reconstruction) calls in path-traced / ray-traced titles (such as *Cyberpunk 2077*, *Alan Wake 2*, *Indiana Jones*, etc.) to run the neural ray reconstruction denoiser natively on AMD.
- **Dual Backends & Coexistence**: Supports **D3D12 Direct-ELF** and **Vulkan**, and automatically coordinates command lists and hotkeys when loaded alongside `dlssnr-amd.asi`.

---

## 4. In-Game Hotkeys & Menu Controls

Once in game with **DLSS** (or FSR / XeSS) and **Ray Reconstruction** (if supported by the game) enabled:

| Hotkey | Menu / Action | Description |
|---|---|---|
| **`Insert (Ins)`** | **OptiScaler Menu** + **OpenRR Sidebar** | Configures upscaling (FSR / XeSS) and frame generation; when `openrr-amd.asi` is installed, the **OpenRR settings panel docks and opens/closes together with `Insert`** |
| **`End`** | **NR Control (Mochizuki Neural Rendering Menu)** | Configures all `dlssnr-amd.asi` settings: live-switch between the 4 networks, switch RenoDX / wilsjo2 styles, adjust Passes (1–3), HUD fix, and diagnostics |
| **`Ctrl + F11`** | Cycle NR Network | Cycles through installed Neural Rendering networks in game |
| **`F5`** | NR Comparison Screenshot | Saves side-by-side screenshots with and without NR into `NR Screenshots\` in the game folder |

---

## 5. Compatibility Repair, Log Packing & Uninstallation

1. **Companion Runtime Compatibility Repair (XeSS / FidelityFX)**:
   - If a game's bundled `libxess.dll` / `libxell.dll` / `amd_fidelityfx_*.dll` causes upscaling or frame generation issues, select the game in the launcher and click **"Compatibility repair"** to back up the original files and replace them; click **"Restore game files"** at any time to revert.
2. **One-Click Log Packing (Bug Reports)**:
   - Click **"Pack Logs"** at the bottom right of the launcher to bundle `OptiScaler.log`, `dlssnr-amd.log`, `openrr-amd.log`, INI files, hardware/driver summary, and recent GPU driver crash events into a `.zip` on your Desktop.
3. **Clean Uninstallation**:
   - Select the game in the launcher and click **"Uninstall"** (or double-click `Uninstall_OptiScaler_NR.bat` in the game folder) to cleanly remove all injected proxies, `.asi` plugins, shaders, logs, and legacy artifacts while keeping extracted models in the launcher folder for future installs.

---

## 6. Free Software Notice, Attributions & Licenses

### Free Software Notice
This software is distributed free of charge. If you paid for this package, request a refund immediately.

### Attributions & Licenses
- **[OptiScaler](https://github.com/optiscaler/OptiScaler)** — **GPL-3.0 License**: Official universal upscaling and frame generation proxy framework (distributed unmodified);
- **[Mochizuki / DLSSNR-AMD](https://gitlab.com/mochizuki0323/DLSSNR-AMD)** & **[TheAutomatic / DLSSNR-AMD](https://github.com/TheAutomatic/DLSSNR-AMD)** (`dlssnr-amd.asi`) — **MIT License** (© mochizuki0323): AMD RDNA4 / RDNA3 Neural Rendering core, D3D12 Direct-ELF native path, ACO machine-code records, and NR Control plugin;
- **[Storm / OpenRR](https://github.com/OpenNeuralRendering/OpenRR)** & **[TheAutomatic / OpenRR-ASI](https://github.com/TheAutomatic/OpenRR-ASI)** (`openrr-amd.asi`) — **MIT License**: Open-source AMD RDNA4 DLSS Ray Reconstruction runtime and standalone ASI plugin;
- **[RenoDX / clshortfuse](https://github.com/clshortfuse/renodx)** — **MIT License**: RenoDX tone mapping and color processing algorithms;
- **[OptiPatcher](https://github.com/optiscaler/OptiPatcher)** — **GPL-3.0 License**: Native DLSS option unlocker ASI plugin;
- **[This Project (TheAutomatic / dlss-5-amd-project)](https://github.com/TheAutomatic/dlss-5-amd-project)** — **GPL-3.0 License**: Graphical launcher, PE import probe, dual `.asi` deployment, shared INI profiles, and style share codes.

> This repository and release packages contain **no** proprietary NVIDIA binaries (`nvngx_dlssnr.dll` / `nvngx_dlssd.dll`) or extracted model weight files (`*.bin`). All model weights are extracted locally by the user from their own files.
