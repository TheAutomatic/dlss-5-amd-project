# Reference-network patch fixture

`hip_reference_network.upstream.h` is the unmodified
`Development/HIP/hip_reference_network.h` from lmxxf/dlss5-on-amd-9070xt-porting.

The following fixed snapshots contain identical bytes:

- `24986ae094bbd150f4d86a0ca76159a43f374884` (pending integration target)
- `f812188b9f8df92275bb15e1ed26518d5466053e` (planned integration target)

SHA256: `f06d492cdb16873e618082992a85fa9999007afad16cabaec391dfbfb636518e`.

Tests apply the maintained product patch to this raw input. It must not be
reconstructed by reversing that same patch or copied from the patched vendor tree.
This is source-only test evidence, not approval of either upstream integration.
The upstream license is retained at `third_party/lmxxf/LICENSE`.
