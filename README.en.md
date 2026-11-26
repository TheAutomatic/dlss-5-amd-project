[中文](README.md) | **English** | [Español](README.es.md)

# OptiScaler(NR)

> [!IMPORTANT]
> **Source-Available · Completely Free · No Resale or Stripped Attribution**: The launcher, installer scripts, and packaging tools in this repository are licensed under **[PolyForm Noncommercial License 1.0.0](LICENSE)** for personal, noncommercial use only—commercial resale, paid bundles/groups/downloads, and paid installation services are strictly prohibited. **Noncommercial forks and derivative works must comply with [NOTICE](NOTICE)**: you may credit your own modifications, but MUST preserve attribution for the original author (`TheAutomatic`), all upstream authors, the official repository URL, and all `Required Notice:` lines; stripping attribution or altering binary metadata/version numbers to claim original authorship is prohibited. If you paid for this tool, request a refund immediately! See [Section 6](#6-free-software-notice-attributions--licenses).
>
> **🎉 v2.1 Architecture & Highlights**: Built on the decoupled **"Official OptiScaler + Dual Standalone `.asi` Plugins (`dlssnr-amd.asi` + `openrr-amd.asi`) + One-Click GUI Launcher"** architecture, supporting four deployment modes (**OptiScaler / Standalone Direct / ReShade Compatibility [D3D9–D3D12 & Vulkan, 32- & 64-bit] / RTX Remix for Minecraft [Java Edition]**) plus experimental standalone **Linux / SteamOS (Proton)** packages with a graphical launcher.
> Looking for the legacy `1.x` documentation? Click here: **[Legacy English README](docs/legacy-readme/README.en.md)**.

**Special thanks** to everyone who tested builds and reported feedback, and to the dozens of Bilibili community members who helped design the otter brand icon!

This repository provides a **Fluent UI** graphical launcher (Windows **`OptiScalerNRLauncher.exe`** / Linux **`OptiScalerNR-Launcher.sh`**) that automatically scans Steam / Epic / GOG / XBOX / Heroic / Lutris game libraries, combines PE import/bitness analysis with OptiScaler Wiki profiles to recommend the optimal deployment mode and proxy DLL, extracts and synthesizes model weights offline in one click, and deploys **official OptiScaler**, two standalone AMD neural rendering plugins—**DLSS Neural Rendering (`dlssnr-amd.asi`)** and **DLSS Ray Reconstruction (`openrr-amd.asi`)**—and the **RTX Remix for Minecraft AMD Patch**, complete with shared INI profiles, in-game Mochizuki hotkey customization, and `MZ5` style share codes.

**Project Home: [github.com/TheAutomatic/dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)**

---

## Table of Contents
- [1. Core Architecture & Upstream Credits](#1-core-architecture--upstream-credits)
- [2. ⚡ Quick Installation Guide (Windows & Linux)](#2--quick-installation-guide-windows--linux)
  - ├─► [Windows GUI Launcher Installation (Recommended)](#windows-gui-launcher-installation-recommended)
  - └─► [Linux / SteamOS (Proton) Graphical Launcher (Experimental)](#linux--steamos-proton-graphical-launcher-experimental)
- [3. Dual `.asi` Plugin Capabilities](#3-dual-asi-plugin-capabilities)
- [4. In-Game Hotkeys & Menu Controls](#4-in-game-hotkeys--menu-controls)
- [5. Compatibility Repair, Bug Report Logs & Uninstallation](#5-compatibility-repair-bug-report-logs--uninstallation)
- [6. Free Software Notice, Attributions & Licenses](#6-free-software-notice-attributions--licenses)

---

## 1. Core Architecture & Upstream Credits

Version `2.x` retires custom-patched `OptiScaler.dll` builds in favor of a decoupled architecture combining **official OptiScaler**, **standalone opt-in `.asi` plugins**, and **multi-mode compatibility routes**:

| Component | Upstream & Source | Role & Features |
|---|---|---|
| **[OptiScaler](https://github.com/optiscaler/OptiScaler)**<br>(Official Upscaling Bridge) | Official upstream Nightly build (SHA-256 verified; replaceable with other third-party builds) | Handles universal upscaling conversion (DLSS/XeSS/FSR → FSR 3.1 / FSR 4 / XeSS) and frame generation (DLSSG → FSR3 FG / XeFG), loading `OptiScaler\plugins\*.asi` via its built-in ASI Loader |
| **`dlssnr-amd.asi`**<br>(DLSS Neural Rendering Plugin) | **[mochizuki0323 / DLSSNR-AMD](https://gitlab.com/mochizuki0323/DLSSNR-AMD)**<br>**[TheAutomatic / DLSSNR-AMD](https://github.com/TheAutomatic/DLSSNR-AMD)** | Runs DLSS Neural Rendering on AMD **RX 9000 (RDNA4)** and **RX 7000 / 700M / 800M / 8000S (RDNA3)** GPUs; supports **D3D12 Direct-ELF**, **D3D11→D3D12 bridge**, and **Vulkan ACO**, across OptiScaler / Standalone / 32- & 64-bit ReShade routes with RenoDX & wilsjo2 styles |
| **`openrr-amd.asi`**<br>(DLSS Ray Reconstruction Plugin) | **[Storm (Matheus) / OpenRR](https://github.com/OpenNeuralRendering/OpenRR)**<br>**[TheAutomatic / OpenRR-ASI](https://github.com/TheAutomatic/OpenRR-ASI)** | Runs NVIDIA's **DLSS Ray Reconstruction (DLSS-RR)** network natively on AMD **RDNA4 (tested)** and **RDNA3 / RDNA3.5 (experimental)** GPUs; supports **D3D12 Direct-ELF** and **Vulkan**, coexisting smoothly with `dlssnr-amd.asi` and ReShade / RenoDX |
| **[rtx-remix-minecraft-amd-patch](https://github.com/TheAutomatic/rtx-remix-minecraft-amd-patch)**<br>(RTX Remix for Minecraft Patch) | **[NVIDIAGameWorks / rtx-remix-minecraft](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)** (Apache-2.0)<br>**[TheAutomatic / rtx-remix-minecraft-amd-patch](https://github.com/TheAutomatic/rtx-remix-minecraft-amd-patch)** | Auto-scans PrismLauncher / `.minecraft\remix-sdk` instances; deploys unrenamed `OptiScaler.dll`, backs up & patches `remixbridge.dll` to unlock AMD hardware **Wave32 RTX Sparse Rendering (~2.5× PT speedup)** (restorable on uninstall), and sets `VulkanExtensionSpoofing = true` |
| **[ReShade](https://github.com/crosire/reshade) / [DXVK](https://github.com/doitsujin/dxvk) / [Vulkan Loader](https://github.com/KhronosGroup/Vulkan-Loader)**<br>(Compatibility Route Runtimes) | Bundled via `DLSSNR-AMD` under their respective open-source licenses (see `Licenses/`) | Powers the 32-bit and 64-bit ReShade Feeder compatibility routes for D3D9 / D3D10 / D3D11 / D3D12 / Vulkan titles without native DLSS / FSR / XeSS |

---

## 2. ⚡ Quick Installation Guide (Windows & Linux)

### Windows GUI Launcher Installation (Recommended)

1. **Download & Extract**: Download `OptiScaler-NR-Win-<version>.zip` from the [Releases page](https://github.com/TheAutomatic/dlss-5-amd-project/releases) and extract all files.
2. **Open Launcher**: Double-click **`OptiScalerNRLauncher.exe`** in the extracted folder to open **`OptiScaler NR` Launcher**.
3. **Select Game & Deployment Mode**:
   - Pick a game from the left-hand library list (or click **`+`** at the top right to manually add a game `.exe` or an RTX Remix Minecraft instance; for Unreal Engine games, pick `<Game>\Binaries\Win64\*-Shipping.exe`);
   - The launcher automatically recommends the optimal deployment mode (**OptiScaler** / **Standalone Direct** / **ReShade Compatibility [32- & 64-bit]** / **RTX Remix for Minecraft**) and proxy DLL (typically `dxgi.dll` or `winmm.dll`) based on PE bitness/imports and OptiScaler Wiki profiles.
4. **Select Which `.asi` Plugins to Install**:
   - **[☑] `dlssnr-amd.asi` (DLSS Neural Rendering · Mochizuki)**:
     - On first use, if prompted for a model source, click **"Import model source file…"** and select your `nvngx_dlssnr.dll` (`310.9.16` [both small & original models] or `310.8.0` [original model]). The launcher saves it into the launcher folder and extracts/synthesizes the weights during installation for reuse across all games.
   - **[☑] `openrr-amd.asi` (OpenRR Ray Reconstruction)**:
     - Supported in games with native NVIDIA Ray Reconstruction. If a `310.9.1.0` `nvngx_dlssd.dll` is already present in an installed game folder, it is detected automatically; if prompted, click **"Import nvngx_dlssd.dll…"** once to select `nvngx_dlssd.dll` (`310.9.1.0`) for reuse across all games.
   - **[☐] Install OptiPatcher (Optional)**: Auto-selected via Wiki and built-in game coverage to unlock native DLSS options in 270+ games.
5. **One-Click Install**: Click **"Install OptiScaler NR"** (or **"Update OptiScaler NR"**) at the top right to deploy.

---

### Linux / SteamOS (Proton) Graphical Launcher (Experimental)

Download a Linux package from the [Releases page](https://github.com/TheAutomatic/dlss-5-amd-project/releases). It is built on Mochizuki's upstream [DLSSNR-AMD for Linux](https://gitlab.com/mochizuki0323/DLSSNR-AMD/-/releases) and, like upstream, comes in two architecture builds:

| Package | For | Install routes |
| --- | --- | --- |
| `OptiScaler-NR-Linux-x86_64-<version>.tar.gz` | 64-bit games | OptiScaler (Mochizuki NR + OpenRR 0.1.7plus6 + OptiPatcher) + three ReShade Feeder routes |
| `OptiScaler-NR-Linux-i686-<version>.tar.gz` | 32-bit games | ReShade Feeder routes only (OptiScaler and OpenRR are 64-bit only) |

```bash
tar -xzf OptiScaler-NR-Linux-x86_64-<version>.tar.gz && cd OptiScaler-NR-Linux-x86_64-<version>
./OptiScalerNR-Launcher.sh                 # graphical launcher (python3 >= 3.8; scans Steam / Heroic / Lutris)
./OptiScalerNR-Launcher.sh --install-desktop   # add an app-menu entry
bash uninstall.sh "/path/to/game/folder"   # uninstall (--all = every game the launcher installed into)
```
Upstream's `install.sh` ships unmodified and can also be used directly (see `README-Linux.md` / `README.txt` in the package). After installing, copy the **launch options** shown by the launcher into Steam › Properties › General › Launch Options, e.g.:
```text
WINEDLLOVERRIDES="dxgi=n,b" %command%
```

---

## 3. Dual `.asi` Plugin Capabilities

### 1. `dlssnr-amd.asi` (Mochizuki · DLSS Neural Rendering)
- **Supported GPUs**:
  - **AMD Radeon RX 9000 Series (RDNA4)**: Supports all 4 networks (`CC_SMALL_FP8`, `CC_SMALL_FP8_EXPERIMENTAL`, `CC_SILVER_AARDWOLD`, `CC_SILVER_AARDWOLD_OPTIMIZED` with INT4 layers).
  - **AMD Radeon RX 7000 Series & Radeon 700M / 800M / 8000S (RDNA3 / RDNA3.5)**: Also supports all 4 networks via pre-compiled RDNA3 ACO machine code.
- **Multi-Backend & Compatibility Routes**: Supports **D3D12 Direct-ELF** (zero-copy native D3D12 command-list execution), **D3D11 → D3D12 bridge**, and **Vulkan bridge / native Vulkan** across OptiScaler, Standalone, and 32- & 64-bit ReShade Feeder routes.
- **Dual Processing Styles**: Supports **RenoDX style** (default) and **wilsjo2 style**, with one-click **`MZ5` style share code** import/export in the launcher.

### 2. `openrr-amd.asi` (OpenRR · DLSS Ray Reconstruction)
- **Supported GPUs**: **AMD Radeon RX 9000 Series (RDNA4, tested)** and **RX 7000 / 700M / 800M / 8000S Series (RDNA3 / RDNA3.5, experimental)**; requires `nvngx_dlssd.dll` (`310.9.1.0`).
- **Core Capability**: Intercepts `NVSDK_NGX_Feature_RayReconstruction` (DLSS Ray Reconstruction) calls in titles with native Ray Reconstruction (such as *Cyberpunk 2077*, *Alan Wake 2*, *Indiana Jones*, *Minecraft RTX Remix*, etc.) to run neural ray reconstruction denoising natively on AMD.
- **Dual Backends & Coexistence**: Supports **D3D12 Direct-ELF** and **Vulkan**; coexists smoothly with `dlssnr-amd.asi` and ReShade / RenoDX, and supports Linux / Proton (`vkd3d-proton`, Mesa RADV ≥ 26.3).
- **GPU Spoofing Policy**: Defaults per game to full spoofing (`full`, setting `[Spoofing]` `Dxgi`, `Vulkan`, and `VulkanExtensionSpoofing` to `true` in `OptiScaler.ini`). If global spoofing causes issues in a specific game, check **"Reduce global GPU spoofing"** on the OpenRR card (Windows `-OpenRrSpoof full|auto` / Linux `--openrr-spoof full|auto`) to write `auto` instead (disabling OpenRR also resets these keys to `auto`). Preserving an existing `OptiScaler.ini` updates only those three keys while leaving all other custom settings intact; CLI runs without an explicit flag reuse the previously recorded policy.

---

## 4. In-Game Hotkeys & Menu Controls

Once in game with **DLSS** (or FSR / XeSS) and **Ray Reconstruction** (if supported by the game) enabled, default hotkeys are listed below (the Windows launcher also includes a **Mochizuki hotkey customization menu** and displays inline hotkey hints on plugin rows):

| Hotkey | Menu / Action | Description |
|---|---|---|
| **`Insert (Ins)`** | **OptiScaler Menu** + **OpenRR Sidebar** | Configures upscaling (FSR / XeSS) and frame generation; when `openrr-amd.asi` is installed, the **OpenRR settings panel docks and opens/closes together with `Insert`** |
| **`End`** | **NR Control (Mochizuki Neural Rendering Menu)** | Configures all `dlssnr-amd.asi` settings: live-switch between the 4 networks, switch RenoDX / wilsjo2 styles, configure NR chain layers and Passes, HUD fix, and VRAM/performance diagnostics |
| **`Ctrl + F11`** | Cycle NR Network | Cycles through installed Neural Rendering networks in game |
| **`F5`** | NR Comparison Screenshot | Saves side-by-side screenshots with and without NR into `NR Screenshots\` in the game folder |

---

## 5. Compatibility Repair, Bug Report Logs & Uninstallation

1. **Companion Runtime Compatibility Repair (XeSS / FidelityFX)**:
   - If a game's bundled `libxess.dll` / `libxell.dll` / `amd_fidelityfx_*.dll` causes upscaling or frame generation issues, select the game in the launcher and click **"Compat Repair"** to back up the original files and replace them; click **"Restore Files"** at any time to revert.
2. **One-Click Bug Report Logs**:
   - Click **"Bug report logs"** in the main header bar of the launcher to bundle `OptiScaler.log`, `dlssnr-amd.log`, `openrr-amd.log`, INI files, hardware/driver summary, and recent GPU driver crash events into a `.zip` on your Desktop.
3. **Clean Uninstallation**:
   - Select the game in the launcher and click **"Uninstall"** (or double-click `Uninstall_OptiScaler_NR.bat` in the game folder) to cleanly remove all injected proxies, `.asi` plugins, shaders, logs, and legacy artifacts (and restore the original `remixbridge.dll` for RTX Remix MC) while keeping extracted models in the launcher folder for future installs.

---

## 6. Free Software Notice, Attributions & Licenses

### Free Software, Anti-Resale & Derivative Attribution Notice
This software is distributed **free of charge** for personal, noncommercial use. **Selling this tool or its packages, bundling it in paid modpacks or paywalled groups/downloads, or charging for "remote installation / setup / tuning" services is strictly prohibited.** If you paid for this package, request a refund immediately!

**Noncommercial forks, modifications, repackaged builds, and derivative works MUST comply with [NOTICE](NOTICE)**: derivative authors may add credit for their own changes, but MUST keep original author attribution (`TheAutomatic`), all upstream copyright notices, the official repository URL, and all `Required Notice:` lines intact. **Stripping attribution or altering binary metadata/version numbers to claim original authorship is strictly prohibited.**

### Attributions & Licenses
Full license texts for all upstream and third-party components are shipped in every release package under `Licenses/` alongside the root [NOTICE](NOTICE):

#### Core Project & Plugins
- **[This Project (TheAutomatic / dlss-5-amd-project)](https://github.com/TheAutomatic/dlss-5-amd-project)** — **[PolyForm Noncommercial License 1.0.0](LICENSE)** (see [NOTICE](NOTICE)): Graphical launcher, PE import probe, multi-mode deployment, installer/uninstaller scripts, shared INI profiles, and style share codes (**noncommercial use only; commercial resale and paid installation services prohibited**);
- **[RTX Remix for Minecraft AMD Patch](https://github.com/TheAutomatic/rtx-remix-minecraft-amd-patch)** (`remixbridge.dll`) — **PolyForm Noncommercial License 1.0.0**: Derived from [NVIDIAGameWorks/rtx-remix-minecraft](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft) (**Apache License 2.0**; includes NVIDIA Remix C API header under **MIT License**);
- **[OptiScaler](https://github.com/optiscaler/OptiScaler)** — **GPL-3.0 License**: Official universal upscaling and frame generation bridge framework (distributed unmodified as an independent aggregate with no additional restrictions; exact pinned version and corresponding source documented in `Licenses/OptiScaler_SOURCE.txt`; replaceable with other third-party builds);
- **[TheAutomatic / DLSSNR-AMD](https://github.com/TheAutomatic/DLSSNR-AMD)** (`dlssnr-amd.asi`) — **PolyForm Noncommercial License 1.0.0** (TheAutomatic modifications and release builds), derived from **[Mochizuki / DLSSNR-AMD](https://gitlab.com/mochizuki0323/DLSSNR-AMD)** (**MIT License**, © mochizuki0323): AMD RDNA4 / RDNA3 Neural Rendering core, D3D12 Direct-ELF / D3D11 bridge paths, ACO machine-code records, and NR Control plugin;
- **[TheAutomatic / OpenRR-ASI](https://github.com/TheAutomatic/OpenRR-ASI)** (`openrr-amd.asi`) — **PolyForm Noncommercial License 1.0.0** (TheAutomatic plugin additions and release builds), derived from **[Storm (Matheus) / OpenRR](https://github.com/OpenNeuralRendering/OpenRR)** (**MIT License**, © 2026 OpenNR): Open-source AMD RDNA4 / RDNA3 DLSS Ray Reconstruction runtime and standalone ASI plugin;
- **[RenoDX / clshortfuse](https://github.com/clshortfuse/renodx)** — **MIT License**: RenoDX tone mapping and color processing algorithms (`Licenses/RenoDX_MIT.txt`);
- **[OptiPatcher](https://github.com/optiscaler/OptiPatcher)** — **GPL-3.0 License**: Native DLSS option unlocker ASI plugin (distributed unmodified as an independent aggregate with no additional restrictions; corresponding source in `Licenses/OptiPatcher_SOURCE.txt`).

#### Compatibility-Mode & Bundled Third-Party Open-Source Components (distributed as independent aggregates under their own licenses)
- **[ReShade](https://github.com/crosire/reshade)** (`ReShade64.dll` / `ReShade32.dll` and standard shader headers) — **BSD-3-Clause License** (`Licenses/ReShade-LICENSE.md`);
- **[DXVK](https://github.com/doitsujin/dxvk)** (GE-Proton build, used for the DX9 compatibility route) — **zlib License** (`Licenses/DXVK-LICENSE.txt`);
- **[Vulkan Loader](https://github.com/KhronosGroup/Vulkan-Loader)** (`vulkan-1.dll`, patches documented in `vulkan/Vulkan-Loader-PATCHES.diff`) — **Apache License 2.0** (`Licenses/Vulkan-Loader-LICENSE.txt`);
- **[DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder)** (`DLSS5_Feed.fx` and ReShade add-on design) — **MIT License** (`Licenses/DLSS5_Feed-LICENSE.txt`);
- **[vort_Shaders](https://github.com/vortigern11/vort_Shaders)** (motion-vector shaders for ReShade routes) — **MIT License** (`Licenses/vort-LICENSE.txt`);
- **[Dear ImGui](https://github.com/ocornut/imgui)** (in-game NR Control UI) — **MIT License** (`Licenses/DearImGui-LICENSE.txt`);
- **[Inter Font](https://github.com/rsms/inter)** (UI font) — **SIL Open Font License 1.1** (`Licenses/Inter-LICENSE.txt`);
- **[AMD FidelityFX SDK / FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR)**, **[Intel XeSS](https://github.com/intel/xess)**, and **DirectX Agility SDK** — distributed with their original license texts under `Licenses/` (`FidelityFX_*`, `XeSS_LICENSE.txt`, `DirectX_LICENSE.txt`).

> This repository and release packages contain **no** proprietary NVIDIA binaries (`nvngx_dlssnr.dll` / `nvngx_dlssd.dll`) or extracted model weight files (`*.bin`). All model weights are extracted locally by the user from their own files.
