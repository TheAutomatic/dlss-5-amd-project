# Precompiled HIP GPU Modules (RDNA4 / gfx1201)

This directory contains precompiled AMD GPU code objects (`.hsaco`) loaded by the lmxxf neural rendering pipeline.

## Provenance
- **Upstream Repository**: https://github.com/lmxxf/dlss5-on-amd-9070xt-porting
- **Commit Base**: `24986ae094bbd150f4d86a0ca76159a43f374884`
- **Target GPU Architecture**: AMD RDNA4 `gfx1201` (RX 9070, RX 9070 XT)
- **License**: MIT, Copyright (c) 2026 Kien (`../LICENSE`)

## Build Method
These modules were compiled from `../hip/*.hip` sources using AMD's Runtime Compiler tool (`rtc_compile.cpp`) through the driver's `amd_comgr_3.dll` (no full HIP SDK required). See `../hip/README.md` and `../hip/build-modules.ps1` for compilation recipes and defines.

## Contents (24 Code Objects, ~4.4 MB)
- `boundary-fast.hsaco`, `boundary_reference.hsaco`
- `c32_fast.hsaco`, `c32_fast_attention.hsaco`, `c32_fused_attention.hsaco`
- `c32_fused_ffn_attention-packed.hsaco`, `c32_fused_ffn_attention.hsaco`
- `c32_prefix_reference.hsaco`, `c32_tiled.hsaco`, `c32_wmma.hsaco`
- `deep_fast-packed.hsaco`, `deep_fast.hsaco`, `deep_reference.hsaco`, `deep_wmma.hsaco`
- `multihead-fast-packed.hsaco`, `multihead-fast-padded-wave-packed.hsaco`, `multihead-fast-padded-wave.hsaco`, `multihead-fast.hsaco`
- `multihead-reference.hsaco`, `multihead-tiled.hsaco`, `multihead-wmma.hsaco`, `multihead_fused_attention.hsaco`
- `prefix_fast.hsaco`, `wave-pointwise.hsaco`
- `modules.json`: Module metadata and layout definitions.
- `runtime-manifest.json`: Runtime loading configuration.
- `SHA256SUMS`: Checksums of all module files.
