# lmxxf recording lifecycle

The product backend negotiates ABI v2 and attaches a recording lease before appending
private commands. Each lease owns its immutable bindings and session independently of
the active backend. NR off or backend shutdown drops the active owner; closed game
recordings remain executable until successful Reset or final Release invalidates them.
Only the current whole-package ABI is accepted; old table/frame sizes and ABI v1 are rejected.

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
bounds and rejection of old ABI layouts. GPU coverage is RX 9070 XT (gfx1201); gfx1200 hardware,
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
support is part of the current package contract. `NrTimingEnabled` and `NrTimingLog` both
default to false. The host caches a non-consuming snapshot every 500 ms; explicit
file summaries are limited to one per five seconds. Means and maxima cover the
last 120 valid samples, counts are cumulative since the last enable change.

Network timing uses the event-query path from upstream fe4d1d73, ported in
bridge.patch onto the pinned product bridge. GetTimings returns the latest completed
frame without waiting and is called only by the rendering thread while measurement
is enabled. Four event pairs are allocated lazily. Toggle-off stops new event records;
old pending pairs stay owned until completion, and enable epochs reject old samples.
Event-query/record failures latch timing off for that bridge, without failing rendering.
The first completed sample after recreation is omitted from display statistics. After
three samples, the displayed network value is a median of the most recent five;
raw last/max and sample count remain separately labelled. Collapsed event spans
below 0.01 ms (observed repeatedly at ~0.001 ms) are counted as dropped samples;
GetTimings itself retains the upstream raw payload. No invented replacement value
is inserted. This rejection is a defensive display check, not a substitute for valid raw timestamps.

After recording the end event, the bridge immediately calls hipEventQuery once,
before enqueueing the external output signal and completion marker. On the tested
Windows HIP runtime, deferring this first query until a later frame produced collapsed
intervals with PDL both on and off. The immediate non-blocking query makes the pending
timestamp batch observable before subsequent submissions; both success and not-ready
are accepted. Unexpected errors disable instrumentation for the bridge. There is no
polling loop, CPU wait, new GPU dependency or change to PDL. The completion marker
remains in place. Timing is available with PDL; GetStatus reports off/n/a or a raw
completed value. Normal logging remains bounded and opt-in.

D3D12 encode (including exposure/input copies) and decode (including output copies)
use separate timestamp intervals on the actual execution queue. A session holds at
most 16 query/readback pairs. Each pair stays owned by its recording until invalidation
and completion; freed pairs are reused, avoiding per-frame committed allocations.
Replay reuses its recording's queries under existing queue ordering. Before replay,
a completed measurement may be collected; an unread pending measurement is dropped
before another write can race the CPU. Only a successful submission certificate can
publish results. Timing never extends a recording's GPU waits. Measurements from
an older enable epoch are ignored. Recordings made while disabled have no queries;
already closed recordings keep their queries until Reset/Release.

The Ins panel and existing FPS overlay read the host's cached snapshot. Values older
than two seconds are marked stale, unavailable stages show N/A, and disabling NR or
switching away clears the displayed data. Just FPS retains its original layout.
Other overlay styles show network time, with encode/decode in detailed styles.
CPU and GPU stage durations are not summed into game frame latency. UI/game visual
validation remains separate from automated timestamp/lifecycle tests.
