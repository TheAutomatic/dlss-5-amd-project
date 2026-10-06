# PR 12 interface migration candidate (not release integrated)

This branch is a consumer proof for upstream PR 12, based on product main
238e066c and staged raw candidate f8e98d984f4815a66eb20626af1770722ae7f379
(upstream base 297b032ac55f005d78568e684f30608651044f62).

The manifest has zero preserved headers and zero applied patches. The official
completed pin in UPSTREAM.md is deliberately unchanged; sync-state.json remains
pending. Do not merge this candidate as a finished upstream update or package it.

## Local ownership

- LmxxfShaderCompiler.h owns System32 compiler identity validation, includes,
  target fallback and cache isolation; it implements the upstream provider API.
- Typeless RGBA16 selection and replay opt-in are per codec instance.
- Runtime requests direct History/post auxiliary output only for its supported
  single-pass configuration and gates adaptive reuse without changing preferences.
- NativeTemporalHistory.h aliases the upstream helper; TemporalControl and the
  product's frame/reset/menu/INI policy stay local.
- Bridge timing epochs, retirement markers and diagnostic transport are requested
  through public APIs; no private copy of the bridge replaces a pin.
- R10 follows the shared fallback admission and private FP16 codec path.

## Validation completed

- Raw candidate include-path MSVC compile and link of LmxxfNrRuntime.dll.
- Standard tools/build/build-lmxxf-runtime.cmd against the staged raw vendor.
- Local compiler cold/warm regressions, including preloaded old same-name DLL,
  include/cache/error paths and 136 shader/target PSO variants, passed after moving
  implementation into the local provider namespace.
- Upstream interface tests: WARP and RX 9070 XT codec/default comparisons, shader
  History, paused-queue deferred addon controls, and module-load fault injection.
- Upstream C32 kernel recipes compiled for gfx1200/gfx1201; legacy machine-code
  comparisons allow only constant/entry relocation (details in upstream PR).

## Pending acceptance gates

1. Upstream author acceptance; resolve requested interface changes on the same PR.
2. Complete staged feature review from b687e13a to the accepted target. The audit
   collected 3490 review items and correctly refused to certify this candidate.
   No blanket approval or fingerprint replacement was used.
3. Build matching complete module sets, verify ABI/recipes, run the real network
   auxiliary-output/replay/failure tests, and migrate old patch-fixture/contract
   tests to the accepted raw-source contract. The old module receipts are not
   candidate module provenance.
4. Verify product typeless/R10/History behavior in games and perform release tests
   only when actually preparing that release. No game files were modified here.

Therefore this proves a zero-patch source consumer can compile. It does not yet
prove zero-patch production integration, unchanged performance, or no regressions.
Historical patch files and fixtures are retained as evidence until final adoption.
