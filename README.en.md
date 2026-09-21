[中文](README.md) | **English**

# OptiScaler AMD pre-SR — 1.9.0

Hooking **AMD Neural Rendering** (DLSS5 on AMD) into **OptiScaler**, enabling **pure DLSS / XeSS games** to run neural denoising on AMD GPUs; super-resolution is still handled by **FFX/FSR**.

This project is forked from **Matheus** and upstream repositories, maintained and evolved from their foundation.

**Project Home: [github.com/TheAutomatic/dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)**

---

## 📢 1.9.0 Changelog (Currently source update only; Release builds will be published shortly after finalizing details and installer upgrades)

1.9.0 is a **major architectural milestone upgrade**. We officially introduce the open-source [**`lmxxf` HIP neural rendering backend**](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting).

### 🚀 Core Updates

1. **Brand-New `lmxxf` Neural Rendering Backend**
   - **Embracing Open-Source Compute Core**: Alongside maintaining compatibility with the danielblnc version, we integrate an open-source HIP neural rendering backend.
   - **Same-Frame Execution Contract**: Input recording, HIP asynchronous inference, and output resource barriers are seamlessly embedded into the game's primary command queue before super-resolution (Pre-SR). Compared to upstream lmxxf, this theoretically enables genuine same-frame neural rendering (DLSS5) in modern Unreal Engine titles and games like *Where Winds Meet* that have complex GPU rendering activities after FSR.
   - **DLSS / XeSS Game Input Support**: Leveraging OptiScaler's generic proxy interception, games without native FSR support have their DLSS / XeSS buffers (Color / Motion Vectors / Depth) intercepted and fed into lmxxf neural denoising, then forwarded to FFX/FSR for super-resolution reconstruction. Pure DLSS games can now enjoy DLSS5 on AMD GPUs.
   - **Seamless Dual-Backend Compatibility**: Fully backward compatible. Users can switch back to Daniel's backend via `NrBackend=daniel` in `OptiScaler.ini` (in-game Ins menu switching coming soon).
   - **Memory & Stability Optimizations**: Optimized `fast_prefix` acceleration mode by skipping redundant 201MB noise buffer allocations, significantly reducing host memory footprint and initialization overhead; enhanced GPU LUID matching when spoofing NVIDIA (Fake NVAPI) to prevent cross-adapter crashes in multi-GPU setups.
   - **⚠️ Resolution Limitation & Recommended Presets**: Currently `lmxxf` **only supports neural rendering for pre-upscaling (render) resolutions ≤ 1080p** (1920x1080 and below). Typical preset recommendations:
     - **4K Display Output**: Recommended to use **FSR Performance mode** (1080p render) or Ultra Performance mode (720p). Using 4K Quality mode (1440p render) exceeds the current model tiling capacity.
     - **2K (1440p) Display Output**: Supported across **FSR Quality / Balanced / Performance modes** (all render at or below 1080p).
     - **1080p Display Output**: Supported on **Native 1080p** and all upscaling modes.

2. **Installer Upgrade (Coming Soon)**
   - Compatibility with lmxxf installation workflows. For manual installs, simply place `LmxxfNrRuntime.dll` and the `native-game-tiled-assets` weight folder directly into the game's executable directory; the runtime detects them automatically.

3. **Menu (Ins Menu) Purification & Native Dynamic Tuning**
   - **Smart Menu Filtering**: In `lmxxf` mode, Daniel-specific options (such as passes, slots, new wait, experimental RTGI) are automatically hidden to avoid confusion.
   - **Layout & Spacing Fixes**: Fixed a layout bug where `Enable NR` and `AMD processing` overlapped on the same line, restoring clean vertical hierarchy and spacing.
   - **Native Dynamic Tuning Sliders**: Added in-menu stepless sliders for `Detail strength` and `Colour strength`, along with `Debug view` real-time visual debugging overlays, applying changes instantly.

---

## Architecture & Background

This project serves as the **bridge layer** for AMD neural rendering. Across several in-game diagnostic rounds, the bridge’s own overhead measures around **0.01–0.03 ms** — virtually zero additional performance penalty.

`1.9.0` = Current repository version; newly supports the `lmxxf` open-source backend (Daniel `0.3.1` / `0.3.0` remains compatible via configuration).

> Not a reimplementation of the neural core, and not a ReShade filter.  
> Path: **Game DLSS/XeSS inputs → This project (Pre-SR scheduling) → DLSSNR (lmxxf / Daniel 0.3.1) → FFX/FSR super-resolution**.

