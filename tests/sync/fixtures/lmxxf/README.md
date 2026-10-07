# Historical upstream patch fixtures

The `*.raw` files contain unmodified official source bytes from
[lmxxf/dlss5-on-amd-9070xt-porting](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting)
at `297b032ac55f005d78568e684f30608651044f62`. `snapshot.json` records each
source path, fixture filename, immutable commit and raw SHA256. Inputs were
extracted with `git show <commit>:<path>`; `.gitattributes` preserves their bytes.

The active product manifest has no pins or source patches after PR #12 merged.
`patch-chain.json` independently freezes the pre-merge manifest and expected
LF-normalized output hashes from product commit
`186ecd30` (full SHA is stored in that file). These hashes were read directly
from Git, not computed by applying the patches under test.

`SourcePatchTests` verifies raw hashes, exact target coverage, `git apply --check`
and replay output against those independent hashes. The orchestrator fixtures
reuse only that verified historical result as their starting vendor, and raw
snapshots as upstream input. Thus patch preservation/conflict support remains
covered without tying today's product sources to an obsolete overlay. Separate
zero-pin/no-patch mirroring coverage verifies the active operating mode.

Do not update these frozen expectations merely because current upstream changes.
A deliberate change to historical regression data requires independent raw and
expected sources and review. Never reverse patches to manufacture their inputs.
These tests prove source transformations, not GPU or integration acceptance.
The upstream license is retained at `third_party/lmxxf/LICENSE`.
