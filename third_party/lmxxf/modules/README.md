# Precompiled HIP GPU Modules (RDNA4 / gfx1200 & gfx1201)

This directory contains precompiled AMD GPU code objects (`.hsaco`) loaded by the lmxxf neural rendering pipeline.

## Provenance
- **Upstream Repository**: https://github.com/lmxxf/dlss5-on-amd-9070xt-porting
- **Commit Base**: `3d9b3e42f3529609f824506c8d385bdd00b3e70c`
- **Target GPU Architectures**:
  - `gfx1200`: AMD RDNA4 gfx1200 (Radeon RX 9060 series) - experimental until hardware validation
  - `gfx1201`: AMD RDNA4 gfx1201 (Radeon RX 9070 / 9070 XT series)
- **License**: MIT, Copyright (c) 2026 Kien (`../LICENSE`)

## Build Method
These modules were compiled from `../hip/*.hip` sources using AMD's Runtime Compiler tool (`rtc_compile.cpp`) through the driver's `amd_comgr_3.dll` (no full HIP SDK required). See `../hip/README.md` and `../hip/build-modules.ps1` for compilation recipes and defines.
Local macro overrides from `tools/lmxxf-sync/module-defines.json` (`HIP_FFN_LINE_STORES 1` on `multihead-fast-padded-wave` and `multihead-fast-padded-wave-packed`) are preserved.

## Directory Layout (48 Code Objects total, 24 per architecture)
```text
modules/
  SHA256SUMS                 # 48-line root manifest covering both architectures
  runtime-manifest.json      # Schema 2 metadata
  README.md                  # This documentation
  gfx1200/                   # 24 .hsaco for gfx1200 + leaf SHA256SUMS + modules.json
  gfx1201/                   # 24 .hsaco for gfx1201 + leaf SHA256SUMS + modules.json
```

## Contents per Architecture (24 Code Objects each)
- `boundary-fast.hsaco`, `boundary_reference.hsaco`
- `c32_fast.hsaco`, `c32_fast_attention.hsaco`, `c32_fused_attention.hsaco`
- `c32_fused_ffn_attention-packed.hsaco`, `c32_fused_ffn_attention.hsaco`
- `c32_prefix_reference.hsaco`, `c32_tiled.hsaco`, `c32_wmma.hsaco`
- `deep_fast-packed.hsaco`, `deep_fast.hsaco`, `deep_reference.hsaco`, `deep_wmma.hsaco`
- `multihead-fast-packed.hsaco`, `multihead-fast-padded-wave-packed.hsaco`, `multihead-fast-padded-wave.hsaco`, `multihead-fast.hsaco`
- `multihead-reference.hsaco`, `multihead-tiled.hsaco`, `multihead-wmma.hsaco`, `multihead_fused_attention.hsaco`
- `prefix_fast.hsaco`, `wave-pointwise.hsaco`
- Leaf `modules.json`: Module metadata, compile defines, and source mapping.
- Leaf `SHA256SUMS`: Checksums of local architecture modules.
