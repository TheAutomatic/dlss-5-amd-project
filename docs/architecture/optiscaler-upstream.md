# OptiScaler upstream integration

Reviewed snapshot: **97e99b4c5d9e8af38f14e3b00e1b3f7b35ab5aed**, from
[optiscaler/OptiScaler](https://github.com/optiscaler/OptiScaler/commit/97e99b4c5d9e8af38f14e3b00e1b3f7b35ab5aed),
2026-10-06. This is a selective integration baseline, not a full upstream merge.

## Selected changes

### Follow-up snapshot (2026-10-09)

Reviewed through **434b9555d960e6e471a461c2b0d98b8e1fdacfa2**
([upstream](https://github.com/optiscaler/OptiScaler/commit/434b9555d960e6e471a461c2b0d98b8e1fdacfa2)).
This remains a selective integration; the older baseline and rounds below describe
previously integrated work.

- HWND swapchains with a zero width or height use the current window dimensions
  (`a676a402`). Both wrapped and detoured paths avoid misclassifying these as
  overlay windows. Only local descriptor copies change; CoreWindow and
  DirectComposition retain their own policies. Legacy and DESC1 sizing share a helper.
- FFX version discovery rejects failed calls, empty lists, invalid names and a
  count that outgrows its allocation (`b040666` / reviewed snapshot). It publishes
  only complete results and stops callers before indexing an empty list. This does
  not import upstream's temporary-device discovery or global spoofing scope.
- FakeNVAPI has no new API coverage in this interval. The upstream DX11 timing
  subtraction fix does not apply to our direct-forwarding implementation.

Deferred: game-follow FG/DMFG and SL1 resolver changes affect our external FG
ownership, NR-only XeFG override and per-plugin callback leases. Late latching,
projection overrides, zero-frame-ID HUD rotation and Vulkan timestamps require
resource/queue and real-game validation. Linux/DXVK paths need that platform;
GameConfigs/dllmain restructuring has no benefit sufficient to justify moving our
Unity early-wrap and per-game defaults in this update. SDK pins are unchanged.

The real descriptor helper tests and host build pass. Full CI/GPU was explicitly
outside this follow-up's validation scope; game resize/exit acceptance is still required.

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

## Round two A: DLSS/DLSSG plugin instance lifetimes (2026-10-07)

Selective source: upstream **97e99b4c5d9e8af38f14e3b00e1b3f7b35ab5aed**, design from
`4abfd7b65d5eb002b44801421837c69e04edfd6b`. DLSS and DLSSG now keep independent
resolver and returned callback identities. A rejected/new plugin cannot evict a live
older plugin. External FG bypass, OTA policy and the Steam original-function bypass remain.

The local implementation deliberately differs from the snapshot: exact Windows DLL
unload notifications only mark atomic retirement; they do not call the loader or acquire
application locks. Callback leases hold the originating module while executing. Returned
wrappers and bounded tombstones are never reassigned to another generation, including
address reuse. Sixteen successful DLSS/DLSSG instances per process are supported; subsequent
copies remain unmodified. This bounds unreclaimable detour storage after DLL unmap and
avoids writing into a freed image. Restart resets the capacity. Initialization JSON/architecture
patches do not wait across threads under loader lock; concurrent conflicting initialization
is rejected rather than racing the shared spoofed system-capability state.

Reflex/PCL/common and the host's private local DLSSG retain their existing paths in this
round. Their parameter callbacks have separate lifetimes (notably common's setVoid detour)
and need their own ownership tests before extending this registry. This is not a claim
that all Streamline plugins or the complete upstream OTA rewrite have been merged.

The host CI fixture uses real DLL exports and Detours to cover two copies, rejected attach,
repeat notification, reference decrements, active-call leases, actual unload, reload,
concurrent callbacks and capacity fallback. Streamline game startup/exit remains a game
acceptance item; the fixture does not simulate the full proprietary plugin implementation.

## Round two B: DX11 companion resize/present

Sources at the same fixed snapshot: `4c682650f32e666c89a5a7885b2b12682fc3c27e`
and `3bc197c297d8074f0793bcd33f3d382c9ff01021`. Only the confirmed equivalent
XeFG/DX11-to-DX12 path skips companion recreation. Other outputs keep the existing
SDK resize path. Width/height zero use the current HWND client size; after the game's
resize the actual description is rechecked before skipping. Unknown descriptions,
changed flags/format/count or a previous companion failure take normal resize.

DX11 wrapper calls serialize interop buffer mutation. The equivalent XeFG path first
deactivates FG, then excludes native presents, then waits for copy and present queues.
The present queue has its own fence timeline. Wait/deactivation failure returns without
releasing buffers; a device-removed fence sentinel is not successful completion. The
writer barrier is released before calling the SDK's non-equivalent resize, which can
drain/reenter its presenter. Original game HRESULTs are preserved; a companion failure
is reported on Present until a later successful resize recovers it. DirectComposition,
HDR and descriptor creation policies are unchanged.

Tests exercise the production equivalence/transaction helpers, failed waits and original/
companion failures, controlled reader/writer exclusion, and a real WARP HWND swapchain
with zero-size resize after a window-size change. This is not a real XeFG game test;
DX11+XeFG resolution changes, Alt+Tab/fullscreen and exit remain required acceptance.

## Round two C: per-call descriptor range lookup

Design source: `a671371848bd32c78d631a396c4204f1a077b751` at the fixed upstream
snapshot. The local port only caches source/destination HeapInfo ownership inside
one CopyDescriptorsSimple call. Every reuse checks active/range; existing descriptor
stripe locks, resource attach/detach bookkeeping and tracking enablement remain.
Single-descriptor calls retain original source-then-destination lookup order.

The production range helper has mapping-parity tests for boundaries, unknown/zero
sources and clearing, overlapping traversal, retirement/address reuse, and independent
calls. A controlled /O2 CPU fixture compares the same lookup/copy workload (not game
FPS): 200,000 16-descriptor copies reduce lookups from 6,400,000 to 400,000. Single-item
lookup counts remain unchanged. No persistent cache or HUD tracking rewrite is included.
