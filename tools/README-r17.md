# OptiScaler AMD pre-SR (r17) — every-frame multi-slot

Fork of **MatheusGViana / dlss-5-amd** (OptiScaler AMD pre-SR) with host-side
multi-slot every-frame NR. Uses the **author's AMD NR 0.3.0 runtime** as
`dlssnr_amd_pass1-3.dll`. Super-resolve is **FFX/FSR**.

**Not** a reimplementation of the neural runtime. **Not** a ReShade filter.

## What is in this zip

| Item | Role |
|---|---|
| `OptiScaler.dll` | This project (r17). Install as `dxgi.dll` (or another proxy) |
| `OptiScaler.ini` | Template; every-frame NR default in r17 |
| `OptiScaler\` | FFX / XeSS / Agility D3D12 dependencies |
| `experimental_lighting\` | Optional RTGI shaders |
| `Setup.ps1` / `Setup.bat` | Installer (interactive) |
| `Licenses\` | Third-party licenses for the bundled upscaler deps |
| `SHA256SUMS.txt` | Checksums of files in this archive |

**Not included (you must supply locally):**

- `nvngx_dlss.dll` (NVIDIA) — from your game
- `dlssnr_on_amd_weights.bin` — generate with the NR author's setup on your PC
- Author AMD NR **0.3.0** `version.dll` — copied by the installer to `dlssnr_amd_pass1-3.dll`

This package does **not** redistribute NVIDIA binaries, NR weights, or the
author's closed-source pass DLL.

## Requirements

- AMD GPU (tested RX 9070 XT) + HIP 7 (`amdhip64_7.dll`)
- Windows 10/11 x64
- A DX12 game that can use OptiScaler / FFX (DLSS-titled games work: OptiScaler
  reads DLSS inputs, then FFX upscales)
- Author AMD NR **0.3.0** + local weights (see vendor folder instructions)

## Install (Xbox / MS Store)

1. Unzip this package.
2. Create `vendor\` next to `Setup.ps1` and put:
   - `version.dll` — author **0.3.0**
   - `nvngx_dlss.dll` — only if you still need to generate weights
   - `dlssnr_on_amd_setup.exe` — only if generating weights here
   - or a ready `dlssnr_on_amd_weights.bin`
3. Close the game.
4. Run:

```bat
Setup.bat "C:\XboxGames\<Game>\Content"
```

Use the **writable Content** folder, **not** `C:\Program Files\WindowsApps\...`.

5. In-game: open the OptiScaler menu (Insert) → enable **DlssNr** / AMD neural.
   Every-frame is the default in r17.

### Other games (Steam / GOG)

Point `Setup.bat` at the folder that contains the game **exe** (where you would
drop `dxgi.dll`). If that folder is not writable, pick another proxy
(`winmm.dll`, etc.) only when you know it is safe; the installer will list
existing injection DLLs and let you cancel / backup / ignore.

## Native 0.3 only (no OptiScaler)

If you only want the author's native `version.dll` path, install **that**
yourself and **do not** also drop this `dxgi.dll` in the same folder.

## Performance (reference)

720p, every-frame NR, RX 9070 XT: typical **~45–49 fps**, PresentMon
`MsGPUWait ≈ 0`, close to native 0.3 on the same route. Scene-dependent.
Rare long stalls can still appear as 2s+ jobs inside the NR runtime log.

## Credits

- Neural runtime: **DLSS-NR on AMD** author (0.3.0 pass DLL)
- OptiScaler bridge base: **MatheusGViana/dlss-5-amd-project**
- Multi-slot / every-frame host path, packaging: this fork

## License

OptiScaler and bundled FFX/XeSS/Agility components: see `Licenses\`.
Your use of the NR runtime and NVIDIA files is under **their** terms.
