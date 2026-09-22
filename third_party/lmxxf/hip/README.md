# hip/ — production HIP kernels (RDNA4, gfx1200 / gfx1201)

This directory is the HIP counterpart of `shaders/`: the kernel sources the shipped add-on actually loads, the compiler
that turns them into code objects, and one script that rebuilds all of them. Experiments, probes, ablations, validators
and the per-experiment compile scripts stay in `Development/HIP/`.

## Contents

| item | role |
|---|---|
| `*.hip` (21 files) | kernel sources; 24 modules are built from them (some modules concatenate two files, some build one file twice with different defines) |
| `rtc_compile.cpp` | host tool: source → `.hsaco` through the driver's `amd_comgr_3.dll` (no HIP SDK); also writes `<out>.hsaco.s` |
| `build-modules.ps1` | the recipe: one row per module (name, extra defines, sources); writes 24 modules and manifests per target, plus aggregate `SHA256SUMS` |
| `SHA256SUMS` | hashes of both architecture sets (48 modules, paths prefixed by gfx1200/gfx1201) |

## Build

Linux (mingw) builds the compiler; Windows with the AMD driver runs it:

```sh
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static hip/rtc_compile.cpp -o hip/rtc_compile.exe
```

```powershell
powershell -ExecutionPolicy Bypass -File hip\build-modules.ps1 -OutputDir D:\somewhere\modules
```

`build-modules.ps1` defaults to both targets, producing `gfx1200/` and `gfx1201/` subdirectories from the same sources. `-Only <module>` rebuilds that module for both targets. For a single-device diagnostic, explicitly pass `-Targets gfx1201` (or gfx1200); single-target output remains flat. Low-level `rtc_compile.exe out.hsaco source.hip comgr gfx1200` selects a target; its omitted-target default remains gfx1201 for old probe scripts. `RTC_EXTRA_OPTS` (space-separated clang/backend options) is honoured by
`rtc_compile.exe`; production is built with it unset.

The add-on reads the selected HIP device’s `gcnArchName` and loads `DLSS5-AMD\native-game-tiled-assets\HIP\gfx1200` or `gfx1201`. Flat directories remain supported for old single-target installs. The standalone reference CLI takes the architecture leaf directory explicitly. Modules and weights are opened with Unicode paths; modules use `hipModuleLoadData`. Device selection prefers Windows LUID, with name matching only when HIP supplies no LUID.

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

## Corrected full MH byte stream (2026-09-19, unreleased)

The new `*_fb_bout_diag` entry points retain the fast residual projection for byte outputs. With the decoder tail fix, the full MH stream passes the recorded cross-tier/input checks and both golden suites. Relative to local byte features, measured savings are ~0.05/0.07/0.16 ms at 720/900/1080. It remains opt-in: use `Development/HIP/full-byte-flags.txt` with the matching new host and module set; the ViT byte stream remains disabled.

The halfweight decoder now selects a full-tile path once per workgroup; only the final partial tile performs per-token input/output bounds checks. Output-dimension cropping remains active in both paths. This preserves the tail fix while saving ~0.14 ms at 900 and ~0.34 ms at 1080 in the recorded ABBA tests.

`DLSS5_HIP_DECODER_BYTE=1` additionally lets upsamplers 48/56/62 store their already-quantized FP8 values as bytes and passes byte input to each following FFN. Decoder39 and the final 32-channel upsampler retain float output. Matched host/module updates are required. The recorded tests save ~0.08 ms at 900 and ~0.05 ms at 1080 relative to the full MH byte stream with float upsampler outputs.

## 0.25 validation status

Both targets compile from the same source. RX 9070 XT/gfx1201 passed the automatic-selection and golden-output checks, including a Chinese-path package check. gfx1200 is built and packaged for RX 9060/9060 XT user testing; no gfx1200 hardware was available locally. Later optimization sections above describe changes now included in 0.25.
