# P1 / G1 submission split

## Call-site verdict

`AmdBridge::Before` records onto the game `ID3D12GraphicsCommandList` while it is still
open. EvaluateFeature then records FSR on the **same** object. The game keeps that
pointer after preSR. There is no natural submit boundary: closing or executing the
game list at Before would break later FSR recording.

Selected scheme: a COM proxy returned from `CreateCommandList`, splitting physical
segments at preSR. Not graphics snapshot.

## Done

- `LogicalList`: split Execute (producer then continuation); optional **between**
  callback (HIP slot) after producer Execute.
- `CommandListProxy`: **ID3D12GraphicsCommandList1–10** full forward; QI succeeds.
  `BeginRenderPass` marks split **ineligible** (fail-closed); Split returns
  `ERROR_NOT_SUPPORTED`.
- `SubmissionHooks.h`: Detour `CreateCommandList` → wrap DIRECT lists as proxy;
  Detour `ExecuteCommandLists` → expand `ILogicalCommandList` with between.
  **Default disarmed.** Product must not `Arm` until G1/P3. Split’s continuation
  create uses `SuppressProxyWrap` so it is not double-wrapped.
- `CreateCommandList1`: pass-through (no allocator at create time) — not wrapped.
- Tests: `tools/test-lmxxf-list-split.cmd`, `tools/test-lmxxf-create-execute.cmd`.
  `LmxxfWired()` stays false.

## Not yet

- Continuation initial-state snapshot/restore (IA/SO/OM/VRS/RT…)
- Cross-Execute resource promotion/decay tracking
- Wire `Arm` into OptiScaler product path (still harness-only)
- Real HIP enqueue in the between slot (callback is the slot)