---

## Standing on the Shoulders of Giants

| Upstream | What they did | What this project adds |
|---|---|---|
| **[OptiScaler](https://github.com/optiscaler/OptiScaler)** | General upscaler proxy (DLSS / FFX / XeSS) | Still the installation and execution host |
| **[Dagherbou / OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR)** → **[wilsjo2 / PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)** | First hooked DLSS neural rendering into OptiScaler, then created pre-SR multi-pass | Inherits the OptiScaler codebase and pre-SR architecture |
| **[Matheus / dlss-5-amd](https://github.com/MatheusGViana/dlss-5-amd-project)** | Connected pre-SR to AMD runtime: DLSS input → AMD NR → FFX | On that base: default **3-slot** scheduling for near every-frame NR; ~**+33%** vs old single-slot baseline in original repo **1.7.3** (33.5→44.5), eliminates ~**8.7 ms**/frame GPU stall; wired to 0.3.1 / 0.3.0; state freeze/restore for new wait; XBOX PC install improvements. Bridge overhead ~**0.01–0.03 ms** |
| **[Original project / author danielblnc](https://github.com/danielblnc/DLSS-NR-on-AMD)** | AMD neural rendering runtime core | **Core untouched**; calls original 0.3.1 / 0.3.0; adds D3D12 state freeze/restore for 0.3.1 **new wait** (including empty→empty restore of known-empty graphics state) for safe execution on DLSS/XeSS games |
| **[lmxxf / dlss5-on-amd-9070xt-porting](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting)** | Open-source HIP neural rendering runtime core | Deeply integrated into OptiScaler's Pre-SR same-frame pipeline; decomposed D3D12 bridge into 3-stage micro-scheduling for same-frame execution in demanding render pipelines; skipped 201MB redundant noise buffer; added Fake NVAPI LUID matching; implemented local weight auto-detection and real-time Ins menu tuning (Detail / Colour / Debug View) |

### Multi-Slot: Every Frame Gets NR

Denoising (DLSS5) sits on the active render path: a frame that acquires a slot must wait for its denoise to finish before presenting. This mod allocates an independent buffer (a **slot**) for each unfinished denoise job. **When all slots are full, that frame skips denoising entirely** — presenting faster, but potentially blurrier or flickery.

The Matheus branch leaned towards fewer slots / skipping frames for throughput: when NR could not keep up, some frames presented without denoise. This project defaults to multi-slot: ensuring nearly **every frame gets NR**, eliminating the single-slot post-submit GPU stall (~**MsGPUWait 8.7 ms**/frame in PresentMon).

| Config (Onimusha-class, 4K FSR Ultra Performance (≈720p render; lock-60 period)) | Median frame period | Approx. FPS | MsGPUWait | NR every frame |
|---|---:|---:|---:|---|
| Single-slot · every-frame NR (old baseline) | 29.82 ms | **33.5** | **8.69 ms** | Blocked on previous frame; low throughput |
| **This project, multi-slot default** | 22.45 ms | **44.5** (about **+33%**) | **≈ 0** | **Nearly every frame gets NR** |
| Original author 0.3 native (reference) | 22.35 ms | 44.8 | 0 | Native path does not rely on skips |

- With **nearly every frame on NR**, measured about **+33%** versus the old single-slot baseline (33.5→44.5), matching native 0.3 performance — not an artificially skip-inflated number.
- Faster due to scheduling: no idle stall waiting for the previous frame, and no full-frame denoise drops. The neural kernel itself did not get faster (`network` remains ~12–13 ms at 720p).
- Later unlocked / different-scene multi-slot Onimusha observations sit roughly in the **44–51 fps** range; the table above is the same-era pair 33.5 vs 44.5.

**How many slots in-game?** Default **3**. Adjustable via `DLSS Neural Rendering` → `NR slots` (2–5, takes effect immediately without restart). Across multiple test rounds, slot count does not increase latency; picking 5 incurs no performance penalty in theory.

| Measured (4K FSR Ultra Performance) | 2 slots | 3 slots |
|---|---:|---:|
| Onimusha | 19.50 ms, **0 skipped** | 19.49 ms, **0 skipped** |
| Where Winds Meet (WWM) | 19.05–19.25 ms, **many NR frames skipped** (faster presents, frames with no denoise) | 21.78–21.89 ms, **0 skipped** |

- In Onimusha, 2/3-slot frame periods and display latency stayed within normal measurement variance; **no difference detected**. WWM at max settings requires **≥3** slots to stay stable.
- In the WWM A/B session, the runtime skip counter increased by ~**1200 / 1440** in the 2-slot segments and was 0 in the 3-slot segments. Those log segments differ from the 45-second PresentMon captures, so they do not translate to a skip percentage.  
  A separate 1→5 slot session measured **1800** in its 60-second 2-slot segment and 0 at 3, 4, and 5 slots. Counts from the two sessions are not cross-compared.
- In the same A/B session, 2 slots showed 47.6–47.9 ms display latency versus 62.9–63.2 ms at 3 slots; the lower latency came with heavy denoise skips, not free performance.
- **4–5 slots were measured in that sweep** and were no faster than 3 in that scene. A scene heavy enough to require 4 or 5 slots has not been observed yet.
- Each slot is one FP16 render target at the **render size** (DLSS input) — ~29 MB when 4K output renders at 1440p, 66 MB only at native 4K render — and **only allocated for the selected count**.
- `AmdSlots` in the ini also accepts `1` (single active denoise, similar to old single-slot behavior); the menu does not expose this.
- `AmdEveryFrame` defaults to `true`; the Ins menu shows **Every-frame** (same row as Enable NR), and `[DlssNr] AmdEveryFrame` still works from the ini (with multiple slots it no longer stalls play).

> The ~**+33%** gain represents the scheduling improvement over the **old single-slot hard-wait baseline in original repo 1.7.3**; neural rendering itself did not speed up. Original author danielblnc’s runtime has **no frame-skipping issue** and primarily targets **games with native FSR support**. Versus Matheus, this project introduces **multi-slot scheduling** (along with install/XBOX compatibility, 0.3.1 integration, and new-wait state restore) so **DLSS / XeSS games** can maintain near-continuous NR without single-slot stalls — not merely adapting daniel 0.3.1.

---

## Installation

### What is in this package

| File / Folder | Purpose |
|---|---|
| `OptiScaler.dll` | Primary project module (renamed to chosen proxy during install) |
| `OptiScaler.ini` | Configuration template; `[DlssNr]` section (including `AmdSlots`) resides here |
| `OptiScaler\` | FFX / XeSS / Agility dependencies |
| `Setup.bat` / `Setup.ps1` | Installer (**double-click `Setup.bat`**) |
| `Uninstall_OptiScaler_NR.bat` / `.ps1` | Uninstaller; Setup copies it to the **game folder**. Double-click there: asks to preserve backups, lists files to delete, confirms Y/N |
| `LmxxfNrRuntime.dll` | lmxxf HIP neural rendering runtime core (place in game directory when using lmxxf backend) |
| `native-game-tiled-assets\` | lmxxf model weights directory (included in Full Pack, or manually placed in game folder) |
| `Licenses\` | Third-party licenses |
| `SHA256SUMS.txt` | Checksums |
| `README.md` / `README.en.md` | Documentation |

**Not included**: NVIDIA binaries, Daniel closed-source weights, and the original-author installer (if using Daniel backend, see next section).

### Step 1 — Files You Must Supply (Not in this zip)

This package contains only the OptiScaler layer. There are two preparation methods: drop `dlssnr_on_amd_setup.exe` + `nvngx_dlssnr.dll` (recommended), or supply pre-generated `version.dll` + `dlssnr_on_amd_weights.bin`. **You do not need all four.** Place them next to `Setup.bat` after unzipping:

| File Name | What It Is | Where to Get It |
|---|---|---|
| `dlssnr_on_amd_setup.exe` | Original author's **0.3.1 / 0.3.0** setup | [Original Project Releases](https://github.com/danielblnc/DLSS-NR-on-AMD/releases) |
| `nvngx_dlssnr.dll` | DLSS 5 neural rendering runtime | Bundled with some recent games; or obtain online |
| (Optional) ready `version.dll` / `dlssnr_on_amd_weights.bin` | Skip generating manually | Produced by a previous original-author setup run |

**Recommended: Drop `dlssnr_on_amd_setup.exe` + `nvngx_dlssnr.dll`.**

If `version.dll` or `weights.bin` is missing when you run `Setup.bat`, it **automatically launches the original-author setup** to generate them (after you select the game directory), then proceeds.  
If `nvngx_dlssnr.dll` is only in the setup directory and not in the game directory, the installer **copies it into the game directory** automatically (original-author 0.3.0 looks for it there).

**Only 0.3.0 / 0.3.1 supported.** Other versions will not run and will be rejected by the installer.

### Step 2 — Run the Installer (Recommended)

1. Extract this release to any directory.  
2. Place `dlssnr_on_amd_setup.exe` and `nvngx_dlssnr.dll` into that directory (next to `Setup.bat`).  
   Existing `version.dll` / `dlssnr_on_amd_weights.bin` can also be placed here.  
3. **Ensure the game is closed.**  
4. **Double-click `Setup.bat`** → A **folder selection dialog** opens → Select the **folder containing the game executable** → OK.  
5. Follow prompts to select your **proxy DLL** (default `dxgi.dll`; also supports `winmm.dll`, `d3d12.dll`, `winhttp.dll`, `wininet.dll`, `dbghelp.dll`. **`dinput8.dll` is not supported**).  
6. If `version.dll` or `weights.bin` is missing, the installer **automatically launches the original setup** to generate them, then finishes installation.

**Being asked for the game directory twice is normal — not a bug.**

The second prompt comes from **danielblnc's original setup tool**, which this installer invokes to generate `version.dll` / weights. Select the same game folder both times.

**Game folder** refers to the directory where the game executable resides (standard OptiScaler install path):

- Many games use `...\Win64\` or `...\Binaries\Win64\`  
- For XBOX PC / Microsoft Store builds, do not select read-only system directories — the installer will reject them and request a writable folder.

The installer performs the following:

| Source | Installed As |
|---|---|
| This zip `OptiScaler.dll` | Your chosen proxy name (default `dxgi.dll`) |
| `version.dll` (0.3.1 or 0.3.0) from `dlssnr_on_amd_setup.exe` | `dlssnr_amd_pass1.dll`, `dlssnr_amd_pass2.dll`, `dlssnr_amd_pass3.dll` |
| Your `dlssnr_on_amd_weights.bin` | Copied as-is |

The automated installer **does not** leave `version.dll` in the game folder (avoiding conflicts). If you wish to inject using `version.dll`, follow the manual install steps below.

On older systems where folder dialogs fail to open, you can pass the path via command line:

```bat
Setup.bat "D:\Games\SomeGame\Binaries\Win64"
```

If an older OptiScaler or conflicting inject DLL exists in the game folder, the installer lists it and offers choices: cancel / **backup and move aside** / ignore (only if not clashing with target proxy). It never silently overwrites.

### Step 3 — Enable DLSSNR In-Game (Scripted or Manual)

1. Launch the game.  
2. Press **Insert (Ins)** to open the OptiScaler menu.  
3. Under **DLSS Neural Rendering**, check **Enable NR** (AMD Neural Rendering). The same row should display the active runtime version, such as `0.3.1`, `0.3.0`, or `lmxxf`.  
4. The render pipeline is now running **DLSS5 neural denoise + FFX/FSR super-resolution**.

For other OptiScaler features (hotkeys, compatibility, advanced FG options), see the [**OptiScaler Wiki**](https://github.com/optiscaler/OptiScaler/wiki).

> **Optional (unrelated to DLSSNR):** The collapsed section below details two external options for **3x+ multi-frame generation**. These files are not bundled with this release.

<details>
<summary><strong>Optional: 3x+ Multi-Frame Generation</strong> (Arturs / XeFG, click to expand)</summary>

Both options require downloading external files. This project **does not bundle either**. After modifying ini settings, **save and restart the game**. Keep `[FrameGen] External=false` (`true` disables Opti's FG). **Do not** enable both options simultaneously.

---

#### 1. Arturs (DLSS Enabler)

1. Obtain `dlss-enabler-headless.dll` from the author (avoid third-party repacks):  
   [artur-graniszewski/DLSS-Enabler](https://github.com/artur-graniszewski/DLSS-Enabler/releases) or [Nexus Mods 757](https://www.nexusmods.com/site/mods/757)  
2. The file must retain this exact name and be placed in the **`OptiScaler\`** subfolder next to the proxy / `OptiScaler.ini`.  
3. When the game **already has DLSSG**:

```ini
[FrameGen]
External=false
Enabled=true
FGInput=nvngxfg
FGOutput=auto
FGNvngxReplacement=Arturs
```

   If the game only has upscaling without DLSSG, use the Wiki config: `FGInput=upscaler` + `FGOutput=dlssg`.  
4. Successful loading is confirmed when the log contains `Artur's initialized`.

Refer to [OptiScaler Wiki · Frame Generation](https://github.com/optiscaler/OptiScaler/wiki) and the Enabler author's instructions. This project does not redistribute the DLL.

---

#### 2. XeFG (XeMFG DP4A Unlocker)

`XeFGUnlock.asi` and the accompanying `XeFGUnlock.ini` originate from the **OptiScaler official community "XeMFG DP4A Unlocker" post**. This project does not bundle these files; please acquire them independently.

1. Place both files in the game's `OptiScaler\plugins\` directory (same hierarchy as `libxess_fg.dll`). Do not add `-loadlate`.  
2. Edit **`OptiScaler.ini`** in the game root, NOT the plugin ini in `plugins\`. If the game **already features Streamline DLSS-FG**:

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

   `Path` can remain `auto` (defaulting to `OptiScaler\plugins`). If DLSS-FG is absent, change `FGInput` to `upscaler`.  
   `InterpolationCount`: `1` = 2x, `2` = 3x, and so on. This package no longer hard-caps the multiplier at 3 (4x). The achievable multiplier depends on Intel XeFG hardware limits and plugin capability.  
3. The plugin ini (`XeFGUnlock.ini`) only defines unlock switches, e.g. `UnlockMFG=true`, `MaxInterpolatedFrames=3`. The number in the plugin is the unlock limit; `InterpolationCount` in the game's `OptiScaler.ini` is the actual multiplier. Both must match your target, and XeFG has its own hardware limit. For the first run, setting `DisableLogging=false` generates `XeFGUnlock.log` alongside it.  
4. We recommend verifying XeFG at 2x first before raising multipliers. Press Page Up to display frame rates, then Page Down to switch detail views and confirm multi-frame generation is operational.

</details>

---

### Manual Installation (Without Setup.bat)

Suitable for users familiar with manually deploying DLLs into game folders. `version.dll` comes from `dlssnr_on_amd_setup.exe` (original author **0.3.1 or 0.3.0**).  
Original project reference: [danielblnc/DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)

1. Run `dlssnr_on_amd_setup.exe` to obtain `version.dll` and `dlssnr_on_amd_weights.bin` (`nvngx_dlssnr.dll` is required to generate weights).  
2. **Rename** `version.dll` to `dlssnr_amd_pass1.dll`, then copy it twice as `dlssnr_amd_pass2.dll` and `dlssnr_amd_pass3.dll` (identical content; pass1 is the minimum requirement).  
3. Place `dlssnr_on_amd_weights.bin` in the same game directory.  
4. Extract **all** contents of this release into that game directory.  
5. **Rename** `OptiScaler.dll` to your target injection name:  
   - Common: `dxgi.dll` (or `winmm.dll`, etc.; **do not use `dinput8.dll`**)  
   - Or directly rename to **`version.dll`** (convenient when matching the original author's proxy name)  
6. **Important Check**: Ensure no conflicting, leftover `version.dll` or old proxy DLLs remain in the game directory (if you renamed OptiScaler to `version.dll` in step 5, do not keep the original author's `version.dll`).  
7. Launch the game, press **Ins** to open the menu, and check **Enable NR** under **DLSS Neural Rendering**.

### Uninstallation

In the **game folder**, double-click **`Uninstall_OptiScaler_NR.bat`** (Setup copies it there). Do not run it from the zip extraction directory. If older `backup-amd-presr-*` folders exist, it first asks whether to keep them (Y = keep / N = delete); it then lists files and folders slated for deletion and asks for **Y or N** confirmation (case-insensitive). It also cleans up `_storage_`.

**Retained by default**: `nvngx_dlssnr.dll`, `dlssnr_on_amd_weights.bin`, original-author setup and logs, non-OptiScaler proxy DLLs with matching names, and extra plugins/files added to `OptiScaler`. `backup-amd-presr-*` follows your prompt choice. Only empty dependency directories are removed; the uninstaller will not delete the entire `OptiScaler` directory if other files exist.

---

## Troubleshooting / Reporting Issues

If the menu fails to open, DLSSNR is missing, or visuals appear degraded after installation, collect the following information first. **Please include these details when filing an issue**, as it is otherwise difficult to distinguish an installation problem from a runtime issue.

### 1. Identify Log Locations

Logs reside in the **same directory as the proxy DLL** (`dxgi.dll` / `winmm.dll`, etc.). Common files:

| File Name | Generated By |
|---|---|
| `OptiScaler.log` | Main project log |
| `amd_bridge.log` | AMD bridge layer |
| `amd_presr.log` | AMD pre-SR / NR scheduling |
| `dlssnr_on_amd.log` | Original author runtime (0.3.1 / 0.3.0) |

**XBOX PC / Microsoft Store games** may utilize filesystem virtualization, creating an adjacent folder named **`_storage_`**.  
If `.log` files are missing in your target directory, check:

```text
<Game_Executable_Directory>\_storage_\
```

Installation files (proxy, `dlssnr_amd_pass1/2/3.dll`, weights) may also reside there — **the directory actively writing logs is the true working directory**.

### 2. Verify Proxy Directory Files

Based on your chosen proxy name (`dxgi.dll` or `winmm.dll`), the following should be present:

| File | Description |
|---|---|
| Your chosen proxy (`dxgi.dll` / `winmm.dll` / …) | This project's OptiScaler |
| `dlssnr_amd_pass1.dll` | **Required** (for Daniel backend); original 0.3.1 or 0.3.0 |
| `dlssnr_amd_pass2.dll`, `dlssnr_amd_pass3.dll` | Multi-pass (for Daniel backend); duplicate of pass1 |
| `dlssnr_on_amd_weights.bin` | **Required** (for Daniel backend) |
| `LmxxfNrRuntime.dll` | **Required** (for lmxxf backend) |
| `native-game-tiled-assets\` | **Required** (for lmxxf backend; weights directory) |
| `nvngx_dlssnr.dll` | Common; bundled with some games |

**Do not** keep the original author's `version.dll` alongside the proxy (causes double injection). The automated installer moves it away.

### 3. In-Game Self-Check

1. Launch the game and press **Ins** to open the OptiScaler menu.  
2. Verify that the NR status displays: **`AMD NR runtime: 0.3.x`** (0.3.1 or 0.3.0) or **`lmxxf`**.  
3. If it displays "waiting", uninitialized, or the line is absent, it typically indicates incorrect pass/runtime or weights paths. Recheck the files listed above.

### 4. What to Include When Reporting

Please specify in your issue or feedback:

1. **Proxy Name**: `dxgi.dll`, `winmm.dll`, or other?  
2. **Directory Contents**: Are pass1/2/3 (or `LmxxfNrRuntime.dll`), weights (or `native-game-tiled-assets`), and optional `nvngx_dlssnr.dll` present? Is there a leftover `version.dll`?  
3. **Ins Menu**: Does NR status display `AMD NR runtime: 0.3.x` or `lmxxf`?  
4. **Logs**: `OptiScaler.log`, `amd_bridge.log`, `amd_presr.log`, `dlssnr_on_amd.log` (specify if located in `_storage_`).  
5. Game title, GPU model, driver version, and issue symptoms (menu won't open / no denoising / stuttering / crash).

---

## Credits & Licensing

Code lineage (top to bottom): [OptiScaler](https://github.com/optiscaler/OptiScaler) → [Dagherbou](https://github.com/Dagherbou/OptiScaler_DLSSNR) → [wilsjo2](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) → [Matheus](https://github.com/MatheusGViana/dlss-5-amd-project) → **This Repository**.

- [**OptiScaler**](https://github.com/optiscaler/OptiScaler) (GPL-3.0)  
- [**Dagherbou / OptiScaler_DLSSNR**](https://github.com/Dagherbou/OptiScaler_DLSSNR) (GPL-3.0) — This project's OptiScaler codebase is built on this fork (`v0.2.0-dlssnr` / commit `97376162`)  
- [**wilsjo2 / OptiScaler-DLSSNR-PreSR-Multipass**](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) — Architectural origin of pre-SR neural rendering and multi-pass scheduling  
- [**Matheus / dlss-5-amd-project**](https://github.com/MatheusGViana/dlss-5-amd-project) — AMD pre-SR bridge  
- [**Original project / author danielblnc**](https://github.com/danielblnc/DLSS-NR-on-AMD) **0.3.1 / 0.3.0** (not bundled in this package)  
- [**lmxxf / dlss5-on-amd-9070xt-porting**](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) — Open-source HIP neural rendering runtime and compute core  
- [**RenoDX / clshortfuse**](https://github.com/clshortfuse/renodx) (MIT) — Color composition in `dlssnr.hlsl` is derived from their DLSS 5 neural rendering addon (full text in `Licenses/RenoDX_ATTRIBUTION.txt`)  
- This project: NR slots, 0.3.1 integration, new-wait state freeze/restore, deep lmxxf HIP runtime integration with Pre-SR same-frame pipeline restructuring, installer, and packaging  

This package does not contain NVIDIA binaries, original-author setup tools, Daniel NR weights, or upstream closed-source passes. Please comply with all respective upstream licenses.
