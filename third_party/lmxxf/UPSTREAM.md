# lmxxf runtime source revision

- Upstream: https://github.com/lmxxf/dlss5-on-amd-9070xt-porting
- Commit: `48a41fccb89300cd6636b16bc7b86010384c4cc1` (synced 2026-10-07)
- License: MIT, Copyright (c) 2026 Kien (`LICENSE`)
- `hip_api.h` also carries the AMD HIP runtime API MIT notice from ROCm 7.1.1.

This is a vendored source closure, not a Git submodule. The commit above is the
last completed integration; `sync-state.json` records any pending attempt.
[Sync workflow](../../tools/lmxxf-sync/README.md) is the entry point for updates.

## Current ownership after PR #12

PR #12 was merged in `b3d05ab34beaea97fa7062b20e19f76398c287fd`, including the
final-pass auxiliary interface. The current integration targets its corrective
follow-up `48a41fccb89300cd6636b16bc7b86010384c4cc1`.
[Review, exact-source proof and validation](../../docs/lmxxf-pr12-merged-review.md).

`tools/lmxxf-sync/manifest.json` has no pinned files and no local source patches.
All selected headers (including `hip_d3d12_bridge.h`), top-level HIP sources and
includes, and top-level HLSL follow the resolved official SHA. Missing required
files fail before vendor mutation. New dependencies must be reviewed and added
to the manifest. Historical patches are independent regression fixtures only.

| Owner | Responsibility |
| --- | --- |
| Upstream source closure | Network, bridge, codec, compiler callback and opt-in auxiliary interfaces; no preservation overlay |
| Product `dlssnr/backend/lmxxf_runtime/` | C ABI, INI/menu policy, compiler binding, recording lifecycle, exposure, History algorithm and model coefficients |
| Product `LmxxfProductionOptions` | Explicit interface permissions, module selection, extra skips; no change to upstream defaults |
| `modules/` and `hip/SHA256SUMS` shipping rows | Locally verified 40 modules per architecture (80 total); generated binary metadata is not copied from upstream Git |
| `module-defines.json` | Two reviewed LINE_STORES build arguments; no source patch |

The LLVM23.1.2/COMGR split, RowOpts, scoped 900 normalization, 1440 Swin and
pool64/output routes remain unchanged. Module provenance and review are still
mandatory. Zero source pins does not mean arbitrary future revisions are safe
without review or that every upstream experiment is enabled.

## Intentionally separate consumers

The product does not instantiate the upstream ReShade/MinHook addon, FFX replay,
`NativeGameFrame`, `NativeGameOneshot`, upstream C ABI runtime or D3D12 network.
Their host lifecycle, logging, profiles and deployment scripts are not copied.
`DLSS5_FAST_HISTORY` is the author's opt-in addon policy (MP1), not our product
History switch. The product retains its own multi-pass History and History/ViT
exclusion; reference History, feature taps and submit pulse remain disabled.

The mandatory audit inventories the official final diff and requires decisions
bound to source and local consumers. Only after review and requested verification
succeed does the completed commit advance. `-SkipEnablementAudit` stages only.
Source integration is not package, game or release acceptance.
