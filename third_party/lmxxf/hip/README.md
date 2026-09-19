# hip/ — production HIP kernels (RDNA4, gfx1201)

This directory is the HIP counterpart of `shaders/`: the kernel sources the shipped add-on actually loads, the compiler
that turns them into code objects, and one script that rebuilds all of them. Experiments, probes, ablations, validators
and the per-experiment compile scripts stay in `Development/HIP/`.

## Contents

| item | role |
|---|---|
| `*.hip` (21 files) | kernel sources; 24 modules are built from them (some modules concatenate two files, some build one file twice with different defines) |
| `rtc_compile.cpp` | host tool: source → `.hsaco` through the driver's `amd_comgr_3.dll` (no HIP SDK); also writes `<out>.hsaco.s` |
| `build-modules.ps1` | the recipe: one row per module (name, extra defines, sources); writes `modules/*.hsaco`, `modules.json`, `SHA256SUMS` |
| `SHA256SUMS` | hashes of the production module set (0.20 baseline; the two C32 FFN/attention modules and fused multihead attention updated on 2026-09-19) |

## Build

Linux (mingw) builds the compiler; Windows with the AMD driver runs it:

```sh
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static hip/rtc_compile.cpp -o hip/rtc_compile.exe
```

```powershell
powershell -ExecutionPolicy Bypass -File hip\build-modules.ps1 -OutputDir D:\somewhere\modules
```

`-Only <module>` rebuilds one module. `RTC_EXTRA_OPTS` (space-separated clang/backend options) is honoured by
`rtc_compile.exe`; production is built with it unset.

The add-on loads the modules from `DLSS5-AMD\native-game-tiled-assets\HIP\` (or the directory in `DLSS5_HIP_MODULES`).

## Recipe rules

- every module is prefixed with `#define HIP_ISA_HALF 1`;
- module names ending in `-packed` also get `#define HIP_PREPACKED_WEIGHTS 1` (prepacked weight layout);
- the four extra defines in the table (`HIP_C32_DIAG_WEIGHTS 1`, `HIP_MH_RTZ_ISA 1`, `HIP_BRANCHLESS_F 1`,
  `HIP_FFN_HOIST_RES 2`) are the production selections of 0.20; they equal the sources' defaults and are written out so
  the recipe does not rely on those defaults;
- `c32_fused_attention.hsaco` is built from `c32_fused_attention_packed.hip` (historical name; the module itself is not
  `-packed`).

## Reproducibility

COMGR output is deterministic for identical source text. The only source-text-dependent bytes are the `__hip_cuid_*`
symbol (a hash of the compilation unit), so editing comments or adding macros that default off changes the module's
SHA-256 without changing a single instruction. Verification therefore is: the three bit-exact checks of
`Development/HIP/validate-modules.ps1` (40-frame HDR hash, 24-frame reset-every-8 hash, seed-123 history hash), plus
`<module>.hsaco.s` compared with the `__hip_cuid` lines removed when byte equality with an older build is in question.

The 0.20 set rebuilt from this directory (2026-09-17 10:07) passed all three checks. 17 of 24 modules are byte-identical
to the shipped `ffnh2-modules`; of the other 7, the four the production flag set loads (`c32_fused_ffn_attention-packed`,
`multihead_fused_attention`, `deep_fast-packed`, `multihead-fast-padded-wave-packed`) differ in the cuid only, and the
three unpacked variants (`c32_fused_ffn_attention`, `multihead-fast-padded-wave`, `deep_fast`, loaded only without
`packed_weights`) now carry the same branch-free conversions as their packed twins, which the shipped set built from the
09-16 sources did not.

## Unreleased attention updates (2026-09-19)

The fused C32 kernels retain half exponent values in registers for probability normalization. Fixed-index loops are explicitly unrolled to avoid expensive dynamic vector-array indexing. On the fixed-capture test bench this saves about 0.19 ms at the 1080 tier and 0.15 ms at the 900 tier, with matching outputs in the recorded checks. Published 0.24.2 archives still contain the preceding modules.

A subsequent bounded-reciprocal change uses hardware reciprocal plus two FMA refinements for the positive normalization denominator. An exhaustive gfx1201 check of every float in [1/256, 624] matched `1.f/x` bit-for-bit (144,441,345 inputs). The fixed-capture ABBA tests show a further ~0.05 ms at 1080 and ~0.06 ms at 900. This is also unreleased.

The C64/C128/C256 fused attention-project bodies also retain their exact half exponent values and unroll fixed-index loops. The combined change measured ~0.10 ms at 1080 and ~0.08 ms at 900; separate per-shape tests did not show additive benefits, so those figures describe the combined module only.

## Decoder tail fix (2026-09-19, unreleased)

The 900 tier has 50×30=1500 input tokens at decoder48. Launching ceil(tokens×channels/256) groups omitted four channel tiles; the correct grid is ceil(tokens/16)×(channels/16). The host now computes that grid and the fast/WMMA decoder kernels mask tail reads and writes. Decoder kernels support partial token tiles; the other WMMA kernels retain their alignment requirements.

The missing tiles left 3072 latent floats unwritten. The old 960-row goldens therefore depended on buffer contents and are replaced in `Development/HIP/validate-modules-960.ps1`; 900w, 720 and 1080 recorded results are unchanged. Install the matching host DLL and decoder modules together.
