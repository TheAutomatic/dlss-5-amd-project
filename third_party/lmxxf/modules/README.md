# Precompiled HIP GPU Modules (RDNA4 / gfx1200 & gfx1201)

This directory contains precompiled AMD GPU code objects (`.hsaco`) loaded by the lmxxf neural rendering pipeline.

## Provenance
- **Upstream Repository**: https://github.com/lmxxf/dlss5-on-amd-9070xt-porting
- **Commit Base**: `c81a88bc8534f7193df08ec3cae21d06d10d285d`
- **Target GPU Architectures**:
  - `gfx1200`: AMD RDNA4 gfx1200 (Radeon RX 9060 series) - experimental until hardware validation
  - `gfx1201`: AMD RDNA4 gfx1201 (Radeon RX 9070 / 9070 XT series)
- **License**: MIT, Copyright (c) 2026 Kien (`../LICENSE`)

## Build Method
The original RowOpts recipe uses LLVM23.1.2 in WSL/Linux for c32-wave1, c32-wave1-rtz, c32-wave1-fast, c64-wave2 and c64-wave2-fast, with HIP_BARRIER_FENCE 1 on C64. The other 29 modules per architecture use driver COMGR via rtc_compile.cpp; c512-m32-deep uses max-ilp. No full HIP SDK is required. Actual defines, compiler commands and source/object hashes are in modules.json; reproducible steps are in [the sync workflow](../../../tools/lmxxf-sync/README.md#llvm23-与-rowopts-构建).
Local macro overrides from `tools/lmxxf-sync/module-defines.json` (`HIP_FFN_LINE_STORES 1` on `multihead-fast-padded-wave` and `multihead-fast-padded-wave-packed`) are preserved.

## Directory Layout (68 Code Objects total, 34 per architecture)
```text
modules/
  SHA256SUMS                 # 68-line root manifest covering both architectures
  runtime-manifest.json      # Schema 2 metadata
  README.md                  # This documentation
  gfx1200/                   # 34 .hsaco for gfx1200 + leaf SHA256SUMS + modules.json
  gfx1201/                   # 34 .hsaco for gfx1201 + leaf SHA256SUMS + modules.json
```

## Contents per Architecture (34 Code Objects each)
- `boundary-fast.hsaco`, `boundary_reference.hsaco`
- `c32_fast.hsaco`, `c32_fast_attention.hsaco`, `c32_fused_attention.hsaco`
- `c32_fused_ffn_attention-packed.hsaco`, `c32_fused_ffn_attention.hsaco`
- `c32_prefix_reference.hsaco`, `c32_tiled.hsaco`, `c32_wmma.hsaco`
- `deep_fast-packed.hsaco`, `deep_fast.hsaco`, `deep_reference.hsaco`, `deep_wmma.hsaco`
- `multihead-fast-packed.hsaco`, `multihead-fast-padded-wave-packed.hsaco`, `multihead-fast-padded-wave.hsaco`, `multihead-fast.hsaco`
- `multihead-reference.hsaco`, `multihead-tiled.hsaco`, `multihead-wmma.hsaco`, `multihead_fused_attention.hsaco`
- `prefix_fast.hsaco`, `wave-pointwise.hsaco`
- `c32-wave1.hsaco`, `c32-wave1-rtz.hsaco`, `c32-wave1-fast.hsaco`, `c64-wave2.hsaco`, `c64-wave2-fast.hsaco`, `c512-m32-mh.hsaco`, `c512-m32-deep.hsaco`
- `vit-stream.hsaco`, `vit-wide-deep.hsaco`, `swin-persistent.hsaco`
- Leaf `modules.json`: Module metadata, compile defines, and source mapping.
- Leaf `SHA256SUMS`: Checksums of local architecture modules.
