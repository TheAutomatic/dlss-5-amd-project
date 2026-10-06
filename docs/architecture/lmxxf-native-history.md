# lmxxf native temporal history

Native history reprojects the preceding model output into the current input grid,
then combines the new network result with a separately reprojected history using
its fourth post-projection row. The blend is `sigmoid(logit) * 0.73974609375`.
Pre motion uses nearest-depth selection; post motion uses the centre vector.
Both use normalized five-tap Catmull–Rom sampling and raw/depth rejection.
The stored feedback is the model result, before downstream output effects.
There is no separate display-smoothing control in this implementation.

## Product contract

`[DlssNr] LmxxfModelHistory` defaults to true. **Model > ViT / image reuse** exposes
it as **Temporal history (anti-flicker)**, directly before adaptive reuse, with an inline help marker.
It shares the group's reset button and the model-page/NR resets; there is no separate
history foldout or reset button. Internal runtime status remains in Diagnostics;
unavailable-history reasons remain visible alongside the compact NR status.
The host passes its explicit value in FrameInfo; no flags-file/environment alias
can override this product setting. Existing ini preferences are not overwritten.

The supported combination is a D3D12 recording lease before upscaling,
one to three passes, graph off, valid motion/depth guides, and wave-owned fused post.
Both the float block69 input and the newer E4M3-byte input are supported. Fast
numeric selection is preserved. Dormant prediction/skin options with one pass
are preserved; they do not turn a one-pass network into a multi-pass network.

Missing/unsupported guides, diagnostic views and unsupported network layouts
leave the current-frame path active and report the reason in `GetStatus`.
Multi-pass uses one final-output history chain, not one history allocation per pass.
Intermediate passes receive only the current network tensor and keep their ordinary
RGB/RGBA output path; they neither consume the final-output history nor write its
auxiliary slot. The last real network pass consumes the reprojected previous final
model frame and exports its logit. Temporal resolve runs once, after prediction or
skin blending, and stores that final result. No-history upstream consumers retain
their existing per-pass history-input behavior.

Predicted pass 3 still executes two networks: its temporal confidence is explicitly
the second real pass's logit, not a fictitious third-network projection. Skin blending
also uses the last real network's confidence. These are approximate combinations,
requiring game acceptance for trails/over-smoothing; no claim of equivalence to three
real temporal networks is made. Pass/prediction/skin changes rebuild the recording
chain and reset temporal continuity; old recorded jobs retain their original chain.
Post-SR currently lacks the guide contract. Graph remains unsupported by the
existing staged recording bridge.

History can add ghosting, soften moving detail, and increase GPU time and memory.
When enabled on a supported network, the shared post buffer grows from 12 to 20
bytes per processing pixel; three additional history/warp buffers add 48 bytes per
pixel. The pre-warp writes directly into the bridge's existing shared history
buffer, avoiding a fourth allocation and one full float4 copy per frame.
Each live recording retains its guide bindings. Control uploads/allocators are
pooled and reused only after their own GPU completion. No old benchmark is a
current universal performance estimate.

## Execution and ownership

`RecordingChain` owns history and its stable DEFAULT control buffer. Each
`RecordingJob` pins the exact motion/depth resources and its immutable descriptor
heap. Prepare/Record do not advance seed, jitter or history validity. Resource
addresses used by closed command lists remain valid through delayed/repeated
execution and through creation of a new active chain.

`BeginHistory` runs after the chain's previous consumer dependencies have been
queued. It chooses seed/useHistory and jitter from actual execution order, then
`TemporalControl` submits a small immutable upload/copy on the actual queue.
The copy has its own completion certificate, attached to both job and chain
before submission. A producer cancellation therefore cannot free that upload.
A failed control Signal leaves the job and chain unconfirmed and retained.

The pre-recorded pre/post shaders read the control buffer at execution time.
The bridge and HIP enqueue receive the same temporal decision and seed. Invalid
history is replaced with current encoded RGB by the pre shader; no temporal
sampling of uninitialized model buffers is required. Native first-frame seed
is 0; subsequent successful executions increment it. History-off stays seed 1.

