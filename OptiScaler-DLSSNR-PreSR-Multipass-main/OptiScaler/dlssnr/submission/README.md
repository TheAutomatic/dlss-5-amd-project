# P1 / G1 submission split

## Done (harness)

- `CommandListProxy` List1–10; Create/Execute Detours in `SubmissionHooks` (**disarmed** by default).
- Split Execute with optional **between** callback (HIP slot).
- `ContinuationState` seed: viewport/scissor/topology/PSO/roots/heaps/blend/stencil/OM.
- `ResourceStateBook`: refuse open split barrier / aliasing at cut.
- Tests: `test-lmxxf-list-split.cmd`, `test-lmxxf-create-execute.cmd`.

## Not yet

- Product Arm call site: `LmxxfEvaluateCut` (dead until `LmxxfWired()`); real session `SetPendingEnqueue` still open; full IA/VRS/SO/RT state;
  full cross-Execute promotion/decay; CreateCommandList1 wrap; `LmxxfWired()`.
