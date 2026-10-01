# Local and Actions build parity

The fixed 0.37 repair uses MSVC 14.44.35207, Windows SDK 10.0.26100.0 and
ordinary Release with v145. The workflow now selects the same compiler for
runtime/tests and MSBuild, checks the active environment, and pins the SDK.
Previously only MSBuild was pinned; setup-msvc-dev and tests inherited the
runner default compiler. This was a real parity gap, not proof of a GPU bug.

Both paths run the full no-GPU suites. Local release builds additionally run
device tests. CI tests a freshly built runtime and stages those exact bytes;
local builds do the same. Packaging requires the successful runtime-ci.sha256
proof and full source freshness. --fast is host compilation only and is not
release validation. Modules come from the checked-in dual-architecture package;
release.yml does not synchronize against a moving upstream branch.

## Historical failures checked on 2026-10-01

- [36343700285](https://github.com/TheAutomatic/dlss-5-amd-project/actions/runs/36343700285):
  ABI assertion expected modules_ok=58 after the package grew; fixed by 2b294be.
- [36388171199](https://github.com/TheAutomatic/dlss-5-amd-project/actions/runs/36388171199):
  packager fixture omitted check-module-contract.ps1, so four tests stopped at
  the new gate before reaching their intended assertions; fixed by da21b03.
- [36389642043](https://github.com/TheAutomatic/dlss-5-amd-project/actions/runs/36389642043):
  release upload failed with "other side closed"; verified staged retries were
  added in 4fef0c3. This was upload transport, not compilation or upstream sync.
- [36748883931](https://github.com/TheAutomatic/dlss-5-amd-project/actions/runs/36748883931):
  v1.9.8.1 / 9afa534 completed successfully. This is evidence for that version,
  not a remote validation of subsequent local lifecycle changes.

The repaired branch has not been pushed or dispatched. Its local validation
cannot certify a future hosted runner; dispatch a non-tag ref for remote artifact
validation when authorized. Both GitHub Release mutation steps remain tag-only.
Preserve module/manifest identity, submodule revisions, compiler/SDK versions and
the first failing step when comparing a future discrepancy.
