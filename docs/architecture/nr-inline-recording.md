# Experimental same-list NR execution

`[DlssNr] NrInline=true` enables the host's experimental same-command-list path
for lmxxf and Mochizuki. Default is false; ordinary command-list splitting remains
available. The control appears on the NR input page. Daniel's existing inline mode
is independent. Neither path artificially ends or restarts the game's queries.

For lmxxf, `LmxxfInlineSlots` selects 1–5 reservations (default 3). The menu only
shows this control while inline is checked. Changes affect new admission immediately:
growing admits more jobs; shrinking preserves existing jobs and refuses new ones until
occupancy falls below the new limit. No restart or model rebuild is needed. Switching
to split waits for inline reservations to retire. This does not change Daniel's
`AmdSlots` or Mochizuki's four-recording limit.

## Recording and execution

The host attaches a recording lease before appending private commands. Inputs are
captured into recording-owned resources, followed by a D3D12 capture flag. A Windows threadpool worker
samples that flag through an independent D3D12 compute queue (50 microseconds
exponential backoff to 5 ms, at most 2048 samples and a 10 s deadline); readback is read only
after that queue's fence completes. Sampling and completion use separate buffers.
No HIP/Vulkan shader is expected to see a D3D12 shader flag. Unique fence events
wake the worker; the completed fence value remains the proof. Registrations stay
owned by that recording until completion and are never reused across fence values.
Sampling backoff uses a private high-resolution waitable timer (ordinary timer
fallback), without changing the global timer period. This avoids 50-us sleeps
turning into whole Windows timer ticks.

Mochizuki's default `NrGpuHandoff` submits a bounded GPU capture watch and then
prequeues the network without CPU result verification. Cap/abort still launches the
network; its output is discarded and history reset is requested at reclaim. This
retains a known unfinished-input/history risk by explicit product decision after the
CPU verification fix regressed sustained 1440p throughput. See the
[capture-proof deferral](nr-capture-proof-deferral.md) for evidence, dormant code,
recovery instructions and research stop criteria. CPU capture mode still verifies input.
Enqueue ordering uses a condition variable notified after the preceding runtime End.
The worker acquires its enqueue turn **before** sampling capture. This applies to
both lmxxf and Mochizuki: ahead workers must not flood the shared side queue with
poll/readback work while the current frame is trying to release the game queue.
Order failure skips capture entirely and reports `inline order`; capture failure
after admission reports `inline capture`. Failure still publishes a fresh side
tail, and neither path releases a reservation without the actual game tail.

A failed game tail signal may be replaced by a later signal on the same queue. That
certificate is forwarded to runtime End; if it is not available yet, End and the enqueue
turn stay deferred through bounded collection retries. End is called only once. Failure
after the proof budget is exhausted still retains uncertain resources.

After capture verification (CPU mode) or watch submission (GPU-armed mode), the worker calls the current recording ABI's Enqueue on
the same-device side compute queue. Existing HIP/Vulkan external-fence ordering
remains: side queue input Signal, network wait/run/signal, side queue output Wait.
For lmxxf, the same compute queue first executes the recording's private pre-list
(exposure, codec, RGB input and temporal warp), then waits for HIP, executes its
post-list (history update, decode, first-pass decode and private result copy), and
only then publishes the D3D12 completion flag. Shared model weights and compute
buffers remain single-instance. Game frame N+1 can capture while N runs; no game
producer writes the shared HIP input or reads the preceding network output.
Temporal work is ordered after the preceding side post-list, including when a game
wait timed out. Each recording owns immutable colour/exposure/motion/depth captures,
result textures, descriptor bindings and command allocators. This preserves the
same network/history arithmetic as split. A side-queue shader then publishes the D3D12 completion flag. Putting the input
Signal behind the entire waiting game list would create a circular dependency;
the side queue and capture proof are essential.

