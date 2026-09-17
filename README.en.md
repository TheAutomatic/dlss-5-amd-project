[中文](README.md) | **English**

# OptiScaler AMD pre-SR — 1.8.5-0.3.1

**OptiScaler** plus **AMD neural rendering** (DLSS5), so **pure DLSS / XeSS games** can run neural denoise on AMD GPUs. Super-resolution is still **FFX/FSR**.

`1.8.5` = this repository; `0.3.1` = primary upstream runtime (**0.3.0 still works**). Versus 1.8.4: uninstall removes only known dependencies; depth SRV / borrowed DX11 resources, HIP search fallback, and XeFG high-ratio persistence. The neural core is unchanged; this is not a frame-rate claim.

**Wait mode: default `AmdGraphicsWait=1`.** This requests the original author runtime's 0.3.1 graphics wait (1-pixel draw). This project requests graphics only when this frame's D3D12 state snapshot and restore preparation succeed; failed admission falls back to compute. This does not provide automatic recovery from a hang, crash, or device removal after admission.

In-game, use **Ins → Graphics wait**: turning it off immediately requests classic compute. When enabling it again, the menu asks for a restart if hooks or a pass's graphics PSO are missing. If graphics mode causes problems, turn it off manually. If you cannot reach the menu, close the game, set `AmdGraphicsWait=0` under `[DlssNr]` in `OptiScaler.ini`, then launch again.

**Project home: [github.com/TheAutomatic/dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)**

(If you got this package from a mirror or cloud drive, use the repository above as the source of truth.)

> Not a reimplementation of the neural core, and not a ReShade filter.  
> Path: **game DLSS inputs → this repo → DLSSNR (0.3.1 / 0.3.0) → FFX/FSR**.

---

## Compared to predecessors

