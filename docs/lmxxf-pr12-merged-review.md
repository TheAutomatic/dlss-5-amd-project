# PR #12 merged: lmxxf source unpin review (2026-10-07)

## Revision and source identity

Official range: `297b032ac55f005d78568e684f30608651044f62` to
`48a41fccb89300cd6636b16bc7b86010384c4cc1`. Product baseline: `186ecd30`.
PR [#12](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting/pull/12) was merged
as `b3d05ab34beaea97fa7062b20e19f76398c287fd`; its tree equals the final PR head
`3ef0b6e98c3db19a2477998e755471ebe3507851`. No submitted interface was omitted.
The sole later commit, 48a41fccb, fixes default RGB resource state and restricts
the author's opt-in addon History scope. It does not remove our interfaces.

The 79 owned headers, HIP sources/includes/build files and HLSL files exactly
match the target's Git blobs after CRLF-to-LF normalization only. Of these, 75
also equal the already-integrated product baseline. The four differences are
listed below. `hip/SHA256SUMS` shipping rows and runtime/module manifests are
locally generated artifact metadata and are checked separately.

`pinned=[]` and `local_patches=[]`. The bridge is an ordinary required header.
The optional `-UpdateBridge` preservation mechanism remains available to old
manifests/tests but is not involved in current source synchronization.

## Reusable interfaces and product policy

The official diff includes previously integrated PR interfaces in
`Development/HIP/hip_api.h`, `hip_d3d12_bridge.h`, `hip_reference_network.h`,
`src/native_game_codec.h`, `native_game_rgb_input.h`, `native_rgb_texture.h`,
`native_shader_cache.h` and both codec HLSL files. They provide failed-load
cleanup, PDL status/preflight, cancellation/zero output, producer/completion
proof, recording leases, direct input/History, typed footprints, active extents,
explicit color policies, optional compiler binding and final-pass post logits.
The per-instance options for hotkeys, polling, extra skips and module selection
retain the upstream defaults when not supplied. Product usage is unchanged from
[the migration review](lmxxf-pr12-interface-migration.md#zero-patch-readiness-2026-10-07).

The complete bridge header equals both raw upstream and the product baseline.
Neither fence order nor the shared COMMON/producer-wait/consumer-signal contract
changes. Callbacks retain their existing lifetimes and capture policy; no new
queues, locks, ABI members, allocation loops or logging are introduced locally.
History/ViT exclusion, INI/ConfigKeys/EnvAlias priority, product numerical defaults,
System32 compiler policy and supported FAST module selection remain local.

`src/LmxxfNrRuntime.cpp` in the author repo changes its consumer to
`BufferFootprint()`; it is not our C ABI runtime and is not vendored. Our runtime
retains its own format/row-pitch copy path. Copying his standalone runtime
would erase product recording/configuration policy and is deliberately excluded.

## Four differences from the integrated product

| Path | Difference and disposition |
| --- | --- |
| `Development/HIP/hip_reference_network.h` | Documents final-real-pass logits and selected-module Hrtz diagnostics; rejects auxiliary History in `HIP_MP_RAW_EXPORT` builds. Product does not define that diagnostic macro. Normal MP1/2/3 scheduling is unchanged. |
| `src/native_game_rgb_input.h` | Always returns private RGB/tiles from UAV to NSR after dispatch. The old condition omitted the first legacy call; product already opts into replayable recording, where the barrier was emitted. External output restoration is unchanged. |
| `src/native_hip_network.h` | Addon wrapper calls `RequireSinglePass` before creation and hot-setting mutation when its Fast History consumer is enabled. Product instantiates `D3D12Bridge` directly and is not subject to this addon restriction. |
| `src/native_fast_history_policy.h` | New required include for that wrapper, added to the source manifest. Throws on addon Fast History with non-MP1; it does not restrict the reusable auxiliary interface. |

The changes neither fix nor worsen the known product predicted-third-pass History
feedback problem by themselves. Static GPU determinism is not evidence of motion
stability. Real MP2/MP3 History and predicted MP3 must remain distinct test cases.
No change to saved settings or mitigation for that separate problem is made here.

## Author addon and deployment scope

`src/native_addon_fast_history.h`, `native_fast_history.h`,
`native_fast_history_support.h`, `native_game_frame.h`, `native_game_oneshot.h`,
`native_pre_upscale.h` and `native_temporal_experiment.h` form the author's
standalone addon consumer. They are not in the product source closure. His
reusable shader approximation and external coefficient loading do not replace
our `NativeTemporalHistory` implementation or embedded model coefficients.

New `DLSS5_FAST_HISTORY` is explicitly zero in `scripts/hip-game-flags.txt`.
`DLSS5_FAST_HISTORY_DEPTH_INVERTED` has only commented examples: the addon must
receive explicit depth convention and unjittered motion metadata. Final addon
scope is FFX pre-upscale, MP1, full viewport, graph off, overlap off and no
reference temporal experiment. Motion mip zero must exactly match the render
extent; incompatible guides clear temporal validity. None of these profile
switches is installed or read as a product History control. Both new flags are
classified as excluded addon experiments with concrete consumer evidence.

`scripts/CONFIGURATION.md`, `CONFIGURATION.zh-CN.md`, `Development/DevHistory.md`
and `WorkingPlan.md` describe these interfaces and the addon opt-in; they are
reviewed documentation, not imported defaults. Existing environment/config
entries retain their prior per-item decisions: the product policy/consumers are
identical to baseline; addon-only call sites above gained only the separate Fast
History path. Excluded reference History, input polling, IO fusion and hotkeys
are not enabled by removing the overlay. Submit-pulse deferrals remain open.

## Kernels, compiler recipe and module provenance

The only upstream-range module differences are `hip/wave_owned_c32.inc` (the
already-integrated optional float/b8 logit export) and the UTF-8 BOM in
`hip/build-modules.ps1`. Every current `.hip`, `.inc`, `rtc_compile.cpp`, recipe
and `Development/HIP/swin_persistent_types.h` equals product baseline. Thus the
changed whole-file fingerprints of C32 macros/compiler rows do not represent a
new numerical rule or compiler option; their existing individual decisions are
retained with this exact-source evidence, not blanket-enabled.

The supplied dual-architecture bundle is the previously built/validated
PR12-integration bundle: 40 modules per gfx1200/gfx1201, six LLVM23.1.2 RowOpts
rows and 34 COMGR rows each, with only the same two LINE_STORES build arguments.
Source and module-header/recipe identity justify reuse rather than a rebuild.
The normal sync checks the complete supplied package and source-bound review,
refreshes generated shipping metadata and checks all hashes. Provenance checks
remain mandatory after removal of source patches.

## Upstream tests and historical patch regression

The official range adds/updates `Development/HIP/test_module_load.cpp`,
`test_multipass_aux.cpp`, `d3d12_codec_integration_test.cpp`,
`fast_history_fixture.h`, `test_addon_fast_history.cpp`, `test_fast_history.cpp`,
`test_fast_history_scope.cpp`, `test_rgb_input_state_contract.py`,
`test_codec_integration.ps1`, `test_integration_interfaces.ps1`,
`codec_integration.md` and `integration_interfaces.md`. They exercise reusable
contracts, addon opt-in and default preservation, rather than product deployment.
No tests or experimental implementations are silently promoted into the runtime.

All seven files under `Development/results/pr12-integration-20261007/` are the
author's receipt/logs. The receipt records MSVC/WARP/AMD checks and the default
RGB barrier fix. Its optional full-network suite lacked Assets/Modules; it is
not fresh full-network auxiliary validation. Our prior PR head suite did run
those modules: seven no-aux output hashes matched the earlier PR baseline,
auxiliary MP/skin/prediction schedules passed, and MinGW compilation passed.
The merge-tree equality establishes these are the accepted submitted sources.

Historical patch support is preserved using a frozen manifest and independent
expected output hashes in `tests/sync/fixtures/lmxxf/patch-chain.json`. Expected
hashes come directly from product commit 186ecd30, never from patch application.
Raw inputs remain official 297b032a. Current upstream edits no longer invalidate
a test whose purpose is replaying the old overlay. Sync still tests missing
headers, patch conflicts, stale reviews, modules and zero-pin mirroring.

## Validation and limits

The normal sync completed with no skipped checks and moved the completed revision
to official 48a41fccb. The unchanged 80-module bundle passed full dual-architecture
package checks; no module rebuild was necessary. Fresh MSVC runtime SHA256:
`550b988a2aa02b9959a2792ed6257c1d51d2da405b7b8142b2162f10b6df495f`.

- `tests/lmxxf/run.cmd abi`: C/C++ ABI and exports, plus 23 runtime validation
  tests passed against that DLL.
- `tests/host/test_config_priority.py`: all seven cases passed; the MSVC-dependent
  independent-CRT test was rerun in the developer environment after the initial
  plain-shell invocation correctly skipped it.
- `tests/lmxxf/run.cmd warp`: passed, including temporal controls/numerics,
  compiler/codec checks, ordering, leases, replay and same-frame boundaries.
- `tests/lmxxf/run.cmd gpu`: full tier passed on RX 9070 XT / gfx1201 against that
  DLL, including output-hash baselines, actual multi-pass auxiliary/History,
  History/ViT exclusion, 900-tier normalization, resize/subrect/ultrawide, replay,
  cancellation, module failure cleanup and producer/completion proofs.
- D3D12 debug layer was unavailable. Executable assertions, GPU output comparisons
  and submission checks passed; no debug-layer-clean claim is made.
- Final 612-item enablement audit passed; the two existing submit-pulse deferrals
  retain their queue/History/resize/timing validation follow-up.

- Final `tests/sync/run.cmd --force`: passed (70 synchronization tests plus Git
  attributes, compiler provenance, packaging, cache and release-exit regressions).
  A later fixture README filename correction is documentation only; no tested
  implementation or fixture bytes changed. No duplicate full suite is needed.

No game run, deployment, package, tag or release preparation is requested.
No claim is made of gfx1200 hardware validation or predicted-History stability.
Source synchronization does not imply enabling every upstream optimization.