For Mochizuki inline jobs, an available compute-only Vulkan family executes the
network dispatch loop. Graphics-required conversion, scaling and history work
remain on the original family. A timeline semaphore orders each pre/network/post
segment; core resources and adapter images support both families concurrently.
The final graphics fence certifies the entire chain before a slot can be reused.
Partial or unconfirmed submission fails closed and retains resources. A recording
failure after crossing a segment boundary poisons the session instead of retrying
potentially stale core layout/history state. The ordinary game-list split path
continues to use its original Vulkan queue. This changes network queue selection,
not the compute/pixel stage of the D3D12 waiting shader.

The lmxxf game list contains 32 bounded wait dispatches, each limited to 65536
iterations. Fewer dispatch/UAV-barrier pairs remove its measured extra multi-slot
scheduling loss on RX 9070 XT. Mochizuki retains 64 x 32768: its graphics/compute
segmented path regressed with longer waits. Both retain the same total loop
budget. See [slot measurements](../lmxxf-inline-slots.md) for the backend comparisons
and Mochizuki's outstanding sustained-load scheduling issue.
UAV barriers serialize slices and the final success snapshot. This is a work budget,
not a portable millisecond timeout. D3D12 does not guarantee concurrent queue progress. Bounded slices prevent an
unbounded shader loop but cannot establish a frame-time or TDR-safe bound. The frozen snapshot predicates game-list output work (lmxxf finishes its private side output independently). A host composition shader selects either that output or this frame's
original colour into a private texture (FP32 for FP32 sources, otherwise FP16). Optional first-pass output uses the
same decision. Late completion cannot change a failed frame's frozen selection.

Pipeline/root/heap bindings captured by the proxy are restored after recording.
Unsplit observed lists remain in the caller's original contiguous ExecuteCommandLists
batch; lifecycle callbacks bracket that submission without adding Execute boundaries.
An open non-timestamp query remains open across the NR commands on its original
native list; the game closes it normally. Injected compute is inside that query's
scope, just as other inline work is; preserving the scope does not remove NR's
contribution to pipeline statistics.

## Admission and enhanced barriers

Native interface escape, untracked/unsupported command state, predication, active
render passes and open legacy split barriers still refuse inline recording.
The `open_query` split restriction holds by default; `NrSplitQuerySplice` (three-state,
explicit value wins, auto = the per-game policy: on for Crimson Desert) re-bases each
open pair across the cut (producer-side EndQuery, continuation-side BeginQuery), so
both halves stay on one native list and the game's resolved result covers only the
post-split half. Query diagnostics record heap, type, index, list identity and
generation without changing query contents.

`LmxxfAllowEnhancedBarriers` initializes proxy policy during Config loading,
independently of backend construction; menu changes synchronize the same policy.
An existing recording's split rejection is not retroactively cleared.
Both backends' ordinary split paths now use the same checked input-layout scope:
known enhanced inputs transition through COMMON and are restored on the continuation.
The continuation's List4/List7 requirements are checked before closing the producer.
Shared output effects still reject enhanced recordings. Enabling experimental inline
execution remains a separate decision; it is not required for this split interop.
Any observed enhanced SYNC_SPLIT still rejects the recording conservatively.

For sources actually consumed by the selected backend, the proxy admits known
whole-resource enhanced layouts and records a transition to COMMON before legacy
runtime operations. It returns each source to COMMON and restores the original
enhanced layout/access afterward. Partial/pending layouts and observations invalidated
by aliasing refuse recording until a complete transition establishes state again.
Untouched inputs retain the caller's Frame/INI legacy-state contract, including states
established on a prior submitted list; unrelated GLOBAL/texture barriers do not invalidate
that contract. No cross-list pointer cache or inferred COMMON state is used. This does
not discover enhanced layouts established only on another list; callers must supply a
valid legacy interop state for unobserved inputs. Ordinary legacy split recordings skip
the input scan and private binding restore; enhanced recordings with only legacy inputs
also skip the private scope. Unused depth, exposure or motion inputs are not normalized.
This is deliberately not a complete D3D12 resource/subresource state tracker. Both backends
rate-limit input rejection logs with frame/list/generation, resource role, requested range,
last observed range/layout/access and invalidation status. Last observations marked unknown
are diagnostic evidence, not current-state assertions. AS Build/EmitPostbuild/Copy retain
their existing `rtas` rejection; relaxing those guards requires a separate DXR review.

