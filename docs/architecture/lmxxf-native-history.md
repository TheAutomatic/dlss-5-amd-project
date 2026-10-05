# lmxxf native temporal history

Native history reprojects the preceding model output into the current input grid,
then combines the new network result with a separately reprojected history using
its fourth post-projection row. The blend is `sigmoid(logit) * 0.73974609375`.
Pre motion uses nearest-depth selection; post motion uses the centre vector.
Both use normalized five-tap Catmull–Rom sampling and raw/depth rejection.
The stored feedback is the model result, before downstream output effects.
There is no separate display-smoothing control in this implementation.

## Product contract

`[DlssNr] LmxxfModelHistory` defaults to false. The Pipeline model controls expose
it as **Temporal history**, with a reset-to-default action and runtime status.
The host passes its explicit value in FrameInfo; no flags-file/environment alias
can override this product setting. Existing ini preferences are not overwritten.

The first supported combination is a D3D12 recording lease before upscaling,
one network pass, graph off, valid motion/depth guides, and wave-owned fused post.
Both the float block69 input and the newer E4M3-byte input are supported. Fast
numeric selection is preserved. Dormant prediction/skin options with one pass
are preserved; they do not turn a one-pass network into a multi-pass network.

Missing/unsupported guides, diagnostic views and unsupported network layouts
leave the current-frame path active and report the reason in `GetStatus`.
Two/three passes retain the configured pass count and report
`history=unsupported-passes`; they do not secretly become one pass. Predicted
third-pass/skin output needs a separate per-pass temporal contract, so it is not
given the last real pass's history weight. Post-SR currently lacks this guide
contract. Graph remains unsupported by the existing staged recording bridge.

History can add ghosting, soften moving detail, and increase GPU time and memory.
When enabled on a supported network, the shared post buffer grows from 12 to 20
bytes per processing pixel; the four history/warp buffers add 64 bytes per pixel.
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
The existing asynchronous adaptive-state reset is retained.

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

The initial dedicated runs passed on WARP and RX 9070 XT (gfx1201). Both gfx1200
and gfx1201 modules were built; gfx1200 hardware, new game acceptance and dynamic
scene/ghosting assessment remain untested. The Windows D3D12 debug layer was
unavailable, so numerical/ordering passes are not a debug-layer certification.
Full integration suite results are recorded when the final artifacts finish.
