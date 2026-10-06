# lmxxf official-source integration and PR #12 consumer migration

## Scope and state

Official comparison: `b687e13a8fcb8efd5be905ebbd0c9d70e15d88e3` to
`297b032ac55f005d78568e684f30608651044f62`. The latter is the official main fetched
on 2026-10-06, not the PR head. PR #12 interface implementation was reviewed from
`f8e98d984f4815a66eb20626af1770722ae7f379` (later PR documentation commit
`68b9a4cd43dce0ec659eb9bed9fb8bbeab88afb2`). Acceptance of that PR is still required
before removing preservation rules. The bridge remains PINNED; all FOLLOW changes
are replayed by `pr12-integration-interfaces.patch` against raw official inputs.

Completion is recorded by `third_party/lmxxf/UPSTREAM.md` and the content-bound
review after the staged sync succeeds. Pending attempts remain in `sync-state.json`.

## Interfaces and product policy

`native_shader_cache.h` supplies a per-instance compiler provider interface. The
System32 compiler identity, caching, include handling and target fallback live in
product `LmxxfShaderCompiler.h`. Codec, RGB and History creation receive that provider;
upstream clients retain their original compiler behavior. No mutable global callback.

`NativeCodecParameters::use_pre_exposure` and `hue_safe` default off upstream.
Our encode/decode call sites explicitly opt in, preserving our pre-exposure and
color-strength curve. R10 uses the upstream format fallback; RGBA16 typeless view
selection is per codec instance. Active input subrect retains full allocation stride.
`EnableReplayableRecording` is requested before Create by product recording sessions.
The bridge retains queue/completion receipts, clear/discard paths and explicit release markers.

The History algorithm remains in product `NativeTemporalHistory.h`, with unchanged
HLSL versus local main. Only compiler injection and initialization validation change.
`RequestDirectHistory`, `RequestPostAuxiliary(row)` and `SetAdaptiveReuseAllowed`
are explicit upstream interfaces; the model row, temporal control, motion/depth checks,
menu defaults and History/ViT exclusion remain product policy. Auxiliary output is
MP1-only, two floats per pixel after RGB; selection checks the actual module exports,
including the optional normalization module. No upstream reference History is enabled.

## Final output and pool64

`hip_reference_network.h::EnqueueRaw/RunGraph/MultiPassRest` borrows the bridge final
RGB sink, without freeing it. Only the last real post, prediction apply or skin blend
writes it; intermediate tensors remain private. Graph, overlap and aliasing retain
the copy path. `HIP_FINAL_OUTPUT_DIRECT=1`, `HIP_FINAL_COPY_REPEAT=1` are retained.
The post auxiliary buffer remains disjoint after the RGB allocation.

`HIP_POOL64_BYTE_EDGE=1` uses `mh_pool_project_c32_b8_out8` only with both producer
and consumer exports and the existing byte-layout prerequisites, with block 5 present.
The producer keeps both WMMA accumulations, Hrtz and F, and encodes the already
quantized result. It does not enable W16_SMALL or change the first C64 arithmetic.
Official evidence: `Development/results/final-output-direct-20261005/README.md`
and `pool64-byte-20261005/README.md`: whole-network and multi-pass comparisons,
including old-module fallbacks; performance is upstream frame replay, not our game FPS.

## Scoped normalization and Swin

`CW_NORM_HOIST=1` exists only in `c32-wave1-fast-norm900`; ordinary recipes keep 0.
`C32Norm900Active/Fn` selects it only for fast numerics, 1600x960, MP1, graph off,
reference experiment off. Full legacy export admission plus auxiliary-export admission
prevents a mixed post implementation. Other dimensions/passes keep ordinary modules.
Official evidence: `c32-norm-hoist-compat-20261006/README.md`,
`c32-norm-hoist-framework-20261006/README.md` and the unreleased CHANGELOG entry.
The reported 0.045-0.049 ms gain is limited to upstream 900-tier frame replay;
1152-row p99 regression is why that geometry is not selected.

