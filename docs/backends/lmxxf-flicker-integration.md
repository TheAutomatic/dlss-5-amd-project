# Flicker investigation integration status

This document separates retained fixes from optional investigation controls. Preserving
them in Git does not enable them by default or certify a release.

## Retain for integration

- Submission ownership and recording-generation tracking prevent a late completion or
  reset list from retiring/reusing the wrong NR job. Early Unity list interception and
  frame diagnostics are retained with their existing activation conditions.
- The production `FinishRecord` path rechecks split eligibility immediately before
  recording inputs. Failure reasons are copied while the proxy reference is held;
  split failures report their HRESULT and current reason. This is not a reservation:
  a later allocation/close failure still needs separate handling if observed. RTAS
  rejection is unchanged. Codec-only diagnostics retain their existing early guard.
- Native model history (`LMXXF_NR_NATIVE_TEMPORAL_TEST`) preserves separate pre/post
  reprojection, the model's fourth-row projection, and history rejection. Display
  smoothing stays outside model feedback.
- Adaptive cache invalidation keeps same-size allocations and clears validity with
  stream-ordered `hipMemsetAsync`. Seed changes must not introduce a synchronous upload
  or allocation destructor on every B frame.
- Configuration ownership stays with menu/INI. Matching runtime and GPU modules are
  required for the native post projection and exact-cache extension.

## Preserve separately from the flicker claim

- Exact ViT reuse remains an optional correctness control, default off. The user still
  observed window flicker with it enabled; it is not the no-history flicker fix.
- Model-style selection is a separate conditioning control; existing observations do
  not establish it as the reason for the earlier visual improvement.
- C32 precision experiments remain build-time opt-ins, default off. No-history
  fluctuation also occurs with the reference CUDA/exact path. Do not reopen precision
  or reuse investigations without a new concrete hypothesis.
- The rejected noise-rounding experiment is not enabled or added to the runtime here.
- F9/host tracing remains diagnostic. Do not turn it on in the generic build script
  merely to match an old investigation branch.

## Acceptance and remaining integration work

The lmxxf menu uses a `Temporal history` group with an enable switch, live status and
`Reset this group`, following the Mochizuki menu conventions. The old A/B/C/D buttons
and independent output-smoothing slider are removed. Enabling the switch selects the
former B path; disabling it selects A. Selecting either clears legacy output smoothing.
An existing nonzero smoothing preference is displayed with an explicit disable action
until the user changes it. The runtime/API field remains for recorded experiments;
this UI cleanup does not remove it or reinterpret it as Mochizuki's history strength.
The native B algorithm and current defaults are unchanged.

On 2026-10-05 the user accepted B's current observed behavior and allowed unobserved
scenes to be checked later. The existing normal session reported 6,791 Evaluate calls
without an original-frame fallback. The user's separate Mochi history on/off comparison
also supports history as the current direction. Neither proves all dynamic scenes or
all causes of NR fallback are fixed.

Slow camera motion, foliage, disocclusion/ghosting and performance across other scenes
remain deferred coverage, not a request to repeat already accepted tests. The core
no-history A fluctuation has no reliable new repair proposal.

Before merging into main:

1. Reconcile these changes with main's then-current submission/Unity/backend fixes.
2. Review optional controls separately; do not market them as window-flicker repairs.
3. Integrate the native-history build path and choose its product default explicitly.
   The investigation tree still defaults `LmxxfModelHistory` to false, and the native
   implementation still uses a test macro. This checkpoint does not change either.
4. Preserve RTAS split rejection unless a separate safety review and device test justify
   relaxing it. It is not required for accepting B's current window behavior.
5. Validate the final integrated source and matching package through the release workflow.
   Existing candidate tests are reusable only where source/artifact inputs still match.

No main merge, package deployment, or public release is implied by this status.
