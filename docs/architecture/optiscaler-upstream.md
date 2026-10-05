# OptiScaler upstream integration

Reviewed snapshot: **97e99b4c5d9e8af38f14e3b00e1b3f7b35ab5aed**, from
[optiscaler/OptiScaler](https://github.com/optiscaler/OptiScaler/commit/97e99b4c5d9e8af38f14e3b00e1b3f7b35ab5aed),
2026-10-06. This is a selective integration baseline, not a full upstream merge.

## Selected changes

- NVAPI DRS DLSSG OTA protection (`ed30706`): only the output owned by this host,
  with the external-frame-generation bypass and DWORD validation preserved.
- DXGI factory calls use our existing thread-local D3D creation scope. No global
  Vulkan suppression is introduced.
- DX11/DX12 companion descriptors share format, sample, buffer-count, stereo,
  usage and tearing checks across detoured and wrapped factory entry points.
  Only copied companion descriptors change. Unsupported sRGB formats fall back
  instead of silently changing color interpretation; the private windowed policy remains.
- Companion swapchain state records the FG chain and descriptor. The existing
  wrapper owns the reference and clears the borrowed state pointer on release.
- ResizeBuffers1 falls back before releasing resources if IDXGISwapChain3 is absent.
- HUD capture barriers use the caller's known state on both sides of CopyResource
  (`b9b0bec`), matching the existing CopyTextureRegion branch.
- Existing quirk mappings extended for Granblue Fantasy Relink, Trails in the Sky
  2nd Chapter and Sword and Fairy 7. The existing config precedence remains in force.

## Deliberately separate work

FakeNVAPI call tables and implementations already match this snapshot. Low-latency
timestamp renaming has no functional benefit here. FidelityFX SDK and XeSS pins
already match; this change is not an SDK upgrade.

Keep our DirectComposition, zero-size/window sizing, HDR, NR command-list tracking,
external FG ownership, and private Vulkan scopes. Upstream does not contain all of them.
Do not replace ConfigKeys, dlssnr, menu_common, or the main lifecycle hooks wholesale.

The D3D12 hook rewrite (`500ed33`), DLSS bridge lifecycle, Streamline/FG reprojection,
new Hudfix resource tracking and equivalent-resize skipping require separate analysis.
In particular, zero ResizeBuffers dimensions mean current window dimensions, and the
new XeFG resize path depends on matching present synchronization; copying only its
skip predicate would be unsafe. These need dedicated lifecycle and game validation.

The existing Unity resource flip quirk is game-specific, not a general Unity fix.
Retain the separate Aniimo NR early-wrap policy. UI changes use local components;
upstream's menu redesign is reference material, not a replacement for the NR workflow.

## Verification

The host CI entry includes descriptor acceptance/rejection and unchanged-on-failure
tests plus existing cross-thread Vulkan scope tests. Build and CI establish software
contracts; game startup, HDR and Alt+Tab/resize behavior still require game acceptance.