Current composition admits single-sample, single-array textures with explicit
FP16/FP32, R11G11B10, R10G10B10A2, RGBA8 or BGRA8 views. Typeless and unsupported
source views pass through. A runtime's own format/geometry restrictions still apply.

## Ownership and package ABI

ABI v6 adds `LmxxfNrFrameInfo.inline_slots` (0=split, 1–5=isolated inline) and
rejects earlier packages. `LMXXF_NR_CREATE_FLAG_INLINE_QUEUE` requires recording leases.
Both host and runtimes must come from the complete current package. Old ABIs are
rejected. The lmxxf bridge's side-queue interface is an explicit optional patch
on the unchanged official pin; it does not change network modules or arithmetic.

The lease retains both side-queue completion and the actual game-list tail fence.
The bridge's separate consumer fence certifies the queue that actually consumed its
shared output: the side queue for isolated lmxxf inline, the game queue for split.
It never advances the HIP fence. The job additionally retains the real game tail;
a game tail alone cannot release a late network. Only side completion is attached
to the shared inline chain, so the next side job does not wait on a game list that
may itself be waiting for NR. A timed-out frame keeps its frozen original selection.
Mochizuki permits four reservations with its own private frame buffers.

Invalidated recordings enter the lmxxf reuse pool only after both queue proofs.
Closed, still replayable recordings keep their buffers even after execution;
replay must reacquire host admission for network execution. If capacity is full,
the completed recording instead executes its original game batch with control=2
(bypass): polling exits immediately, the frozen decision selects original colour
for both final and first-pass output, and no runtime Begin/Enqueue/End runs.
The bypass retains its private resources until the reset-side and submitted-game
tails complete; a missing/failed game signal never substitutes a side-only proof.
An overlapping replay of the same unfinished recording remains rejected because
its control buffers cannot be reset safely. The pool is bounded by the requested slot
count and trimmed on subsequent PrepareFrame calls. Retained game command lists
can therefore keep more physical buffers alive than the active admission limit.
Lowering the limit cannot free resources still referenced by those lists.

Reset/final Release during an asynchronous execution defers
runtime invalidation until the worker's End event, then collects only after both
queues finish. Enqueue failure still poisons the runtime session. The host appends a fresh
side-queue tail even when no network succeeded, and reclaims its reservation only
after both that tail and the actual game tail complete. Runtime CollectRecording
independently certifies HIP/Vulkan lifetime; failed or partially submitted network
work can still retain that runtime job. Unknown signals retain resources without
inventing completion proof. These unprovable retirements do not continually rearm
the collection timer; first-failure logging remains available. No GPU completion wait occurs under
the host recording mutex.

## Crimson Desert follow-up fixes (2026-10-11)

Each session now retains one compute side queue in its pipeline cache. New recordings
reuse that queue identity while keeping their own flags, allocators, fences and output
textures. Admission follows the backend's buffer ownership rule above. This
prevents artificial Mochizuki producer switches, history resets and retained producer
queues from growing with the number of new recordings. Actual game queue changes and
history discontinuities retain their safety checks.

The product lmxxf loader uses the same controlled HIP 7 search as our Daniel adapter: default
directories, absolute HIP_PATH/bin, then absolute PATH directories with dependency-aware
DLL flags. It does not mutate process-wide search paths or fall back to HIP 6. The explicit
`hip-runtime-loader.patch` supplies a product loader hook before any vendor header is
included, including LmxxfProductionOptions. Unmodified upstream users retain their
System32 policy. Attempts, selected path and queried version are written to
`lmxxf_runtime.log` beside the runtime DLL; load failures retain Windows error codes.

Host logs periodically report actual applied/timeout/aborted decisions, pending bypasses,
live recordings, side queue identity and the last wait slice's iteration count. These
decisions are read only after both queues complete. Mochizuki logs requested/pending
resets and missing motion, network/frame/time/geometry/pass discontinuities separately.
Counters do not suppress necessary history resets or change the GPU wait budget.

