[中文](README.md) | **English**

# OptiScaler AMD pre-SR — 1.8.3-0.3.1

**OptiScaler** + **AMD neural rendering** so **pure-DLSS games** can run neural denoise on AMD GPUs. Super-resolution is **FFX/FSR**.

`1.8.3` = this repository; `0.3.1` = primary upstream runtime (**0.3.0 still works**).

**Wait mode (feature branch): default `AmdGraphicsWait=1`.** This requests the author runtime's 0.3.1 graphics wait (1-pixel draws). The host only allows it when this frame's state snapshot succeeded; otherwise that frame stays on compute spin. The `main` release line still forces compute. Do not call graphics "stable" on untested games.

**Project home: [github.com/TheAutomatic/dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)**

(If you got this package from a mirror or cloud drive, use the repository above as the source of truth.)

> Not a reimplementation of the neural core, and not a ReShade filter.  
> Path: **game DLSS inputs → this repo → DLSSNR (0.3.1 / 0.3.0) → FFX/FSR**.

---

## Compared to predecessors

| Upstream | What they did | What this project adds |
|---|---|---|
| **[OptiScaler](https://github.com/optiscaler/OptiScaler)** | General upscaler proxy | Still the install/run vehicle |
| **[dlss-5-amd (Matheus)](https://github.com/MatheusGViana/dlss-5-amd-project)** | AMD pre-SR bridge | **Adjustable NR slots**: more in-flight buffers reduce busy-frame skips; generic folder picker in the installer |
| **[DLSS-NR on AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)** (below: original project; danielblnc = original author) | AMD neural runtime | **Core untouched**; original author’s 0.3.1 / 0.3.0 |

### Inline NR and "slots"

NR is inline: a frame that obtains a slot waits for its own denoise before it can present. This mod keeps one buffer
(a **slot**) per denoise still in flight. When a frame finds every slot busy it is recorded with
**no denoise at all** — faster, with a possible image-quality loss.

**Three slots by default.** Adjustable in-game under `DLSS Neural Rendering` → `NR slots`
(2-5, takes effect without a restart).

| Measured (test condition only; the project is not limited to 720p: **4K FSR Ultra Performance**, equivalent to a 720p render, 60 lock; slots flipped **inside one session at one standing position**) | 2 slots | 3 slots |
|---|---:|---:|
| Onimusha (light) | 19.50 ms, **0 skipped** | 19.49 ms, **0 skipped** |
| YYSLS (heavy) | 19.05-19.25 ms, **about 1200-1440 frames undenoised per segment** | 21.78-21.89 ms, **0 skipped** |

- On Onimusha, the 2/3-slot frame periods and display latency stayed within repeat variation;
  **no difference was detected**
- In the YYSLS A/B session, the runtime counter increased by about **1200 / 1440** in the two-slot
  segments and by 0 in the three-slot segments. Those log segments are not the same window as the
  45-second PresentMon captures, so they do not yield a skip percentage
- A separate 1-to-5-slot session measured **1800** in its 60-second two-slot segment and 0 at three,
  four and five. Counts from the two sessions are not compared with each other
- In the A/B session, 2 slots showed 47.6-47.9 ms display latency versus 62.9-63.2 ms at 3 slots;
  the lower figure came with many denoise skips, not equal work for free
- **4-5 slots were measured in that sweep** and were no faster than 3 in that scene. A scene heavy
  enough to require a fourth or fifth slot has not been measured
- Each slot is one FP16 target at the **render size** (the DLSS input) — about 29 MB when a 4K
  output renders at 1440p, 66 MB only at a native 4K render — and **only the selected number is
  allocated**
- The ini's `AmdSlots` also accepts `1` (the old one-frame-outstanding path); the menu does not
  offer it

The early single-slot and dual-slot figures came from separate capture sessions. They show the
direction of improvement after removing the previous-job retirement block, not a precise in-session
performance gain; the neural core itself did not become faster.

---

## Install

### What is in this zip

| File / folder | Purpose |
|---|---|
| `OptiScaler.dll` | This project (installed under the proxy name you pick) |
| `OptiScaler.ini` | Config template; the `[DlssNr]` section (including `AmdSlots`) lives here |
| `OptiScaler\` | FFX / XeSS / Agility dependencies |
| `Setup.bat` / `Setup.ps1` | Installer (**double-click `Setup.bat`**) |
| `Licenses\` | Third-party licences |
| `SHA256SUMS.txt` | Checksums |
| `README.md` / `README.en.md` | This document |

**Not included**: NVIDIA binaries, the original-author setup, NR weights, or the author's closed-source pass - see the next section.

### Step 1 — Files you must supply (not in this zip)

This package only contains the OptiScaler layer. There are two input routes: provide `dlssnr_on_amd_setup.exe` + `nvngx_dlssnr.dll` (recommended), or provide an already-generated `version.dll` + `dlssnr_on_amd_weights.bin`. **You do not need all four.** Put the chosen pair next to `Setup.bat` after you unzip:

| File name | What it is | Where to get it |
|---|---|---|
| `dlssnr_on_amd_setup.exe` | Original author’s **0.3.1 / 0.3.0** setup | [Original project Releases](https://github.com/danielblnc/DLSS-NR-on-AMD/releases) |
| `nvngx_dlssnr.dll` | DLSS 5 neural-rendering runtime | Shipped with some recent games; or obtain online |
| (optional) ready `version.dll` / `dlssnr_on_amd_weights.bin` | Skip a setup run | Produced by a previous original-author setup run |

**Recommended: drop `dlssnr_on_amd_setup.exe` + `nvngx_dlssnr.dll`.**

If `version.dll` or `weights.bin` is still missing when you run this package’s `Setup.bat`, it **launches the original-author setup** (after you pick the game folder), then continues.  
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

The second prompt comes from **danielblnc’s original 0.3.0 setup**, which this package calls to generate `version.dll` / weights. Pick the same game folder both times.

**Game folder** = where the game exe lives (the same path you use for a normal OptiScaler install):

- Many games: `...\Win64\` or `...\Binaries\Win64\`  
- Store builds: do not pick a read-only system install path — the installer will refuse it  

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
4. You now get **DLSS5-style neural denoise + FFX/FSR** super-resolution (**compute wait**).

More OptiScaler options: [OptiScaler Wiki](https://github.com/optiscaler/OptiScaler/wiki).

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

---

## Troubleshooting / reporting a problem

If the menu will not open, DLSSNR is missing, or the image looks wrong after install, collect the following first. **Include these items when you report an issue** — otherwise it is hard to tell an install problem from a runtime problem.

### 1. Where the logs are

Logs live **next to the proxy DLL** (`dxgi.dll` / `winmm.dll`, etc.). Common files:

| File name | Written by |
|---|---|
| `OptiScaler.log` | This project’s main log |
| `amd_bridge.log` | AMD bridge layer |
| `amd_presr.log` | AMD pre-SR / NR scheduling |
| `dlssnr_on_amd.log` | Original-author runtime (0.3.1 / 0.3.0) |

**Xbox PC / some store builds** may create a folder next to the game exe (often named **`_storage_`**) because of filesystem mapping.  
If you cannot find the `.log` files in the folder you installed into, look in:

```text
<folder containing the game exe>\_storage_\
```

The install files (proxy, `dlssnr_amd_pass1/2/3.dll`, weights) may also appear there — treat **the directory that actually writes the logs** as the real install location.

### 2. Files that should sit next to the proxy

Using the proxy name you chose (`dxgi.dll`, `winmm.dll`, …), the same folder should contain:

| File | Notes |
|---|---|
| Your proxy (`dxgi.dll` / `winmm.dll` / …) | This project’s OptiScaler |
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

- [**OptiScaler**](https://github.com/optiscaler/OptiScaler) (GPL-3.0)  
- [**Dagherbou/OptiScaler_DLSSNR**](https://github.com/Dagherbou/OptiScaler_DLSSNR) (GPL-3.0) — this project's OptiScaler code is based on it (`v0.2.0-dlssnr` / commit `97376162`)
- [**MatheusGViana/dlss-5-amd-project**](https://github.com/MatheusGViana/dlss-5-amd-project)  
- [**Original project / original author danielblnc**](https://github.com/danielblnc/DLSS-NR-on-AMD) **0.3.1 / 0.3.0** (not redistributed)  
- [**RenoDX / clshortfuse**](https://github.com/clshortfuse/renodx) (MIT) — the colour composition in `dlssnr.hlsl` is taken from their DLSS 5 neural rendering addon; full text in `Licenses/RenoDX_ATTRIBUTION.txt`  
- This project: NR slots, installer, packaging  

No NVIDIA binaries, original-author setup, NR weights, or author pass DLLs are included. Follow each upstream’s license.