`HIP_SP_1440=1` adds 2560x1472 to the persistent route only for fast numerics and
valid init/run/recover exports from `swin-persistent-fast`. Constructor prepares both
plans before producer waits. Existing 900/1080 routes keep the normal persistent module.
FAST0, graph, missing module/exports use the original Body. The new recipe adds only
`W2_FAST_NUM 3`, matching the surrounding fast C256 arithmetic. Official evidence:
`Development/results/sp1440-fast-20261005/README.md`, including timeout recovery,
ticket rollover and whole-network comparisons. Other geometries are not generalized.

## Excluded experiments and addon-only changes

Submission pulse is deferred: its own `pulse-production-20261006/README.md` still
records incomplete post-integration MP/history/resize and performance coverage.
`LmxxfProductionOptions` sets submit_pulse=0 after environment parsing. Reconsider only
with product queue/History/multi-pass/resize failure tests and controlled frame timing.
Device-TLS selection fixes for prepare/drain are retained independently of the pulse.

The upstream reference experiment allocates feature-tap and sigmoid/history assets,
uses its own warp/gate and addon FFX metadata/seed policy. `experimental_temporal` and
`temporal_feature_tap` are forced false in product options. Its new addon frame,
oneshot, pre-upscale and trial-installer changes are not compiled or packaged here.
Reconsider only as a separately compared algorithm with acceptable cost and image quality;
do not replace the product History or assume its motion/depth contract is equivalent.

Graph0 replay/DAG, alternate ViT math/layout, sparse stages, prefix noise cache and
other research scripts/results are outside the manifest/build closure. Their changes
are inventoried, not installed. The production diff/recipe was checked independently
for promoted changes (the four groups above); directory names alone do not establish
exclusion. Numeric/reference research is useful evidence, not a new product default.
Input polling and the independent F8 reuse hotkey stay removed by the preservation
overlay, as decided before this sync. Graph/diagnostic defaults and INI-owned selections
are unchanged. Do not remove these hunks merely because the generic PR interfaces exist.
No deployment profile changed between these two official commits.

## Build and validation

40 modules per architecture: six LLVM23.1.2 rows with original RowOpts and
-real-true16, 34 driver-COMGR rows; two existing LINE_STORES overrides remain.
Both architecture builds and full module provenance are required before completion.
The release workflow still requires its own final artifact CI and game acceptance.

Validation on 2026-10-06, RX 9070 XT (gfx1201):

- Built all 80 modules with source/recipe/compiler provenance, MSVC runtime and module contract checks.
- Raw official snapshot / full active patch replay: 2 tests passed. Sync tools: 69 tests;
  66 passed initially, three consolidated-patch test expectations corrected and rerun successfully.
- ABI/C smoke and 23 runtime validation tests passed; seven cross-CRT configuration-priority
  tests passed; module package/installer focused tests: 24 passed.
- WARP suite passed, including temporal/recording checks and 136 shader/PSO variants,
  private compiler cold/warm cache and error injection.
- `tests/lmxxf/run.cmd gpu`: passed History, exclusion, format/exposure, recorded submission,
  adaptive resets, zero/recovery, source precedence, subrect, size and MP2/MP3 lifecycle checks.
- After restoring the previously excluded input polling and F8 hotkey paths, final MSVC
  build plus History900, recording, bridge adaptive-reset1080 and forced-exclusion GPU
  checks passed. Setting external pulse/history-experiment/input-poll/hotkey flags did not
  enable these paths; pulse creation/record counts stayed zero.
- Eight baseline comparisons against local main `238e066c` were byte-identical:

| Case | Output FNV hash (both versions) |
|---|---|
| 1600x900 FAST1 MP1 | `4e4edf07434696d2` |
| 1920x1080 FAST1 MP1 | `806dd30da2c516da` |
| 2560x1440 FAST1 MP1 | `b372262be18f3ed1` |
| 1600x900 MP2 | `79a456c0074a5880` |
| 1600x900 predicted MP3 + skin | `e5f338720d6fdc6d` |
| 1600x900 real MP3 | `98422bddcb60d1ab` |
| 1920x1080 auto exposure | `aa04e0dcec54f00e` |
| 1920x1080 R10 input | `c71b655ccda6a57c` |

The migration initially made `activeWidth != 0` enable codec fitting even at 1:1 size.
This introduced a bilinear interpolation and FP16 rounding change before the network
(1080p mean absolute final RGBA error 0.00422, max 0.15723). Isolating old modules,
old shaders, pool64 and final direct output traced it to codec macro selection.
Restoring the existing geometry/decoder-allocation predicate restored byte equality.
The GPU tier now asserts the FAST1 1:1 baseline, in addition to its existing adapted/subrect
checks. `none` extra skips, FAST0 tall RTZ selection and the supported fast-twin list are
also preserved rather than silently discarded by interface migration.

These corrections remain in the local overlay. PR acceptance alone is insufficient to
remove pin/patch rules: compare the author's merged interfaces **and these preservation
hunks**, then replay the baseline and lifecycle tests before removing each rule.
Our History implementation and compiler policy remain product-owned.

No gfx1200 hardware, D3D12 debug layer, game acceptance, full release CI or packaging
was performed for this integration. Both architectures compile; only gfx1201 ran.
The synthetic equality tests do not establish gameplay FPS gains or eliminate every
scene-specific flicker. Upstream performance numbers above remain upstream evidence.

## Unchanged switches and recipe scope

The final official diff adds the four production groups and the reference temporal/pulse
branches listed above. Existing Options and profile assignments retain their prior values;
no deployment profile changed. Existing per-switch decisions are rechecked against the
changed consumer blobs, preserving their original classification rather than enabling
all switches in a changed header. The new norm900 recipe clones the existing FAST C32
row with only CW_NORM_HOIST=1; the fast Swin row clones the original with W2_FAST_NUM=3.
The other per-module macro values and original RowOpts are unchanged. Added PostTap is
false in ordinary exports; the product auxiliary row is opt-in. The pool64 export is
paired with the existing byte-input consumer. This is why the existing macro decisions
can retain their evidence while adding the scoped new-row validation.

## Changed-path inventory groups

Every changed path has its own content-bound decision in upstream-review.json.
The table groups research apparatus by reviewed reachability; raw result CSVs are
not repeatedly treated as separate runtime implementations. Family names below are
relative to Development/HIP/experiments or Development/results.

### norm

900-only normalization proof/benchmark harness; production selection and extra export coverage reviewed separately. See Scoped normalization and Swin. Covered paths: 515.

`c32-norm-hoist-20261006`, `c32-norm-hoist-compat-20261006`, `c32-norm-hoist-framework-20261006`, `c32-norm-hoist-stat-audit-20261006`, `h900-route-review-20261006`, `norm-contract-20261006`.

### swin

Paired FAST C256 persistent module proof; ordinary geometries retain the old module. See Scoped normalization and Swin. Covered paths: 61.

`sp-fast-coverage-20261006`, `sp1440-fast`, `sp1440-fast-20261005`.

### output

Direct output ownership and paired FP8 edge proof; no standalone harness is shipped. See Final output and pool64. Covered paths: 238.

`final-output-direct-20261005`, `pool64-byte`, `pool64-byte-20261005`.

### pulse

Driver-scoped submission-event experiment; product forces submit_pulse=0 because integrated MP/History/resize and end-to-end timing evidence is incomplete. See Excluded experiments and addon-only changes. Covered paths: 103.

`production-pulse-h-review-20261006`, `pulse-lifecycle-20261006`, `pulse-lifecycle-review-20261006`, `pulse-pdl-scope-20261006`, `pulse-production-20261006`.

