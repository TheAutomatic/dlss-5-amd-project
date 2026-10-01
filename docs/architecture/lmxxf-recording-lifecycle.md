# lmxxf recording lifecycle

The product backend negotiates ABI v2 and attaches a recording lease before appending
private commands. Each lease owns its immutable bindings and session independently of
the active backend. NR off or backend shutdown drops the active owner; closed game
recordings remain executable until successful Reset or final Release invalidates them.
ABI v1 retains its original 136-byte function-table prefix and legacy serial behavior.

## Submission events

`CommandListProxy` assigns a stable list identity, a recording generation, and an execution
serial. Successful Reset and final COM Release invalidate the exact recording generation.
Close, failed Reset, elapsed frames, and successful submission do not invalidate it.
The recording owns its observer; the observer must not retain the proxy COM object.

Record/Execute/Reset/invalidation use `RecordingMutex`. Observers may transfer ownership
under that lock, but must not wait for GPU completion. Final proxy destruction unlocks
before the logical-list destructor can wait. Runtime/session destruction runs on a Windows threadpool callback with the DLL pinned
until callback return; HIP destruction selects the original adapter on that thread.

Each execution reports actual producer and continuation submission separately from the
Signal HRESULT. Fence pointers in callbacks are borrowed; retained credentials require
AddRef. A failed Signal leaves submitted work unconfirmed, and later execution cannot
silently certify it. Cross-queue replay waits for the previous consumer fence before
submitting its producer. UINT64_MAX is not normal completion; confirmed device removal
has a separate teardown path.

## Replayable resources

Codec, RGB input and RGB output private resources start in NON_PIXEL_SHADER_RESOURCE.
Each recording uses the same NON_PIXEL_SHADER_RESOURCE -> UAV -> NON_PIXEL_SHADER_RESOURCE
transitions. `PinRecording` captures the exact current resource, descriptor heap, root
signature and PSO references. A closed recording must retain these even after its first
GPU execution completes. Merely retaining a codec object is insufficient when its binding
cache replaces a heap.

`ExposureMeter::Record` accepts an optional `ExposureRecording` lease. This path allocates
an immutable descriptor pair and retains the colour, exposure value, root and PSO.
An occupied lease cannot be overwritten. Legacy callers retain their original ring path;
the v2 runtime Job supplies and owns this immutable lease.

## Explicit bridge mode

A fresh graph-off bridge can opt into `EnableRecordingLeases`. After recording its output
readers, `SealRecordedOutput` returns shared output to COMMON and seals CPU recording
state. An unsubmitted recording may then be discarded without inventing a GPU execution.

For every execution, call `BeginRecordedExecution` before the producer. It validates the
actual same-device queue and orders cross-queue execution after the prior consumer.
Submit producer, call `EnqueueAfterProducer`, submit the recorded consumer, then call
`EndRecordedExecution` with actual submission facts. A completed HIP execution without a
submitted consumer is an execution discard; it does not invalidate the recording lease.
An unconfirmed consumer Signal or failed enqueue prevents reuse and ordinary reclamation.
The legacy staged API retains its original serial, non-replay contract.

## Runtime ownership and failures

Jobs use monotonic opaque tokens; stale tokens are looked up, never dereferenced.
Token exhaustion fails permanently instead of wrapping. Prepare publishes the token only
after ownership transfer succeeds. Each job retains its original bridge across geometry
changes and pins exact codec resources rather than a mutable binding-cache slot.

Begin/End surround each actual proxy execution. The actual DIRECT queue must belong to
the session device. Completion certificates retain their queue and fence; collection
requires both invalidation and confirmed completion of every execution. Missing/failed
Signal proof remains unconfirmed even after another fence completes. Confirmed device
removal has a distinct result. Unconfirmed resources are intentionally retained.

Partial failed recordings retain their observer and private pins. They may execute the
already-recorded commands without HIP; the failed active owner is retired from new work.
A threadpool timer collects invalidated completed leases even when no backend is active.
The registry and observers never own a COM reference back to the proxy.

## Validation

The WARP tier exercises real proxy Reset/Release, failed Reset, delayed recordings,
replay, cross-queue ordering, Signal failure and concurrent Execute/Reset. The exposure
lease test records 33 distinct bindings, destroys the exposure owner, then executes in
reverse order and replays with numerical readback and D3D12 debug checks.

The AMD GPU tier exercises actual HIP inference with nonzero input, recording discard,
post-HIP consumer discard and cross-queue replay with byte-identical nonzero output.
Formal patches reproduce the independent upstream fixtures; the Git roundtrip suite
checks both all shipping HIP binaries and raw patch/fixture evidence.

The runtime GPU test covers ten delayed bindings, stale handles, replay across queues,
geometry changes, exposure, passthrough/HIP ordering, and another live session. Real
SessionOwner + CommandListProxy tests cover NR off/on, Reset while GPU work is blocked,
background collection, and partial recordings without HIP. Fault tests reject the wrong
execution queue and retain work with failed Signal proof. ABI tests cover old prefix
bounds and v1/v2 negotiation. GPU coverage is RX 9070 XT (gfx1201); gfx1200 hardware,
real-game hot switching, and forced physical device removal remain untested.

## Pure-backend startup policy

With NrConvenience=0 and lmxxf selected, AmdGraphicsWait does not install Daniel
state-tracking or CreateCommandSignature metadata hooks. Early and late hook
installation use the same latched startup policy. The lmxxf proxy/split/fence
path remains enabled. Disabled tracker OnCreate notifications return without
locking, and the NR envelope skips Daniel snapshot/pin/replay preparation while
retaining generic compute restoration. Daniel is still created only when selected.

With convenience enabled and Daniel installed, capture can be prepared for a
later switch; Daniel startup still honors AmdGraphicsWait. If capture was armed
but new wait is turned off live, render-pass safety observations remain active.
Changing NrConvenience in the menu saves the next-launch value without changing
the running session's backend policy; restart is required.

## Optional timing

`LmxxfNrGetTimingApi` negotiates a separate versioned timing table. Missing timing
support leaves rendering ABI v1/v2 intact. `NrTimingEnabled` and `NrTimingLog` both
default to false. The host caches a non-consuming snapshot every 500 ms; explicit
file summaries are limited to one per five seconds. Means and maxima cover the
last 120 valid samples, counts are cumulative since the last enable change.

The v2 execution path records HIP events after the input semaphore wait and around
network enqueue, including its final output buffer copy. This is a GPU stream span,
not isolated kernel busy time or end-to-end frame latency. Eight event pairs are
reused only after the existing output fence proves completion; a full pool drops
telemetry without waiting. Failed enqueue/signal leaves reserved events owned by
the bridge until its existing safe teardown. Timing failures do not fail rendering.
Enable epochs prevent older pending samples from repopulating a restarted window.
Legacy v1, codec passthrough and unexecuted recordings have no network GPU sample.
The standalone synchronous development probe remains separate and is not enabled
by product telemetry. Game performance still requires an external off/on comparison.
