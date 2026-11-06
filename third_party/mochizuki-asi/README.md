# Official Mochizuki NR ASI payload (0.0.8.3)

Vendored payload for `dlssnr-amd.asi`:

- Base: `DLSSNR-AMD 0.0.8.3` (`931c50f`) + Direct ELF D3D12 passthrough (`6a9249f`)
- Plugin: `OptiScaler/plugins/dlssnr-amd.asi`
- Probe: `nr_present_probe.exe`
- Resources: `dlssnr-amd/` (shaders, ACO records/shaders, INT4 / performance / ultra data, language files)
- Extraction tools: `model-tools/dlssnr_extract_model.exe`, `model-tools/dlssnr-int4-weights.exe`
- Weight deltas: `experimental/dlssnr-small-experimental.delta`, `optimized/dlssnr-optimized.delta`
- ASI config templates: `ini/dlssnr-amd-optiscaler.ini`, `ini/dlssnr-amd-optiscaler-renodx.ini`
- Extra licenses: `Licenses/DearImGui-LICENSE.txt`, `Licenses/Inter-LICENSE.txt`

Neural Rendering is owned by this plugin. The project no longer builds or ships
`MochizukiNrRuntime.dll`, and the host does not require it: it detects the loaded
`dlssnr-amd.asi` module at runtime and disables its own NR engine.

Installation copies this tree as-is into the game folder (`OptiScaler/plugins/dlssnr-amd.asi`,
`dlssnr-amd/*`, `model-tools/*`). Models are extracted from the user's
`nvngx_dlssnr.dll` with the tools above; only file names, layout and extraction
success are checked. There is deliberately no hash or version whitelist, so a
newer official plugin or resource set can replace these files.

Do not mix this resource tree with the retired built-in runtime's `dlssnr-amd`
shaders or ACO files. Upgrade and uninstall operate on the installed file
inventory, never on pattern guesses.
