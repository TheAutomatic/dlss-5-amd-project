# P1 / G1 submission split

## Done (harness)

- `CommandListProxy` List1–10; Create/Execute Detours in `SubmissionHooks` (**disarmed** by default).
- `CreateCommandList1`: wrap as **closed** proxy; producer allocator binds on first `Reset`.
- Split Execute with optional **between** callback (HIP slot).
- `ContinuationState` seed: viewport/scissor/topology/PSO/root/heaps/blend/stencil/OM + IA (IB/VB), SO, VRS, strip-cut, view-instance mask + **root tables/CBV/SRV/UAV/constants** (`RootBindState`) + **sample positions** + **depth bounds**.
- Fail-closed (no restore yet): meta / RTAS / `SetPipelineState1` / `DispatchRays` → `MarkSplitIneligible`; root index/constants overflow → `root_overflow`; sample-position overflow → `sample_positions`.
- `ResourceStateBook`: refuse open split barrier / aliasing; after Split apply **Execute decay** (read states → COMMON; RT/DSV-write/UAV/COPY_DEST/… survive).
- Product Execute path: when `ExpandEnabled()`, `AmdBridge::ExecuteBatch` always runs `ExecuteExpanded` (QI `ILogicalCommandList` → `ExecuteOnWithBetween`). `PendingListIndex` is Daniel-only batch isolation; lmxxf returns -1 on purpose.
- Unreal startup: install the Execute hook before exposing early proxies; before the first swapchain, wrap only DIRECT lists created by the host executable. Other modules retain native lists until the existing post-swapchain switch. The swapchain log reports how many game lists were wrapped early.
- Min G1 admission reject (`MarkSplitIneligible`): open split barrier / aliasing / `resource_state`; open Begin/End query at the cut, invalid query scope, predication / enhanced_barrier / render_pass. Completed queries and timestamp EndQuery remain eligible. Split failure → ordinary SR (safe reject).
- Tests: `test-lmxxf-list-split.cmd`, `test-lmxxf-create-execute.cmd`, `test-lmxxf-list1-wrap.cmd`, `test-lmxxf-evaluate-cut.cmd`.

## Not yet

- Product Arm live (`LmxxfWired()`); live-game debug-layer promotion/decay proof (plan E).
- G3∧G4∧G5 + release default (plan F).
