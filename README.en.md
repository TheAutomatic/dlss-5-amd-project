[中文](README.md) | **English** | [Español](README.es.md)

# OptiScaler(NR)

> [!IMPORTANT]
> **Source-Available · Completely Free · No Commercial Resale or Paid Installation**: The launcher, installer scripts, and packaging tools in this repository are licensed under **PolyForm Noncommercial License 1.0.0** for personal, noncommercial use only. If you paid for this tool, request a refund immediately! See [Section 6](#6-free-software-notice-attributions--licenses).
>
> **🎉 v2.0 Full Architecture Upgrade**: Upgraded to the **"Official OptiScaler + Dual Standalone `.asi` Plugins (`dlssnr-amd.asi` + `openrr-amd.asi`) + One-Click GUI Launcher"** architecture! Supports **OptiScaler / Standalone Direct / ReShade Compatible / RTX Remix for Minecraft (Java Edition)**.
> Looking for the legacy `1.x` documentation? Click here: **[Legacy English README](docs/legacy-readme/README.en.md)**.

**Special thanks** to everyone who tested builds and reported feedback.

This repository provides a graphical launcher (**`OptiScalerNRLauncher.exe`**) that automatically scans Steam / Epic / GOG / Xbox game libraries, combines PE import analysis with OptiScaler Wiki profiles to recommend the optimal injection proxy DLL, extracts and synthesizes model weights offline in one click, and deploys **official OptiScaler** alongside two standalone AMD neural rendering plugins—**DLSS Neural Rendering (`dlssnr-amd.asi`)** and **DLSS Ray Reconstruction (`openrr-amd.asi`)**—complete with shared INI profiles and `MZ5` style share codes.

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

Version `2.0` retires custom-patched `OptiScaler.dll` builds in favor of a decoupled architecture combining **official OptiScaler** with **standalone opt-in `.asi` plugins**:

| Component | Upstream & Source | Role & Features |
|---|---|---|
| **[OptiScaler](https://github.com/optiscaler/OptiScaler)**<br>(Official Upscaling Bridge) | Official upstream Nightly build (SHA-256 verified; replaceable with other third-party builds) | Handles universal upscaling conversion (DLSS/XeSS/FSR → FSR 3.1 / FSR 4 / XeSS) and frame generation (DLSSG → FSR3 FG / XeFG), loading `OptiScaler\plugins\*.asi` via its built-in ASI Loader |
| **`dlssnr-amd.asi`**<br>(DLSS Neural Rendering Plugin) | **[mochizuki0323 / DLSSNR-AMD](https://gitlab.com/mochizuki0323/DLSSNR-AMD)**<br>**[TheAutomatic / DLSSNR-AMD](https://github.com/TheAutomatic/DLSSNR-AMD)** | Runs DLSS Neural Rendering on AMD **RX 9000 (RDNA4)** and **RX 7000 / 700M / 800M / 8000S (RDNA3)** GPUs; supports **D3D12 Direct-ELF native recording** and **Vulkan ACO machine-code acceleration**, with built-in RenoDX and wilsjo2 styles |
| **`openrr-amd.asi`**<br>(DLSS Ray Reconstruction Plugin) | **[Storm (Matheus) / OpenRR](https://github.com/OpenNeuralRendering/OpenRR)**<br>**[TheAutomatic / OpenRR-ASI](https://github.com/TheAutomatic/OpenRR-ASI)** | Runs NVIDIA's **DLSS Ray Reconstruction (DLSS-RR)** network natively on AMD **RDNA4 (tested)** and **RDNA3 / RDNA3.5 (experimental)** GPUs; supports **D3D12 Direct-ELF native recording** and **Vulkan**, coexisting seamlessly with `dlssnr-amd.asi` |
| **[rtx-remix-minecraft-amd-patch](https://github.com/TheAutomatic/rtx-remix-minecraft-amd-patch)**<br>(RTX Remix for Minecraft Patch) | **[NVIDIAGameWorks / rtx-remix-minecraft](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)** (Apache-2.0)<br>**[TheAutomatic / rtx-remix-minecraft-amd-patch](https://github.com/TheAutomatic/rtx-remix-minecraft-amd-patch)** | Auto-scans PrismLauncher / `.minecraft\remix-sdk` instances or adds them manually; deploys unrenamed `OptiScaler.dll`, backs up & patches `remixbridge.dll` (restoring the original on full uninstall), and dynamically sets `[Spoofing] VulkanExtensionSpoofing = true` |

---

## 2. ⚡ Quick Installation Guide (Windows & Linux)

### Windows GUI Launcher Installation (Recommended)

1. **Download & Extract**: Download the latest `.zip` archive from the [Releases page](https://github.com/TheAutomatic/dlss-5-amd-project/releases) and extract all files.
2. **Open Launcher**: Double-click **`OptiScalerNRLauncher.exe`** in the extracted folder to open **`OptiScaler NR` Launcher**.
3. **Select Game & Injection Proxy**:
   - Pick a game from the left-hand library list (or click **`+`** at the top right to manually select the game's actual rendering `.exe`; for Unreal Engine games, pick `<Game>\Binaries\Win64\*-Shipping.exe`);
   - The launcher combines PE import analysis and OptiScaler Wiki profiles to pre-select the recommended proxy DLL (typically `dxgi.dll` or `winmm.dll`).
4. **Select Which `.asi` Plugins to Install**:
   - **[☑] `dlssnr-amd.asi` (DLSS Neural Rendering · Mochizuki)**:
     - On first use, if prompted for a model source, click **"Import model source file…"** and select your `nvngx_dlssnr.dll` (`310.9.16` [both small & original models] or `310.8.0` [original model]). The launcher saves it into the launcher folder and extracts/synthesizes the weights during installation for reuse across all games.
   - **[☑] `openrr-amd.asi` (OpenRR Ray Reconstruction)**:
     - Supported in games with native NVIDIA Ray Reconstruction. If a `310.9.1.0` `nvngx_dlssd.dll` is already present in an installed game folder, it is detected automatically; if prompted, click **"Import nvngx_dlssd.dll…"** once to select `nvngx_dlssd.dll` (`310.9.1.0`) for reuse across all games.
   - **[☐] Install OptiPatcher (Optional)**: Auto-selected via Wiki and built-in game coverage to unlock native DLSS options in 270+ games.
5. **One-Click Install**: Click **"Install OptiScaler NR"** (or **"Update OptiScaler NR"**) at the top right to deploy.

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
  - **AMD Radeon RX 7000 Series & Radeon 700M / 800M / 8000S (RDNA3)**: Also supports all 4 networks via pre-compiled RDNA3 ACO machine code.
- **Dual Backends**: Supports **D3D12 Direct-ELF** (executes directly on the game's native D3D12 command list via the AMD driver with zero cross-API copy) and **Vulkan bridge / native Vulkan**.
- **Dual Processing Styles**: Supports **RenoDX style** (default) and **wilsjo2 style**, with one-click **`MZ5` style share code** import/export in the launcher.

### 2. `openrr-amd.asi` (OpenRR · DLSS Ray Reconstruction)
- **Supported GPUs**: **AMD Radeon RX 9000 Series (RDNA4, tested)** and **RX 7000 / 700M / 800M / 8000S Series (RDNA3 / RDNA3.5, experimental)**; requires `nvngx_dlssd.dll` (`310.9.1.0`).
- **Core Capability**: Intercepts `NVSDK_NGX_Feature_RayReconstruction` (DLSS Ray Reconstruction) calls in titles with native Ray Reconstruction (such as *Cyberpunk 2077*, *Alan Wake 2*, *Indiana Jones*, etc.) to run neural ray reconstruction denoising natively on AMD.
- **Dual Backends & Coexistence**: Supports **D3D12 Direct-ELF** and **Vulkan**, and automatically coordinates command lists and hotkeys when loaded alongside `dlssnr-amd.asi`.
- **GPU Spoofing Policy**: Defaults per game to full spoofing (`full`, setting `[Spoofing]` `Dxgi`, `Vulkan`, and `VulkanExtensionSpoofing` to `true` in `OptiScaler.ini`). If global spoofing causes issues in a specific game, check **"Reduce global GPU spoofing"** on the OpenRR card (Windows `-OpenRrSpoof full|auto` / Linux `--openrr-spoof full|auto`) to write `auto` instead (disabling OpenRR also resets these keys to `auto`). Preserving an existing `OptiScaler.ini` updates only those three keys while leaving all other custom settings intact; CLI runs without an explicit flag reuse the previously recorded policy.

---

## 4. In-Game Hotkeys & Menu Controls

Once in game with **DLSS** (or FSR / XeSS) and **Ray Reconstruction** (if supported by the game) enabled:

| Hotkey | Menu / Action | Description |
|---|---|---|
| **`Insert (Ins)`** | **OptiScaler Menu** + **OpenRR Sidebar** | Configures upscaling (FSR / XeSS) and frame generation; when `openrr-amd.asi` is installed, the **OpenRR settings panel docks and opens/closes together with `Insert`** |
| **`End`** | **NR Control (Mochizuki Neural Rendering Menu)** | Configures all `dlssnr-amd.asi` settings: live-switch between the 4 networks, switch RenoDX / wilsjo2 styles, configure NR chain layers and Passes, HUD fix, and VRAM/performance diagnostics |
| **`Ctrl + F11`** | Cycle NR Network | Cycles through installed Neural Rendering networks in game |
| **`F5`** | NR Comparison Screenshot | Saves side-by-side screenshots with and without NR into `NR Screenshots\` in the game folder |

---

## 5. Compatibility Repair, Log Packing & Uninstallation

1. **Companion Runtime Compatibility Repair (XeSS / FidelityFX)**:
   - If a game's bundled `libxess.dll` / `libxell.dll` / `amd_fidelityfx_*.dll` causes upscaling or frame generation issues, select the game in the launcher and click **"Compat Repair"** to back up the original files and replace them; click **"Restore Files"** at any time to revert.
2. **One-Click Log Packing (Bug Reports)**:
   - Click **"Pack logs"** at the bottom right of the launcher to bundle `OptiScaler.log`, `dlssnr-amd.log`, `openrr-amd.log`, INI files, hardware/driver summary, and recent GPU driver crash events into a `.zip` on your Desktop.
3. **Clean Uninstallation**:
   - Select the game in the launcher and click **"Uninstall"** (or double-click `Uninstall_OptiScaler_NR.bat` in the game folder) to cleanly remove all injected proxies, `.asi` plugins, shaders, logs, and legacy artifacts while keeping extracted models in the launcher folder for future installs.

---

## 6. Free Software Notice, Attributions & Licenses

### Free Software & Anti-Resale Notice
This software is distributed **free of charge** for personal, noncommercial use. **Selling this tool or its packages, bundling it in paid modpacks or paywalled groups, or charging for "remote installation / setup" services is strictly prohibited.** If you paid for this package, request a refund immediately!

### Attributions & Licenses
- **[This Project (TheAutomatic / dlss-5-amd-project)](https://github.com/TheAutomatic/dlss-5-amd-project)** — **[PolyForm Noncommercial License 1.0.0](LICENSE)** (see [NOTICE](NOTICE)): Graphical launcher, PE import probe, multi-mode deployment, installer/uninstaller scripts, shared INI profiles, and style share codes (**noncommercial use only; commercial resale and paid installation services prohibited**);
- **[RTX Remix for Minecraft AMD Patch](https://github.com/TheAutomatic/rtx-remix-minecraft-amd-patch)** (`remixbridge.dll`) — **PolyForm Noncommercial License 1.0.0**: Derived from [NVIDIAGameWorks/rtx-remix-minecraft](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft) (**Apache License 2.0**);
- **[OptiScaler](https://github.com/optiscaler/OptiScaler)** — **GPL-3.0 License**: Official universal upscaling and frame generation bridge framework (DLL distributed unmodified; replaceable with other third-party builds);
- **[Mochizuki / DLSSNR-AMD](https://gitlab.com/mochizuki0323/DLSSNR-AMD)** & **[TheAutomatic / DLSSNR-AMD](https://github.com/TheAutomatic/DLSSNR-AMD)** (`dlssnr-amd.asi`) — **MIT License** (© mochizuki0323): AMD RDNA4 / RDNA3 Neural Rendering core, D3D12 Direct-ELF native path, ACO machine-code records, and NR Control plugin;
- **[Storm (Matheus) / OpenRR](https://github.com/OpenNeuralRendering/OpenRR)** & **[TheAutomatic / OpenRR-ASI](https://github.com/TheAutomatic/OpenRR-ASI)** (`openrr-amd.asi`) — **MIT License**: Open-source AMD RDNA4 / RDNA3 DLSS Ray Reconstruction runtime and standalone ASI plugin;
- **[RenoDX / clshortfuse](https://github.com/clshortfuse/renodx)** — **MIT License**: RenoDX tone mapping and color processing algorithms;
- **[OptiPatcher](https://github.com/optiscaler/OptiPatcher)** — **GPL-3.0 License**: Native DLSS option unlocker ASI plugin.

> This repository and release packages contain **no** proprietary NVIDIA binaries (`nvngx_dlssnr.dll` / `nvngx_dlssd.dll`) or extracted model weight files (`*.bin`). All model weights are extracted locally by the user from their own files.
