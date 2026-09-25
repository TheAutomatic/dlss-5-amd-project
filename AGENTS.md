# Repository agent instructions

## Release tests (no GPU)

After merging to `release/1.9.0` or touching installer/packaging/sync, run
[tools/RELEASE-TESTS.md](tools/RELEASE-TESTS.md). `PACKAGE_RELEASE.ps1` does
not run those suites; it only checks artifact freshness.

## Synchronizing lmxxf upstream

Before changing or running `tools/sync-lmxxf-upstream.ps1`, read
`tools/lmxxf-sync/README.md` and follow its staged integration workflow.

- Work in an isolated worktree for upstream integration.
- Read the complete upstream diff, including deployment profiles, generators and experiments
  outside the vendor closure. Trace relevant options through this product's runtime, module
  selection, compile definitions and kernel consumers. Inspect the three pinned header diffs.
- Classify each new/changed switch using evidence. Do not enable or exclude a switch merely
  because of its name. Never bulk-fill the generated review template to get a passing exit.
- Integrate incrementally. Record concrete reasons, source locations, actual validation and
  actionable next steps for every deferred feature. Carry over only unchanged evidence.
- A staged source tree, successful compilation, or `--report-only` audit is not a completed
  integration review. Respect pending state and nonzero exits; rerun after real review.
- Report remaining deferrals and skipped validation. Do not claim all upstream optimizations
  are in use, or that the result is release-ready, without the corresponding evidence.
