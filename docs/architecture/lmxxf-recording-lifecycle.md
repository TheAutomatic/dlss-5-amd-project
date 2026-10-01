# lmxxf recording lifecycle foundations

This is infrastructure for recording leases. The product backend still uses the legacy
single pending job; it does not yet consume the recording observers or enable bridge leases.
These changes alone do not fix session teardown or replay in the shipped backend.

## Submission events

`CommandListProxy` assigns a stable list identity, a recording generation, and an execution
serial. Successful Reset and final COM Release invalidate the exact recording generation.
Close, failed Reset, elapsed frames, and successful submission do not invalidate it.
The recording owns its observer; the observer must not retain the proxy COM object.

Record/Execute/Reset/invalidation use `RecordingMutex`. Observers may transfer ownership
under that lock, but must not wait for GPU completion. Final proxy destruction unlocks
before the logical-list destructor can wait. Runtime/session destruction needs separate
asynchronous ownership when the backend is connected.

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
the runtime Job must supply and own the lease when the v2 contract is connected.

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

## Validation and remaining integration

The WARP tier exercises real proxy Reset/Release, failed Reset, delayed recordings,
replay, cross-queue ordering, Signal failure and concurrent Execute/Reset. The exposure
lease test records 33 distinct bindings, destroys the exposure owner, then executes in
reverse order and replays with numerical readback and D3D12 debug checks.

The AMD GPU tier exercises actual HIP inference with nonzero input, recording discard,
post-HIP consumer discard and cross-queue replay with byte-identical nonzero output.
Formal patches reproduce the independent upstream fixtures; the Git roundtrip suite
checks both all shipping HIP binaries and raw patch/fixture evidence.

Still required: stable runtime Job storage, ABI v2 negotiation, per-recording immutable
pin snapshots, backend session owners, completion-aware invalidation/collection, and
runtime destruction outside the submission lock. Joint certification and the upstream
0.38 integration remain subsequent steps.
