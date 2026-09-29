# Command-list submission split

Contract only. Backend/product: `../backend/README.md`. Design notes: `docs/backends/lmxxf.md`, `docs/architecture/`.

- Proxy `ID3D12GraphicsCommandList1`–`10` + `ILogicalCommandList`; CL1 starts closed, allocator on first `Reset`.
- `SplitSegments` + `ExecuteOnWithBetween` (HIP between producer and continuation). Hooks disarmed unless `SubmissionHooksWanted()`.
- **No split** (ordinary SR): open split barrier, open/invalid query, predication, enhanced barrier, render pass, meta, RTAS builds/copies, root/sample overflow. Completed aliasing barriers, finished queries and recorded ray dispatches are OK.
- Continuation seed: viewport/scissor/PSO/roots/heaps/OM + IA/SO/VRS + root binds + sample/depth. Retain the DXR state object and restore PSO/state-object bindings in last-write order; `DispatchRays` stays in the producer and is never replayed. Decay updates our book only — not game barriers.
- Harness: `tests\lmxxf\run.cmd device` (`lmxxf_list_split`, `lmxxf_create_execute`, `lmxxf_list1_wrap`, `lmxxf_evaluate_cut`; hardware D3D12). Same-frame boundary: `tests\lmxxf\run.cmd warp`.
