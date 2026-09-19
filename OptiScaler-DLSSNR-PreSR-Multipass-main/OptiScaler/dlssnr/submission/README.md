# P1 submission split (no NR)

## Call-site verdict

`AmdBridge::Before` records onto the game `ID3D12GraphicsCommandList` while it is still
open. EvaluateFeature then records FSR on the **same** object. The game keeps that
pointer after preSR. There is no natural submit boundary: closing or executing the
game list at Before would break later FSR recording.

Selected scheme (after this bookkeeping proof): a COM proxy returned from
`CreateCommandList`, splitting physical segments at preSR. Not graphics snapshot.
Not hooked in this increment.

## This increment

`LogicalList` owns the split Execute contract on lists **we** create:

1. Close producer, create a continuation on a **new** allocator (never Reset the
   in-flight producer allocator).
2. `Execute` submits producer then continuation, each once.
3. Generation increments on `Reset`.

GPU test: unsplit copy vs split copy, readback equal. No HIP, no game hook,
`LmxxfWired()` stays false.

## This increment also

`CommandListProxy` implements **base** `ID3D12GraphicsCommandList` only. QI for
`ID3D12GraphicsCommandList1`..`10` is `E_NOINTERFACE` (fail-closed). Not installed
on `CreateCommandList`. GPU test records through the proxy pointer across Split.

## Not yet

- Wrap List1–10 (render pass, VRS, mesh, barriers 1.1, …)
- Hook `CreateCommandList` / `ExecuteCommandLists` in the game
- Continuation initial state snapshot
- Cross-Execute resource promotion/decay tracking
- Inserting HIP between the two Executes