End publishes pending history only after HIP enqueue succeeds, the consumer is
submitted, and its completion credential is accepted. This certifies GPU order,
not immediate GPU completion. A discard cannot publish history. Partial failed
recordings still receive valid control data before any already-recorded temporal
commands execute; their history never becomes valid.

Reset increments a session epoch checked by every execution, including old
closed recordings. Repeated/reversed frame ordinals, gaps over 500 ms, changing
chains, guide geometry/conventions, model scale, paper white or exposure scale
prime again. Normal pre-exposure changes do not reset normalized history.
Native history and adaptive ViT reuse are temporarily mutually exclusive.
Requesting history disables adaptive reuse for that network, including warm-up,
priming, missing guides and unsupported history combinations. The menu shows the
reuse checkbox as unchecked/disabled and disables its four sliders. Saved reuse
preferences and environment values remain intact; turning history off builds a
network that respects them again. The runtime enforces this independently of the
menu. Original seed invalidation and asynchronous adaptive reset remain intact;
the experimental consecutive-seed exception has been removed.

The shared pre-warp is retained by the chain and follows COMMON -> UAV -> COMMON
on every producer execution. HIP reads it behind the original producer semaphore;
the next producer still waits for the previous consumer, including across queues.
Reprojection computes its default/valid results in registers and stores each
pre/post pixel once. Sampling, precision, disocclusion checks and model seeds are
unchanged. Finish and RGB texture conversion remain separate: measured fusion
candidates did not reduce output-stage time.

## ABI and modules

The shared host/runtime table negotiates ABI 3. lmxxf FrameInfo is 160 bytes,
including guides, motion scales, jitter and temporal flags. Old ABI versions and
old frame sizes are rejected, with a whole-package update required. Mochizuki
uses the same table version but retains its separate frame structure; rebuild
both runtimes with the host. The module manifest's historical `runtime_abi=1`
is its module schema marker, not this C function-table version.

`c32_wave1_post_logit` and `c32_wave1_post_b8_logit` extend the original post
implementations without replacing their RGB expressions. The extra output is
FP32 logit plus its diagnostic half-rounded value; model blending reads FP32.
The fourth-row weights preserve the established feature ordering and are not
retuned. Missing exports refuse native history with a full-package error.

`native-post-history.patch` applies after the existing FOLLOW patches;
`history-adaptive-exclusion.patch` then preserves the network-level exclusion.
`bridge.patch` remains the pinned bridge patch. The raw 0.41 fixture includes
`wave_owned_c32.inc`; patch replay must reproduce every changed vendor file.
The upstream pin remains b687e13a8fcb8efd5be905ebbd0c9d70e15d88e3.
Only the three C32 module variants per architecture change; all other modules
retain their existing validated bytes. LLVM23.1.2 RowOpts builds and the original
provenance checker bind source, defines, scheduling flags and object hashes.

## Validation

`tests/lmxxf/run.cmd warp` includes independent five-tap numerical checks,
pre/post motion selection, depth/raw rejection, zero-output handling and reset,
plus 33 blocked/reversed control updates, cancellation and failed Signal proof.
`tests/lmxxf/run.cmd gpu` includes `lmxxf_native_history_gpu`: actual HIP history,
delayed recordings, repeated execution, cross-queue execution, reset of already
recorded commands, control-only cancellation, missing guides, history off,
old-chain replay and unsupported multi-pass fallback without changing passes.
The `--history-excludes-adaptive` variant verifies that a retained adaptive
preference does not run the reuse path while history is requested, including
warm-up, missing guides, replay/reset/cancel and unsupported passes. Turning
history off must resume actual reuse, and turning it on again restores full ViT.
The shader tests cover direct COMMON-state history writes at narrow padded,
ultrawide and 4K dimensions as well as the original separate-buffer path.

