# P1 / G1 submission split

## Done (harness)

- `CommandListProxy` List1–10; Create/Execute Detours in `SubmissionHooks` (**disarmed** by default).
- `CreateCommandList1`: wrap as **closed** proxy; producer allocator binds on first `Reset`.
- Split Execute with optional **between** callback (HIP slot).
- `ContinuationState` seed: viewport/scissor/topology/PSO/roots/heaps/blend/stencil/OM.
- `ResourceStateBook`: refuse open split barrier / aliasing at cut.
- Product Execute path: when `ExpandEnabled()`, `AmdBridge::ExecuteBatch` always runs `ExecuteExpanded` (QI `ILogicalCommandList` → `ExecuteOnWithBetween`). `PendingListIndex` is Daniel-only batch isolation; lmxxf returns -1 on purpose.
- Tests: `test-lmxxf-list-split.cmd`, `test-lmxxf-create-execute.cmd`, `test-lmxxf-list1-wrap.cmd`, `test-lmxxf-evaluate-cut.cmd`.

## Not yet

- Product Arm live (`LmxxfWired()`); full IA/VRS/SO/RT continuation; cross-Execute promotion/decay.
- Admission reject for unknown state at cut (G1 min); richer CL1 stress beyond Reset→Split.