### history

Addon temporal trial and original gate/feature-tap research; not the product History algorithm, and temporal experiment remains forced off. See Excluded experiments and addon-only changes. Covered paths: 139.

`history-contract-20261006`, `history-trial-041a`, `history-trial-041a-20261006`, `package-041a-20261006`, `post-history-gate`, `post-history-gate-20261005`, `temporal-package-review-20261006`, `temporal-replay-contract-20261006`, `temporal-sequence-20261005`.

### graph

Replay/DAG benchmark implementations are not referenced by the production header/recipe closure; product graph stays off. No new queue ownership path is adopted. See Excluded experiments and addon-only changes. Covered paths: 276.

`graph0-app-20261006`, `graph0-app-perf-20261006`, `graph0-app-source-review-20261006`, `graph0-bounded-replay-20261006`, `graph0-perf-20261006`, `graph0-product-baseline-20261006`, `graph0-product-baseline-review-20261006`, `graph0-product-perf-20261006`, `graph0-replay-auto-20261006`, `graph0-samework-20261006`, `graph0-samework-source-review-20261006`, `graph0-single-replay-20261006`, `hip-graph-abi-20261006`.

### local-kernels

Standalone local-fusion/register/sparse/noise-cache variants have no new production recipe or consumer in the final diff. Copying them would require new layout/ownership and image comparisons. See Excluded experiments and addon-only changes. Covered paths: 345.

`body22-down-local-20261006`, `body22-down-ownership-review-20261006`, `body22-down-regcache-20261006`, `body22-down-regcache-noescape-20261006`, `body22-down-source-review-20261006`, `body22-regcache-review-20261006`, `c32-direct-feature-20261006`, `c32-norm-transpose-20261006`, `prefix-noise-cache`, `prefix-noise-cache-20261006`, `qkv-store-butterfly-20261006`, `sparse-stage-20261006`, `up48-body48-audit-20261006`, `up48-body48-local-20261006`.

### vit

Alternative ViT indexing/math/layout probes do not change the shipped ViT recipe or consumer. Existing per-macro numeric policy is retained. See Excluded experiments and addon-only changes. Covered paths: 272.

`vit-av-trload-20261006`, `vit-column-layout-20261006`, `vit-contract960`, `vit-contract960-20261005`, `vit-den-keyperm-20261006`, `vit-layout-feasibility-20261006`, `vit-m32-grid-20261006`, `vit-m32-halfmix-20261006`, `vit-math-contract-20261006`, `vit-math-stair-20261006`, `vit-score-halfclamp-20261006`.

### mochi

Cross-backend measurement/reference captures; no Mochizuki source or deployment path is changed by this lmxxf sync. See Excluded experiments and addon-only changes. Covered paths: 144.

`fresh-mochi1088-20261006`, `mochi-driver-cache-20261006`, `mochi-old-lock`, `mochi-old-lock-20261006`, `mochi-spm-20261006`, `mochi-vit-score-control-20261006`, `mochizuki-gap-audit-20261006`.

### measurement

Timing, dispatch and comparison apparatus/results are outside production closure. Promoted output/pool/normalization/Swin changes were reviewed independently; measurement scripts themselves are not runtime features. See Excluded experiments and addon-only changes. Covered paths: 785.

`c512-den-app-preparation-20261006`, `c512-den-repeat-20261006`, `c512-den-repeat-audit-20261006`, `c512-den-typed-basis-20261006`, `combined-vs041-20261006`, `dispatch-sequence-20261006`, `framework-single-event-20261006`, `framework-submit-pair-20261006`, `fullnn-spm-20261006`, `hw-window-comparability-20261006`, `pipeline-gap`, `pool-coldage-20261006`, `real-sequence`, `submission-pacing-20261006`, `submission-untimed-20261006`, `submit-boundary-audit-20261006`, `sync-network-gap-20261006`, `sync-network-gap1080-20261006`.
