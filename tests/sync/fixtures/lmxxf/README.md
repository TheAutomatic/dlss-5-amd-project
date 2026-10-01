# Frozen upstream patch fixtures

The `*.upstream.*` files are unmodified source bytes from
[lmxxf/dlss5-on-amd-9070xt-porting](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting)
at the immutable commits recorded in `snapshot.json`: FOLLOW inputs at
`c809efb0ea2960f148624730898da61b8fb55a45`, and the separately pinned bridge at
`54e14de503431cd4536f8a7151b022af232178a9`.
`snapshot.json` records each upstream path, fixture filename and raw SHA256.
The snapshots were extracted with `git show <commit>:<path>`; `.gitattributes`
preserves their LF line endings on Windows.

`SourcePatchTests` in `tests/sync/test_upstream_sync.py` validates the hashes and
coverage of every active patch target in `tools/lmxxf-sync/manifest.json`.
It then checks and applies the bridge patch followed by every `local_patches`
entry, in order, in a temporary source tree. The result must equal the current
vendor files after checkout line-ending normalization. The orchestrator tests
also use these raw inputs in their temporary upstream repositories.

When the upstream baseline or maintained patches change, collect the affected
raw files from an immutable upstream commit and update `snapshot.json` after
reviewing the source diff. Never reconstruct inputs by reversing the tested
patches or copy them from the patched vendor tree. Keep patch-conflict checks
and output comparisons intact; they detect stale patches and unrecorded local
changes.

These fixtures validate source transformations only. They do not approve an
upstream integration or validate GPU behavior. The upstream license is retained
at `third_party/lmxxf/LICENSE`.
