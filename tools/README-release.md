# OptiScaler AMD pre-SR — 1.8.0-0.3.0

Fork of **MatheusGViana / dlss-5-amd** (OptiScaler AMD pre-SR) with host-side
**every-frame multi-slot** NR. Requires the **author's AMD NR 0.3.0** runtime
as `dlssnr_amd_pass1-3.dll`. Super-resolve is **FFX/FSR**.

| Version part | Meaning |
|---|---|
| **1.8.0** | This fork (OptiScaler host / installer / multi-slot) |
| **0.3.0** | Required upstream AMD NR runtime |

**Not** a reimplementation of the neural runtime. **Not** a ReShade filter.

## What is in this zip

| Item | Role |
|---|---|
| `OptiScaler.dll` | This project. Install as `dxgi.dll` (or another proxy) |
| `OptiScaler.ini` | Template; **every-frame NR is default** |
| `OptiScaler\` | FFX / XeSS / Agility D3D12 dependencies |
| `experimental_lighting\` | Optional RTGI shaders (if present) |
| `Setup.ps1` / `Setup.bat` | Installer |
| `Licenses\` | Third-party licenses for bundled upscaler deps |
| `SHA256SUMS.txt` | Checksums |

**Not included (supply locally):**

- `nvngx_dlss.dll` — from **your** game
- `dlssnr_on_amd_weights.bin` — generate with the NR author's setup on your PC
- Author AMD NR **0.3.0** `version.dll` — installer copies it to `dlssnr_amd_pass1-3.dll`

No NVIDIA binaries, NR weights, or the author's closed-source pass DLL are redistributed.

## Requirements

- AMD GPU (tested RX 9070 XT) + HIP 7 (`amdhip64_7.dll`)
- Windows 10/11 x64
- DX12 game that can use OptiScaler / FFX (DLSS titles work: DLSS **inputs** → NR → FFX)
- Author AMD NR **0.3.0** + local weights

## Install (Xbox / MS Store)

1. Unzip this package.
2. Create `vendor\` next to `Setup.ps1`:
   - `version.dll` — author **0.3.0**
   - `nvngx_dlss.dll` — only if you need to generate weights
   - `dlssnr_on_amd_setup.exe` — only if generating weights here
   - or a ready `dlssnr_on_amd_weights.bin`
3. Close the game.
4. Run:

```bat
Setup.bat "C:\XboxGames\<Game>\Content"
```

Use the writable **Content** folder, **not** `C:\Program Files\WindowsApps\...`.

5. In-game: OptiScaler menu (Insert) → enable **DlssNr** / AMD neural.

The menu also lets you change `Dx12Upscaler` — this package ships `ffx` (FSR) as the
default. Games driven through a DLSS or XeSS input can switch it there; NR sits on the
DLSS-input path either way.

### Other games

Point `Setup.bat` at the folder that contains the game **exe** (where you would drop `dxgi.dll`). The installer lists existing injection DLLs and lets you cancel / backup / ignore.

## Native 0.3 only (no OptiScaler)

Install the author's `version.dll` yourself and **do not** also drop this `dxgi.dll` in the same folder.

## Performance (reference)

720p, every-frame NR, RX 9070 XT: typical **~45–49 fps**, PresentMon `MsGPUWait ≈ 0`, close to native 0.3 on the same route. Scene-dependent. Rare long stalls can still appear as 2s+ jobs inside the NR runtime log.

## Credits

- **[OptiScaler](https://github.com/optiscaler/OptiScaler)** — core upscaler proxy this package is built on (GPL-3.0; see `Licenses\OptiScaler_LICENSE.txt`)
- **[MatheusGViana / dlss-5-amd-project](https://github.com/MatheusGViana/dlss-5-amd-project)** — AMD pre-SR / DLSS-input bridge fork this work continues
- **[danielblnc / DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)** — AMD NR runtime **0.3.0** (`dlssnr_amd_pass1-3.dll`; not bundled)
- Multi-slot every-frame host path, installer, packaging: this fork

## License

OptiScaler and bundled FFX/XeSS/Agility: see `Licenses\`.  
**OptiScaler is GPL-3.0**, so this package is distributed under the same terms and its
corresponding source is published at <https://github.com/TheAutomatic/dlss-5-amd-project>
(the release tag matches the version on this package).  
Use of the NR runtime and NVIDIA files is under **their** terms.

## Building this package (developers)

The FFX / XeSS signed DLLs live in **git submodules** under `external\`, so a plain clone
is not enough:

```bat
git clone --recursive <repo>
:: or, in an existing clone
git submodule update --init --recursive
```

Without them `tools\PACKAGE_RELEASE.ps1` fails with a missing-dependency list (it will not
silently ship a package without FFX). Then:

```powershell
analysis\build-release-r18.cmd            # Release build, multi-slot is the source default
tools\PACKAGE_RELEASE.ps1                 # -> dist\OptiScaler-AMD-PreSR-<version>.zip
```