| Upstream | What they did | What this project adds |
|---|---|---|
| **[OptiScaler](https://github.com/optiscaler/OptiScaler)** | General upscaler proxy (DLSS / FFX / XeSS) | Still the install/run body |
| **[Dagherbou / OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR)** | Hooked DLSS neural rendering into OptiScaler | Inherits that OptiScaler base (`v0.2.0-dlssnr`) |
| **[wilsjo2 / OptiScaler-DLSSNR-PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)** | Neural rendering before super-resolution, multi-pass | Inherits that pre-SR architecture |
| **[Matheus / dlss-5-amd](https://github.com/MatheusGViana/dlss-5-amd-project)** | Connected pre-SR to the AMD runtime: DLSS input → AMD NR → FFX | **Adjustable NR slots** (fewer skipped frames); wired to the original author's 0.3.1 / 0.3.0; installer better at XBOX PC games |
| **[Original project / original author danielblnc](https://github.com/danielblnc/DLSS-NR-on-AMD)** | The AMD neural-rendering runtime itself | **Core untouched**; calls the original author's 0.3.1 / 0.3.0 |

### Inline NR and "slots"

Denoise (DLSS5) sits on the picture path: a frame that gets a slot must wait for its own denoise to finish before it can present. This mod keeps one buffer (a **slot**) for every denoise still running. When no slot is free, that frame is recorded with **no denoise at all** — the picture comes out faster, possibly blurrier.

**Three slots by default.** Adjustable in-game under `DLSS Neural Rendering` → `NR slots` (2–5, takes effect without a restart).

| Measured (**4K FSR Ultra Performance**, equivalent to a 720p render) | 2 slots | 3 slots |
|---|---:|---:|
| Onimusha | 19.50 ms, **0 skipped** | 19.49 ms, **0 skipped** |
| YYSLS | 19.05–19.25 ms, **many NR frames skipped** | 21.78–21.89 ms, **0 skipped** |

- On Onimusha, the 2/3-slot frame periods and display latency stayed within repeat variation; **no difference was detected**
- In the YYSLS A/B session, the runtime counter increased by about **1200 / 1440** in the two-slot segments and by 0 in the three-slot segments. Those log segments are not the same window as the 45-second PresentMon captures, so they do not yield a skip percentage  
  A separate 1→5-slot session measured **1800** in its 60-second two-slot segment and 0 at three, four and five. Counts from the two sessions are not compared with each other
- In the A/B session, 2 slots showed 47.6–47.9 ms display latency versus 62.9–63.2 ms at 3 slots; the lower figure came with many denoise skips, not equal work for free
- **4–5 slots were measured in that sweep** and were no faster than 3 in that scene. A scene heavy enough to need a fourth or fifth slot has not been measured
- Each slot is one FP16 target at the **render size** (the DLSS input) — about 29 MB when a 4K output renders at 1440p, 66 MB only at a native 4K render — and **only the selected number is allocated**
- The ini's `AmdSlots` also accepts `1` (only one denoise at a time, close to the old behaviour); the menu does not offer it
- `AmdEveryFrame` defaults to `true` and is not shown in the Ins menu; change it only in the ini (with multiple slots it no longer blocks waiting in normal play)

Early single-slot and multi-slot figures came from different capture sessions. They only show the direction of improvement after not blocking on the previous frame; they are not a precise same-session performance gain. The neural render itself did not get faster.

---

## Install

### What is in this zip

| File / folder | Purpose |
|---|---|
| `OptiScaler.dll` | This project (installed under the proxy name you pick) |
| `OptiScaler.ini` | Config template; the `[DlssNr]` section (including `AmdSlots`) lives here |
| `OptiScaler\` | FFX / XeSS / Agility dependencies |
| `Setup.bat` / `Setup.ps1` | Installer (**double-click `Setup.bat`**) |
| `Uninstall.bat` / `Uninstall.ps1` | Removes this project from the game folder (**double-click `Uninstall.bat`**); keeps backups, weights, original-author setup, and `nvngx_dlssnr.dll` |
| `Licenses\` | Third-party licences |
| `SHA256SUMS.txt` | Checksums |
| `README.md` / `README.en.md` | This document |

**Not included**: NVIDIA binaries, NR weights, the original-author installer, or the author's closed-source pass — see the next section.

### Step 1 — Files you must supply (not in this zip)

This package only contains the OptiScaler layer. There are two input routes: provide `dlssnr_on_amd_setup.exe` + `nvngx_dlssnr.dll` (recommended), or provide an already-generated `version.dll` + `dlssnr_on_amd_weights.bin`. **You do not need all four.** Put the chosen pair next to `Setup.bat` after you unzip:

| File name | What it is | Where to get it |
|---|---|---|
| `dlssnr_on_amd_setup.exe` | Original author's **0.3.1 / 0.3.0** setup | [Original project Releases](https://github.com/danielblnc/DLSS-NR-on-AMD/releases) |
| `nvngx_dlssnr.dll` | DLSS 5 neural-rendering runtime | Shipped with some recent games; or obtain online |
| (optional) ready `version.dll` / `dlssnr_on_amd_weights.bin` | Skip a setup run | Produced by a previous original-author setup run |

**Recommended: drop `dlssnr_on_amd_setup.exe` + `nvngx_dlssnr.dll`.**

If `version.dll` or `weights.bin` is still missing when you run this package's `Setup.bat`, it **launches the original-author setup** (after you pick the game folder), then continues.  
If `nvngx_dlssnr.dll` is only next to `Setup.bat` and not in the game folder, the installer **copies it into the game folder** after you pick that folder (original-author 0.3.0 looks for it there).

**Only 0.3.0 or 0.3.1 is supported.** Other versions will not run — the installer rejects them.

### Step 2 — Run the installer (recommended)

1. Unzip this release anywhere.  
2. Drop `dlssnr_on_amd_setup.exe` + `nvngx_dlssnr.dll` into that folder (next to `Setup.bat`).  
   Ready-made `version.dll` / `dlssnr_on_amd_weights.bin` files may be placed there as well.
3. **Close the game.**  
4. **Double-click `Setup.bat`** → pick the **folder that contains the game .exe**.  
5. Choose the **proxy DLL** (default `dxgi.dll`; also `winmm.dll`, `d3d12.dll`, `winhttp.dll`, `wininet.dll`, `dbghelp.dll`. **`dinput8.dll` is not supported**).  
6. If `version.dll` or `weights.bin` is still missing at this point, the installer **launches the original-author setup** to produce them, then continues.

**Asking for the game folder twice is normal — not a bug.**

The second prompt comes from **danielblnc's original setup**, which this package calls to generate `version.dll` / weights. Pick the same game folder both times.

**Game folder** = where the game exe lives (the same path you use for a normal OptiScaler install):

- Many games: `...\Win64\` or `...\Binaries\Win64\`  
- XBOX PC / some store builds: do not pick a read-only system install path — the installer will refuse it  

| Source | Installed as |
|---|---|
| This zip `OptiScaler.dll` | your chosen proxy (default `dxgi.dll`) |
| `version.dll` (0.3.1 or 0.3.0) from `dlssnr_on_amd_setup.exe` | `dlssnr_amd_pass1/2/3.dll` |
| Your `dlssnr_on_amd_weights.bin` | copied as-is |

The scripted installer does **not** leave `version.dll` in the game folder. To inject as `version.dll` itself, use the manual steps below.

On older systems where the folder picker does not open, pass the path on the command line:

```bat
Setup.bat "D:\Games\SomeGame\Binaries\Win64"
```

If an old OptiScaler or other inject DLL is already in the game folder, the installer lists it and asks: cancel / **backup and move aside** / ignore (only when it is not the proxy name you are installing). It does not silently overwrite.

### Step 3 — Enable DLSSNR in-game (scripted or manual)

1. Launch the game.  
2. Press **Insert (Ins)** to open the OptiScaler menu.  
3. Enable **DLSSNR**. The original-project version (`0.3.1` or `0.3.0`) should appear to the right of the checkbox.  
4. You now get **DLSS5 neural denoise + FFX/FSR** super-resolution.

Other OptiScaler options (hotkeys, compatibility, more FG modes): [OptiScaler Wiki](https://github.com/optiscaler/OptiScaler/wiki).

> **Optional (not part of DLSSNR):** the collapsed section below is two **external** ways to get **3x+ multi-frame generation**. None of those files ship in this zip.

<details>
<summary><strong>Optional: 3x+ multi-frame generation</strong> (Arturs / XeFG — expand)</summary>

Both paths need files you download yourself. This project ships **neither**. After editing the ini, **save and restart**. Keep `[FrameGen] External=false` (`true` turns Opti's FG off for that process). Do **not** enable both paths at once.

---

#### 1. Arturs (DLSS Enabler)

1. Get `dlss-enabler-headless.dll` from the original author (not a third-party mash-up):  
   [artur-graniszewski/DLSS-Enabler](https://github.com/artur-graniszewski/DLSS-Enabler/releases) or [Nexus Mods 757](https://www.nexusmods.com/site/mods/757)  
2. That exact filename, in the **`OptiScaler\`** folder next to the proxy / `OptiScaler.ini`.  
3. If the game already has DLSSG:

```ini
[FrameGen]
External=false
Enabled=true
FGInput=nvngxfg
FGOutput=auto
FGNvngxReplacement=Arturs
```

   If it only has upscaling, use the Wiki's `FGInput=upscaler` + `FGOutput=dlssg`.  
4. The log line `Artur's initialized` means it loaded.

Details: [OptiScaler Wiki · Frame Generation](https://github.com/optiscaler/OptiScaler/wiki) and Enabler's own docs. This project does not redistribute that DLL.

---

#### 2. XeFG (XeMFG DP4A Unlocker)

`XeFGUnlock.asi` and the matching `XeFGUnlock.ini` come from the **OptiScaler official group post "XeMFG DP4A Unlocker"**. This build loads them with the existing ASI loader — **no unlock patches in this repo, and none in the zip**. Output is **XeFG** (you still need this pack's `libxess_fg.dll` / `libxell.dll`); it is **not** NVIDIA DLSSG and **not** the Arturs path above.

1. Put both files in the game's `OptiScaler\plugins\` folder (same tree as `libxess_fg.dll`). Do not add `-loadlate`.  
2. Edit the game-root **`OptiScaler.ini`**, not the plugin ini. If the game has Streamline DLSS-FG:

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

   Leave `Path=auto` (that is `OptiScaler\plugins`). If there is no DLSS-FG, set `FGInput=upscaler`.  
   `InterpolationCount`: `1` = 2x, `2` = 3x, `3` = 4x. This build only accepts 1–3.  
3. The plugin ini (`XeFGUnlock.ini`) only holds unlock switches, e.g. `UnlockMFG=true`, `MaxInterpolatedFrames=3`. The plugin's `3` is only a cap; `InterpolationCount` in the game's `OptiScaler.ini` is the actual multiplier. For a first run set `DisableLogging=false` so `XeFGUnlock.log` appears next to the ASI.  
4. Get XeFG working at 2x first, then raise the multiplier. You can press Page Up for the frame counter, then Page Down for detail, and confirm multi-frame generation is actually on.

</details>

---

### Manual install (no Setup.bat)

`version.dll` comes from `dlssnr_on_amd_setup.exe` (original author **0.3.1 or 0.3.0**).  
Original project docs: [danielblnc/DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)

1. Run `dlssnr_on_amd_setup.exe` to obtain `version.dll` and `dlssnr_on_amd_weights.bin` (`nvngx_dlssnr.dll` is required to generate weights).  
2. Rename `version.dll` → `dlssnr_amd_pass1.dll`; copy it as `dlssnr_amd_pass2.dll` and `dlssnr_amd_pass3.dll` (pass1 is the minimum).  
3. Put `dlssnr_on_amd_weights.bin` in the same game folder.  
4. Extract **all** of this release into that folder.  
5. Rename `OptiScaler.dll` to the name you want to inject:  
   - usually `dxgi.dll` (or `winmm.dll`, etc.; **not** `dinput8.dll`)  
   - or **`version.dll`** if you want the same proxy name as the original author  
6. **Second-to-last:** remove any leftover file that would clash with your inject name (if step 5 uses `version.dll`, do not keep the original-author `version.dll` there too).  
7. In-game: **Ins** → enable **DLSSNR**.

### Uninstall

Double-click **`Uninstall.bat`**, pick the same game folder, and confirm. It removes identified OptiScaler proxies, passes, configuration, logs, and explicitly listed FFX / XeSS / Agility dependencies; it also checks `_storage_`.

**Kept**: `backup-amd-presr-*`, `nvngx_dlssnr.dll`, `dlssnr_on_amd_weights.bin`, the original-author setup/logs, non-OptiScaler files using a proxy name, and extra plugins or files you added under `OptiScaler`. Dependency directories are removed only when empty; the uninstaller never deletes the entire `OptiScaler` tree, so that folder may remain afterward.

---

## Troubleshooting / reporting a problem

If the menu will not open, DLSSNR is missing, or the image looks wrong after install, collect the following first. **Include these items when you report an issue** — otherwise it is hard to tell an install problem from a runtime problem.

### 1. Where the logs are

Logs live **next to the proxy DLL** (`dxgi.dll` / `winmm.dll`, etc.). Common files:

| File name | Written by |
|---|---|
| `OptiScaler.log` | This project's main log |
| `amd_bridge.log` | AMD bridge layer |
| `amd_presr.log` | AMD pre-SR / NR scheduling |
| `dlssnr_on_amd.log` | Original-author runtime (0.3.1 / 0.3.0) |

**XBOX PC / some store builds** may create a folder next to the game exe (often named **`_storage_`**) because of filesystem mapping.  
If you cannot find the `.log` files in the folder you installed into, look in:

```text
<folder containing the game exe>\_storage_\
```

The install files (proxy, `dlssnr_amd_pass1/2/3.dll`, weights) may also appear there — treat **the directory that actually writes the logs** as the real install location.

### 2. Files that should sit next to the proxy

Using the proxy name you chose (`dxgi.dll`, `winmm.dll`, …), the same folder should contain:

| File | Notes |
|---|---|
| Your proxy (`dxgi.dll` / `winmm.dll` / …) | This project's OptiScaler |
| `dlssnr_amd_pass1.dll` | **Required**; original-author 0.3.1 or 0.3.0 |
| `dlssnr_amd_pass2.dll`, `dlssnr_amd_pass3.dll` | Multi-pass; same bytes as pass1 |
| `dlssnr_on_amd_weights.bin` | **Required** |
| `nvngx_dlssnr.dll` | Common; some games ship it |

Do **not** leave the original-author `version.dll` next to the proxy (double injection). The scripted installer moves it aside.

### 3. In-game check

1. Launch the game and press **Ins** to open the OptiScaler menu.  
2. Confirm DLSSNR status shows **`AMD NR runtime: 0.3.x`** (0.3.1 or 0.3.0).  
3. If it says waiting / unknown runtime / that line is missing, pass or weights are usually wrong — recheck the files above.

### 4. What to include in a report

When opening an issue or asking for help, state:

1. **Proxy name**: `dxgi.dll`, `winmm.dll`, or something else?  
2. **Files next to the proxy**: pass1/2/3, weights, optional `nvngx_dlssnr.dll`; any leftover `version.dll`?  
3. **Ins menu**: does DLSSNR show `AMD NR runtime: 0.3.x`?  
4. **Logs**: `OptiScaler.log`, `amd_bridge.log`, `amd_presr.log`, `dlssnr_on_amd.log` (say the full path if they are under `_storage_`).  
5. Game name, GPU, driver version, and the symptom (no menu / no denoise / stutter / crash).

---

## Credits & license

Code chain (top to bottom): [OptiScaler](https://github.com/optiscaler/OptiScaler) → [Dagherbou](https://github.com/Dagherbou/OptiScaler_DLSSNR) → [wilsjo2](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) → [Matheus](https://github.com/MatheusGViana/dlss-5-amd-project) → **this repo**.

- [**OptiScaler**](https://github.com/optiscaler/OptiScaler) (GPL-3.0)  
- [**Dagherbou / OptiScaler_DLSSNR**](https://github.com/Dagherbou/OptiScaler_DLSSNR) (GPL-3.0) — this project's OptiScaler code is based on it (`v0.2.0-dlssnr` / commit `97376162`)  
- [**wilsjo2 / OptiScaler-DLSSNR-PreSR-Multipass**](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) — architecture source for neural rendering before super-resolution and multi-pass  
- [**Matheus / dlss-5-amd-project**](https://github.com/MatheusGViana/dlss-5-amd-project) — AMD pre-SR bridge  
- [**Original project / original author danielblnc**](https://github.com/danielblnc/DLSS-NR-on-AMD) **0.3.1 / 0.3.0** (not redistributed)  
- [**RenoDX / clshortfuse**](https://github.com/clshortfuse/renodx) (MIT) — the colour composition in `dlssnr.hlsl` is taken from their DLSS 5 neural rendering addon; full text in `Licenses/RenoDX_ATTRIBUTION.txt`  
- This project: NR slots, installer, packaging, wait mode, and the original-author wiring  

No NVIDIA binaries, original-author setup, NR weights, or author pass DLLs are included. Follow each upstream's license.
