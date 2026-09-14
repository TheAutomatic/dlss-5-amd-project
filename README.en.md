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
   (other versions are not matched to this layout — do not mix.)  
2. Local **weights** per upstream instructions (or an existing `dlssnr_on_amd_weights.bin`).  
3. Any NVIDIA-related files **only from your own game**.

### Scripted install

1. Unzip this release.  
2. Create `vendor\` beside the scripts: put the **0.3.0** `version.dll`, plus weights or the upstream setup + `nvngx_dlss.dll` from **your** game.  
3. Close the game, then run (**argument is the directory that contains the game exe**, not the `.exe` file itself):

```bat
Setup.bat "path-to-game-folder"
```

**Game folder** = the directory with the main 64-bit exe — the same place you would drop `dxgi.dll` for a normal OptiScaler install.

- Onimusha (Xbox PC) example: `C:\XboxGames\Onimusha- Way of the Sword\Content`  
  (on this machine `OnimushaWotS.exe` lives under `Content\`.)  
- Many games use `...\Win64\` or `...\WinGDK\` — **use wherever the exe actually is**, not a fixed folder name.

The installer copies `OptiScaler.dll` to your chosen proxy name (default `dxgi.dll`) and copies `vendor\version.dll` to `dlssnr_amd_pass1.dll` (and pass2/3).

### Manual install

1. Install [DLSS-NR on AMD 0.3.0](https://github.com/danielblnc/DLSS-NR-on-AMD/releases) into the game folder per their instructions.  
2. In that folder, **copy** that **`version.dll`** to:  
   - `dlssnr_amd_pass1.dll` (**at least one**)  
   - `dlssnr_amd_pass2.dll` / `pass3.dll` if you want multi-layer neural rendering (same bytes, separate filenames so each pass can load its own instance)  
3. Extract **all** of this release into the same game folder.  
4. **Rename** `OptiScaler.dll` to the proxy you inject, e.g. `dxgi.dll` (`winmm.dll` etc. if that fits the game/other mods).  
5. Enable **DlssNr** in the OptiScaler menu when you want NR.

For other OptiScaler usage (**frame generation**, menu shortcuts, compatibility notes, per-game tips, etc.), see the official docs:  
[**OptiScaler Wiki**](https://github.com/optiscaler/OptiScaler/wiki).

---

## Credits & license

- [**OptiScaler**](https://github.com/optiscaler/OptiScaler) (GPL-3.0)  
- [**MatheusGViana/dlss-5-amd-project**](https://github.com/MatheusGViana/dlss-5-amd-project)  
- [**danielblnc/DLSS-NR-on-AMD**](https://github.com/danielblnc/DLSS-NR-on-AMD) **0.3.0** (not redistributed)  
- This repo: dual-slot every-frame, installer, packaging  

No NVIDIA binaries, NR weights, or upstream closed-source pass DLL are included. Follow each upstream’s license.
