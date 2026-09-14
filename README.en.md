[中文](README.md) | **English**

# OptiScaler AMD pre-SR — 1.8.0-0.3.0

**OptiScaler** + **AMD neural rendering (DLSS-NR on AMD)** so **pure-DLSS games** can run neural denoise on AMD GPUs. Super-resolution is **FFX/FSR**.

`1.8.0` = this repository; `0.3.0` = required upstream NR runtime.

> Not a reimplementation of the NR core, and not a ReShade filter.  
> Path: **game DLSS inputs → this repo → NR (0.3.0) → FFX/FSR**.

---

## Compared to predecessors

| Upstream | What they did | What this repo adds |
|---|---|---|
| **[OptiScaler](https://github.com/optiscaler/OptiScaler)** | General upscaler proxy (DLSS / FFX / XeSS) | Still the install/run vehicle |
| **[dlss-5-amd (Matheus)](https://github.com/MatheusGViana/dlss-5-amd-project)** | AMD pre-SR: DLSS inputs → AMD NR → FFX | **Dual-slot every-frame** (stop skipping when busy; keep the GPU fed); **Xbox PC install path fix** (the original installer wrote beside the exe and often hit `WindowsApps` → permission errors; this installer targets the **game exe directory** and probes writability) |
| **[DLSS-NR on AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)** | The AMD NR runtime itself | **Core untouched**; used as 0.3.0 |

### Dual-slot every-frame (vs skip-on-busy)

| Config (Onimusha 720p, every-frame NR) | Median period | ~fps | GPU Wait |
|---|---:|---:|---:|
| Single-slot / skip-on-busy | ~29.7 ms | ~33.5 | ~8.7 ms |
| **This repo, dual-slot every-frame** | ~22.4 ms | **~44.6** | **~0** |
| Native 0.3 (reference) | ~22.2 ms | ~45.0 | 0 |

The win is **not blocking record until the previous NR job retires**, so the GPU stays busy — not a faster NR core. Lighter scenes can hit ~49 fps median. Rare 2s+ stalls can still be `SPIKE` jobs inside the NR runtime log.

---

## Install

### You must supply (not in this package)

1. **Only** [DLSS-NR on AMD **0.3.0** Release](https://github.com/danielblnc/DLSS-NR-on-AMD/releases)  
2. Local **weights** (`dlssnr_on_amd_weights.bin`, or upstream setup + `nvngx_dlss.dll` from **your** game)  
3. NVIDIA-related files **only from your own game**

### Scripted install

1. Unzip this release.  
2. Drop these into the **same folder as `Setup.bat`**:  
   - `version.dll` (0.3.0)  
   - `dlssnr_on_amd_weights.bin` (or setup + `nvngx_dlss.dll`)  
3. Close the game, then run (**argument is the directory that contains the game exe**):

```bat
Setup.bat "path-to-game-folder"
```

**Game folder** = the directory with the main 64-bit exe (where you would put `dxgi.dll` for a normal OptiScaler install).

- Onimusha (Xbox PC): `C:\XboxGames\Onimusha- Way of the Sword\Content`  
- Many games use `...\Win64\` or `...\WinGDK\` — **use wherever the exe actually is**.

The installer: `OptiScaler.dll` → your chosen proxy (default `dxgi.dll`); `version.dll` → `dlssnr_amd_pass1/2/3.dll`.  
It does **not** leave `version.dll` in the game folder.

### Manual install

1. Put **0.3.0 `version.dll`** in the game folder, then:  
   - **Rename** it to `dlssnr_amd_pass1.dll`  
   - **Copy** that file twice as `dlssnr_amd_pass2.dll` and `dlssnr_amd_pass3.dll` (same bytes; pass2/3 only if you want multi-layer NR — **pass1 is the minimum**)  
2. Confirm there is **no leftover `version.dll`** in the game folder (if you installed 0.3.0 under another proxy name, make sure you do not also inject the same name as step 4).  
3. Extract **all** of this release into the same game folder.  
4. **Rename** `OptiScaler.dll` to the proxy you inject, e.g. `dxgi.dll` (or `winmm.dll`, etc.).  
5. Enable **DlssNr** in the menu when you want NR.

For other OptiScaler usage (**frame generation**, menu shortcuts, compatibility, etc.):  
[**OptiScaler Wiki**](https://github.com/optiscaler/OptiScaler/wiki).

---

## Credits & license

- [**OptiScaler**](https://github.com/optiscaler/OptiScaler) (GPL-3.0)  
- [**MatheusGViana/dlss-5-amd-project**](https://github.com/MatheusGViana/dlss-5-amd-project)  
- [**danielblnc/DLSS-NR-on-AMD**](https://github.com/danielblnc/DLSS-NR-on-AMD) **0.3.0** (not redistributed)  
- This repo: dual-slot every-frame, installer, packaging  

No NVIDIA binaries, NR weights, or upstream closed-source pass DLL are included. Follow each upstream’s license.
