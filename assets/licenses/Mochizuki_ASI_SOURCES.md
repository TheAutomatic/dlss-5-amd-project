# Mochizuki NR ASI payload (0.1.0m3)

Vendored payload for `dlssnr-amd.asi` and Standalone / ReShade / DXVK routes:

- Base resources & build: built from `DLSSNR-AMD` (release `v0.1.0m3` on `TheAutomatic/DLSSNR-AMD`, `x86_64` + 32-bit `i686` runtime overlay, upstream `v0.1.0` base), preserving the CJK language pack (`DLSSNR-AMD-lang-CJK-0.1.0.zip`).
- Plugin path: `OptiScaler/plugins/dlssnr-amd.asi` (also reused for Standalone route)
- Probe path in the release: `dlssnr-amd/nr_present_probe.exe`
- Resources: `dlssnr-amd/` (SPIR-V shaders, ACO records/shaders, pre-captured D3D12 outer ACO archives `aco/d3d12_outer.nra` & `aco/rdna3/d3d12_outer.nra`, `CC_SILVER_AARDWOLD_OPTIMIZED-{ultra,balanced}` / `CC_SMALL_FP8_OPTIMIZED-{ultra,balanced}` INT4 data, language files; retired D3D12 HLSL shader sources removed)
- Standalone route: `standalone/dxgi.dll`, `standalone/vk-override/`
- ReShade / DXVK / Vulkan routes: `reshade/` (`ReShade64.dll`, `ReShade32.dll`, `dlssnr_amd.addon64`, `dlssnr_amd.addon32`, shaders, presets, `vk-override/`), `dxvk/` (`d3d9.dll`, `dxgi.dll`, `x86/`), `vulkan/` (`vulkan-1.dll`, `x86/`)
- Extraction tools: `model-tools/dlssnr_extract_model.exe`, `model-tools/dlssnr-int4-weights.exe`, `model-tools/*_INT4*.bin.sha256`
- Weight deltas: `CC_SMALL_FP8_OPTIMIZED/CC_SMALL_FP8_OPTIMIZED.delta`, `CC_SILVER_AARDWOLD_OPTIMIZED/CC_SILVER_AARDWOLD_OPTIMIZED.delta`
- ASI config templates: `ini/dlssnr-amd-optiscaler.ini`, `ini/dlssnr-amd-optiscaler-renodx.ini`, `ini/dlssnr-amd-standalone.ini`, `ini/dlssnr-amd-addon.ini`
- Extra licenses: `Licenses/DearImGui-LICENSE.txt`, `Licenses/Inter-LICENSE.txt`, `Licenses/ReShade-LICENSE.md`, `Licenses/DXVK-LICENSE.txt`, `Licenses/Vulkan-Loader-LICENSE.txt`, `Licenses/vort-LICENSE.txt`, `Licenses/DLSS5_Feed-LICENSE.txt`

Neural Rendering is owned by this plugin. The project no longer builds or ships
`MochizukiNrRuntime.dll`, and the host does not require it: it detects the loaded
`dlssnr-amd.asi` module at runtime and disables its own NR engine.

Installation copies this tree as-is into the game folder (`OptiScaler/plugins/dlssnr-amd.asi`,
`dlssnr-amd/*`, `model-tools/*`). Models are extracted from the user's
`nvngx_dlssnr.dll` with the tools above; only file names, layout and extraction
success are checked. There is deliberately no hash or version whitelist, so a
newer official plugin or resource set can replace these files.

Do not mix this resource tree with the retired built-in runtime's `dlssnr-amd`
shaders or ACO files. In-place upgrades use the installed file inventory to
retire stale assets. A full Windows uninstall removes the dedicated
dlssnr-amd resource directory, including generated models and caches.