The initial dedicated runs passed on WARP and RX 9070 XT (gfx1201). Both gfx1200
and gfx1201 modules were built; gfx1200 hardware, new game acceptance and dynamic
scene/ghosting assessment remain untested. The Windows D3D12 debug layer was
unavailable, so numerical/ordering passes are not a debug-layer certification.
Final `tests/run-all.cmd --tier ci,device` passed on the matching artifacts,
including host/config, shader, both runtime ABIs, WARP, installer, complete sync
and hardware list/Unity admission tests. The CI receipt binds lmxxf runtime
SHA256 `a5b62235c02ea6fbff3069d4ae8cdf06865c938ee046ed317bcee9fa98821d97`.
The complete lmxxf GPU tier passed; the fast-numeric native-history replay suite
also passed. A focused old-module check refused history with the full-package
update error, and the final old-FrameInfo format-rejection test passed.

Only gfx1201's default b8 production path and fast-numeric history were exercised
on hardware. Float post exports were built and resolved, but their complete GPU
matrix was not run. Mochizuki was rebuilt for ABI3 and passed ABI tests; its
Vulkan model/GPU matrix was not run. No new package, game installation, remote
Actions run or main merge is implied by these local validation results.

An instrumented synthetic 1280x720 bridge run measured completed input/output
command-list spans. Its overall span was too variable to establish a reliable
end-to-end history cost; it is not a game or network benchmark. Dynamic-scene
acceptance and a stable in-game cost comparison remain follow-up work.

### 2026-10-06 performance follow-up

An experiment allowed consecutive history seeds to retain the adaptive cache.
Static synthetic 1080p timing on RX 9070 XT improved from about 9.5 to 8.2-8.3 ms
network time, and offline execution/gate checks passed. Yimo game testing then
showed severe flicker with both features enabled, disappearing when adaptive
reuse was disabled. This rejects the experiment for product use; the speedup is
not available under the current exclusion policy.

Investigation remains open: seed changes alter the model noise, and the previous
model output is fed into the next inference. Coarse image/token thresholds do not
bound final temporal error; approximate/full refresh differences may feed back
into history. This is a hypothesis, not a per-frame diagnosis. Any future attempt
needs dynamic-sequence output-error/refresh measurements and game validation of
flicker and ghosting. Static hashes, speed or correct submission ordering alone
cannot establish compatibility.

The independent direct-history and single-store optimizations remain: measured
input stage about 0.63 -> 0.47 ms and one fewer 33.75 MiB buffer at 1920x1152.
They retained output hashes with adaptive off in both numeric modes and passed
WARP/gfx1201 numerical, replay/reset and 4K coverage checks. No full release CI
proof or game acceptance is implied; older receipts do not certify later changes.

### Why native history excludes adaptive ViT

Adaptive ViT approximates blocks 31–38 as an anchored output plus a per-channel
linear gain times the input delta. Its token L1 and raw RGB tile-mean thresholds
do not bound final RGB/logit error or align cached tokens to motion. Native history
changes both the seed-dependent prefix noise and the reprojected RGB prefix.
Its final blended output becomes the next frame's model input, so approximation
and hard reuse/refresh transitions can feed errors back into subsequent frames.
The final blend weight being below 0.74 is not a stability bound for that full loop.

A continuous-seed cache experiment passed static execution tests but produced severe
in-game flicker; that exception has been removed. Current production retains full ViT
whenever history is requested, including warmup and missing-guide fallbacks, without
overwriting saved reuse preferences. Future compatibility needs dynamic-sequence
comparison against full inference, RGB/logit error measurements and game validation;
threshold tightening or fixed seeds alone do not establish correctness.

## 1.10.4 final-pass extension

The host/INI switch and ABI are unchanged. `RunGraph` receives an explicit final-pass
marker; auxiliary output is emitted only for its final RGB output. Intermediate
RGBA kernels stay usable. NativeHistorySupported/SetMultiPass permit MP1/2/3 without
changing the separate upstream experimental-feature-tap restriction.

The GPU sentinel fixture verifies intermediate RGBA leaves auxiliary memory untouched,
final RGB writes every float, and an auxiliary-enabled Network accepts live pass changes.
Runtime regressions cover MP2 history activation, MP3 prediction/real modes, skin blend,
old-chain replay, cross-queue execution, cancellation and restoration to MP1.
