# P1 / G1 submission split

## Done (harness)

- `CommandListProxy` List1–10; Create/Execute Detours in `SubmissionHooks` (**disarmed** by default).
- `CreateCommandList1`: wrap as **closed** proxy; producer allocator binds on first `Reset`.
- Split Execute with optional **between** callback (HIP slot).
- `ContinuationState` seed: viewport/scissor/topology/PSO/roots/heaps/blend/stencil/OM.
- `ResourceStateBook`: refuse open split barrier / aliasing at cut.
- Product Execute path: when `ExpandEnabled()`, `AmdBridge::ExecuteBatch` always runs `ExecuteExpanded` (QI `ILogicalCommandList` → `ExecuteOnWithBetween`). `PendingListIndex` is Daniel-only batch isolation; lmxxf returns -1 on purpose.
- Min G1 admission reject (`MarkSplitIneligible`): open split barrier / aliasing / `resource_state`; `BeginQuery`/`EndQuery`/`ResolveQueryData` → `query`; `SetPredication` → `predication`; List7 `Barrier` → `enhanced_barrier`; render pass → `render_pass`. Split then fails → ordinary SR (safe reject). Covered by `lmxxf_list_split` query case.
- Tests: `test-lmxxf-list-split.cmd`, `test-lmxxf-create-execute.cmd`, `test-lmxxf-list1-wrap.cmd`, `test-lmxxf-evaluate-cut.cmd`.

## Not yet

- Product Arm live (`LmxxfWired()`); full IA/VRS/SO/RT continuation; cross-Execute promotion/decay.
- Richer CL1 stress beyond Reset→Split; full continuation / promotion-decay (plan D).