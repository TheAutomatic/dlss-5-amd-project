# Command-list submission split

Contract only. Backend/product: `../backend/README.md`. Design notes: `docs/backends/lmxxf.md`, `docs/architecture/`.

- Proxy `ID3D12GraphicsCommandList1`–`10` + `ILogicalCommandList`; CL1 starts closed, allocator on first `Reset`.
- `SplitSegments` + `ExecuteOnWithBetween` (HIP between producer and continuation). Hooks disarmed unless `SubmissionHooksWanted()`.
- **No split** (ordinary SR): open split barrier, open/invalid query, predication, enhanced barrier, render pass, meta, null state object/ray-dispatch descriptor, root/sample overflow. Completed aliasing barrier and finished queries are OK. AS-build commands no longer block the cut: they stay on the producer like DispatchRays, so RT and NR can run together.
- Continuation seed: viewport/scissor/PSO/state object/roots/heaps/OM + IA/SO/VRS + root binds + sample/depth. The PSO and state object retain COM references and their last binding order; Reset/ClearState discard both. List4 support is checked before closing a producer that needs state-object restoration. Recorded `DispatchRays` and AS-build commands stay in their original segment and are never replayed. Decay updates our book only — not game barriers.
- Diagnostics: `SplitRejectionReason` reports all current blockers. `lmxxf continuity v1` counts Evaluate outcomes (NR recorded/original and transitions), not presented frames or GPU completions.
- Harness: `tests\lmxxf\run.cmd device` (`lmxxf_state_object`, `lmxxf_list_split`, `lmxxf_create_execute`, `lmxxf_list1_wrap`, `lmxxf_evaluate_cut`; hardware D3D12). State-object coverage needs SDK DXC and DXR (prints SKIP without a DXR adapter): producer/continuation GPU readback, both PSO/state-object binding orders, repeated binds, Reset/ClearState and retained RTAS rejection. Same-frame boundary: `tests\lmxxf\run.cmd warp`.