Focused validation: host CI, HIP loader fixtures (including Unicode/space paths and
same-directory dependencies), HIP injection/export validation, HIP 6 rejection, ABI
C/C++ and 23 runtime checks, host and both runtime builds. Hardware/WARP mechanism
tests retain open queries, enhanced layout restoration, replay, immutable timeout
selection and dual completion. Actual HIP 1280x720 and Vulkan 256x256 each passed 120
fresh recordings in success and forced-timeout modes with stable queue identity and
zero retained invalidated leases after collection. Vulkan 2560x1440 passed 30 fresh
recordings; non-temporal output hash `12cf1e45fb84c1b8` matched split, and zero-motion
temporal validation reported 85% history consumption including earlier reset/replay
negative cases. `tests/mochizuki/run.cmd inline` includes the large temporal fixture.

These fixtures do not certify Crimson Desert frame pacing, long-run VRAM use, all
feature combinations, ACO mode or other drivers. Game retesting remains required.

## Unsubmitted recording retirement (2026-10-12)

An inline reservation is relinquished when its recording is invalidated before
execution. Successful collection also removes any remaining unstarted reservation.
`ForgetUnstarted` leaves started resources alone; their normal dual-completion
reclaim still owns them. This prevents four discarded command lists from permanently
occupying the admission cap. `tests/lmxxf/run.cmd inline-discard` uses WARP and a mock
runtime to exercise 12 alternating real proxy Reset/Release operations, verify exact
job retirement and capacity reuse, and retain a started reservation until its
certified side-reset completion. It also checks single-slot rejection before
submission, dual-completion reuse, replay admission, and the explicit four-slot
private-buffer policy, live shrinking without dropping reservations, and expansion to five. No model or hardware GPU run is needed.

## Failure and capacity reports

lmxxf reports the first recording failure at error level for every game, including
the phase, result, execution queue and error captured on the callback thread.
The NR-off status retains that first error after releasing the backend's session
reference; a later poisoned-session error must not replace it.

Mochizuki logs requested passes, active passes, installed capacity and the deferred
build/refusal reason when that combination changes. A refused expansion keeps the
compatible network running at its existing capacity. This is reduced execution,
not proof that the requested pass count ran. The VRAM guard remains in force.
Pending history resets carry reason bits for startup, queue, network, extent,
passes, missing motion, passthrough, cancellation, external reset and enqueue failure.
The reason legend accompanies the bounded reset log; it does not change reset policy.

## Initial validation (2026-10-11)

RX 9070 XT / gfx1201: bounded success and exhaustion, original/current-frame and
first-pass selection, immutable late-completion decision, query preservation,
closed-list replay, whole-resource enhanced normalization/restoration, and retention
until both queue proofs passed. WARP exercises the same fixture.
`tests/host/nr_recording_admission.cpp` covers read-only query diagnostics and
partial/pending enhanced-layout rejection.

Actual HIP at 1280x720 and Vulkan at 256x256 matched the split baseline exactly:
`965376b3eb4e4fc8` and `e8da65152a88c7ff`, respectively, for the fixture settings.
Both passed cross-queue replay and successful Reset before GPU capture, followed
by deferred invalidation/collection. Commands:

- `tests\lmxxf\run.cmd inline` (hardware mechanism)
- `tests\lmxxf\run.cmd inline-gpu` (`LMXXF_ASSETS`, `LMXXF_TEST_RUNTIME`)
- `tests\mochizuki\run.cmd inline <MochizukiNrRuntime.dll>`

These are mechanism/fixture results, not Crimson Desert compatibility or performance
certification. Cold-start game testing, other adapters/drivers, full temporal/multi-pass
acceptance and long hot-switch stress remain outstanding. lmxxf now supports 1–5 private inline captures; Mochizuki
allows four with private buffers. See [lmxxf slots validation](../lmxxf-inline-slots.md) for the newer acceptance and performance scope. Retained recordings reacquire admission
for NR; full capacity selects the original frame without dropping the game batch.
Keep the option off by default.
