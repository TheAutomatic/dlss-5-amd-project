# Repository agent instructions

## Where things go

Read [docs/workspace.md](docs/workspace.md) before creating any file outside product code.

- Throwaway scripts and output: `work/scratch/` (purged after 14 days). Never the repo root,
  the root of `tools/` or `tests/`, `exports/` or `dist/`.
- Investigation write-ups: `work/notes/YYYY-MM-DD-topic.md`; once settled, distil into `docs/`.
  Decisions go to `docs/decisions.md`.
- Game/user logs: `work/logs/<game>/<date>/`. Anything that must not be public: `work/private/`.
- Handoff: `work/handoff/HANDOFF.md`, at most ~80 lines; sections older than 7 days move to
  `work/handoff/archive/`.
- Intermediate build/test output goes in `exports/`. All local test packages and release packages
  (staging directories and zip files) go in `dist/`. Preserve other versions and third-party inputs.
- New tools go in a `tools/<group>/` folder; new tests go in `tests/<area>/` and must be wired
  into that area's `run.cmd`.
- Tracked files must not depend on files under `work/`. `analysis/`, `.analysis-tools/` and
  `.handoff/` are retired: do not recreate them.

## Release tests (no GPU)

Before release preparation, read [docs/release.md](docs/release.md), the single release workflow.
Use an explicit host path and `-OutDir dist` for local packaging, matching the packager default.
A local package in `dist/` is not a published release; record its validation status separately.
A standalone ABI pass or `--fast` build does not replace full CI or its runtime hash proof.

After merging to `release/1.9.0` or touching installer/packaging/sync, run
[tests/RELEASE-TESTS.md](tests/RELEASE-TESTS.md) (`tests\run-all.cmd --tier ci`).
`tools/release/PACKAGE_RELEASE.ps1` does not run those suites; it only checks artifact freshness.

## Synchronizing lmxxf upstream

Before changing or running `tools/sync-lmxxf-upstream.ps1`, read
`tools/lmxxf-sync/README.md` and follow its staged integration workflow.

- Work in an isolated worktree for upstream integration.
- Read the complete upstream diff, including deployment profiles, generators and experiments
  outside the vendor closure. Trace relevant options through this product's runtime, module
  selection, compile definitions and kernel consumers. Inspect the pinned bridge header diff.
- Classify each new/changed switch using evidence. Do not enable or exclude a switch merely
  because of its name. Never bulk-fill the generated review template to get a passing exit.
- Integrate incrementally. Record concrete reasons, source locations, actual validation and
  actionable next steps for every deferred feature. Carry over only unchanged evidence.
- A staged source tree, successful compilation, or `--report-only` audit is not a completed
  integration review. Respect pending state and nonzero exits; rerun after real review.
- Report remaining deferrals and skipped validation. Do not claim all upstream optimizations
  are in use, or that the result is release-ready, without the corresponding evidence.

## Config priority (code, not tribal knowledge)

Ins menu / `OptiScaler.ini` win. `native-game-flags.txt` and external `DLSS5_*` only fill
keys the host did not set (`ApplyFlagsFileFallback` in `LmxxfNrRuntime.cpp`). Compile
defaults in `LmxxfProductionOptions` are last. Do not invent a second order; add product
keys to Config/menu so they are owned by ini, not by txt.

Config identifiers live in `ConfigKeys.h` (`CfgKey::`). Menu **labels are UI-only** — never
write an ImGui label into the ini. New menu controls: add the ini key to `CfgKey::kKnown`
first, bind `Config` fields (or `CfgKey::` names), then a display string. Product keys that
must win over `DLSS5_*` need an `EnvAlias` + `PutEnvAlias`.
