[中文](README.md) | **English**

# OptiScaler AMD pre-SR — 1.8.2-0.3.0

**OptiScaler** + **AMD neural rendering** so **pure-DLSS games** can run neural denoise on AMD GPUs. Super-resolution is **FFX/FSR**.

`1.8.2` = this repository; `0.3.0` = required upstream runtime.

> Not a reimplementation of the neural core, and not a ReShade filter.  
> Path: **game DLSS inputs → this repo → DLSSNR (0.3.0) → FFX/FSR**.

---

## Compared to predecessors

| Upstream | What they did | What this project adds |
|---|---|---|
| **[OptiScaler](https://github.com/optiscaler/OptiScaler)** | General upscaler proxy | Still the install/run vehicle |
| **[dlss-5-amd (Matheus)](https://github.com/MatheusGViana/dlss-5-amd-project)** | AMD pre-SR bridge | **Adjustable NR slots**: no more skip-on-busy; generic folder picker in the installer |
| **[DLSS-NR on AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)** (below: original project; danielblnc = original author) | AMD neural runtime | **Core untouched**; original author’s 0.3.0 |

### Every-frame NR and "slots"

NR is inline: the game waits for its own denoise before it can present. This mod keeps one buffer
(a **slot**) per denoise still in flight. When a frame finds every slot busy it is recorded with
**no denoise at all** — faster, and visibly worse.

**Three slots by default.** Adjustable in-game under `DLSS Neural Rendering` → `NR slots`
(2-5, takes effect without a restart).

| Measured (720p render, 60 lock; slots flipped **inside one session** so scene and GPU state are fixed) | 2 slots | 3 slots |
|---|---:|---:|
| Onimusha (light) | 19.50 ms, **0 skipped** | 19.49 ms, **0 skipped** |
| YYSLS (heavy) | 19.25 ms, **31.8% of frames undenoised** | 21.85 ms, **0 skipped** |

- On Onimusha 3 slots are **indistinguishable from 2** (0.05% frame rate, same display latency),
  so the default costs nothing there
- On a heavy scene 2 slots drop nearly a third of the denoise; 3 slots give **29% more denoised
  frames per second** (35.6 -> 45.8)
- 2 slots do show lower display latency on that scene (47.9 vs 63.2 ms) — that is the third of the
  denoising being skipped, not a free win
- **4-5 slots** are headroom for a scene heavier than this. **Not measured**, and not known to be
  faster. Each slot is one FP16 target at the **render size** (the DLSS input) — about 29 MB when a
  4K output renders at 1440p, 66 MB only at a native 4K render — and **only the selected number is
  allocated**
- The ini's `AmdSlots` also accepts `1` (the old one-frame-outstanding path); the menu does not
  offer it

The earlier single-slot measurement (~33.5 fps vs ~44.6 fps dual-slot) came from **no longer
blocking the recording thread on the previous NR job**, not from a faster neural core.

---

## Install

### Step 1 — Files you must supply (not in this zip)

This package only contains the OptiScaler layer. Put these next to `Setup.bat` after you unzip — **without them DLSS5 cannot be enabled**:

| File name | What it is | Where to get it |
|---|---|---|
| `dlssnr_on_amd_setup.exe` | Original author’s **0.3.0** setup | [Original project 0.3.0 Release](https://github.com/danielblnc/DLSS-NR-on-AMD/releases) |
| `nvngx_dlssnr.dll` | DLSS 5 neural-rendering runtime | Shipped with some games (e.g. certain NBA 2K builds); or obtain online |
| (optional) ready `version.dll` / `dlssnr_on_amd_weights.bin` | Skip a setup run | Produced by a previous original-author setup run |

**Recommended: only drop `dlssnr_on_amd_setup.exe` + `nvngx_dlssnr.dll`.**  
If `version.dll` or `weights.bin` is still missing when you run this package’s `Setup.bat`, it **launches the original-author setup** (after you pick the game folder), then continues.  
If `nvngx_dlssnr.dll` is only next to `Setup.bat` and not in the game folder, the installer **copies it into the game folder** after you pick that folder (original-author 0.3.0 looks for it there).

**Only 0.3.0 is supported.**

### Step 2 — Run the installer (recommended)

1. Unzip this release anywhere.  
2. Drop `dlssnr_on_amd_setup.exe` + `nvngx_dlssnr.dll` into that folder (next to `Setup.bat`).  
3. **Close the game.**  
4. **Double-click `Setup.bat`** → pick the **folder that contains the game .exe**.  
5. Choose the **proxy DLL** (default `dxgi.dll`; also `winmm.dll`, `d3d12.dll`, `winhttp.dll`, `wininet.dll`, `dbghelp.dll`. **`dinput8.dll` is not supported**).  
6. If `version.dll` or `weights.bin` is still missing at this point, the installer **launches the original-author setup** to produce them, then continues.

**Game folder** = where the game exe lives (the same path you use for a normal OptiScaler install):

- Many games: `...\Win64\` or `...\Binaries\Win64\`  
- Store builds: do not pick a read-only system install path — the installer will refuse it  

| Source | Installed as |
|---|---|
| This zip `OptiScaler.dll` | your chosen proxy (default `dxgi.dll`) |
| `version.dll` (0.3.0) from `dlssnr_on_amd_setup.exe` | `dlssnr_amd_pass1/2/3.dll` |
| Your `dlssnr_on_amd_weights.bin` | copied as-is |

The scripted installer does **not** leave `version.dll` in the game folder. To inject as `version.dll` itself, use the manual steps below.

```bat
Setup.bat "D:\Games\SomeGame\Binaries\Win64"
```

If an old OptiScaler or other inject DLL is already in the game folder, the installer lists it and asks: cancel / **backup and move aside** / ignore (only when it is not the proxy name you are installing). It does not silently overwrite.

### Step 3 — Enable DLSSNR in-game (scripted or manual)

1. Launch the game.  
2. Press **Insert (Ins)** to open the OptiScaler menu.  
3. Enable **DLSSNR**.  
4. You now get **DLSS5-style neural denoise + FFX/FSR** super-resolution.

More OptiScaler options: [OptiScaler Wiki](https://github.com/optiscaler/OptiScaler/wiki).

---

### Manual install (no Setup.bat)

`version.dll` comes from `dlssnr_on_amd_setup.exe` (original author 0.3.0).  
Original project docs: [danielblnc/DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)

1. Run `dlssnr_on_amd_setup.exe` to obtain 0.3.0 `version.dll` and `dlssnr_on_amd_weights.bin` (`nvngx_dlssnr.dll` is required to generate weights).  
2. Rename `version.dll` → `dlssnr_amd_pass1.dll`; copy it as `dlssnr_amd_pass2.dll` and `dlssnr_amd_pass3.dll` (pass1 is the minimum).  
3. Put `dlssnr_on_amd_weights.bin` in the same game folder.  
4. Extract **all** of this release into that folder.  
5. Rename `OptiScaler.dll` to the name you want to inject:  
   - usually `dxgi.dll` (or `winmm.dll`, etc.; **not** `dinput8.dll`)  
   - or **`version.dll`** if you want the same proxy name as the original author  
6. **Second-to-last:** remove any leftover file that would clash with your inject name (if step 5 uses `version.dll`, do not keep the original-author `version.dll` there too).  
7. In-game: **Ins** → enable **DLSSNR**.

---

## Credits & license

- [**OptiScaler**](https://github.com/optiscaler/OptiScaler) (GPL-3.0)  
- [**MatheusGViana/dlss-5-amd-project**](https://github.com/MatheusGViana/dlss-5-amd-project)  
- [**Original project / original author danielblnc**](https://github.com/danielblnc/DLSS-NR-on-AMD) **0.3.0** (not redistributed)  
- [**RenoDX / clshortfuse**](https://github.com/clshortfuse/renodx) (MIT) — the colour composition in `dlssnr.hlsl` is taken from their DLSS 5 neural rendering addon; full text in `Licenses/RenoDX_ATTRIBUTION.txt`  
- This project: NR slots, installer, packaging  

No NVIDIA binaries, NR weights, or author pass DLL are included. Follow each upstream’s license.
