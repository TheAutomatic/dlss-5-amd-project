# Command-list submission split

Contract only. Backend/product: `../backend/README.md`. Status: `.handoff/HANDOFF.md` · `exports/lmxxf-*.md`.

- Proxy `ID3D12GraphicsCommandList1`–`10` + `ILogicalCommandList`; CL1 starts closed, allocator on first `Reset`.
- `SplitSegments` + `ExecuteOnWithBetween` (HIP between producer and continuation). Hooks disarmed unless `SubmissionHooksWanted()`.
- **No split** (ordinary SR): open split barrier, open/invalid query, predication, enhanced barrier, render pass, meta, RTAS, `SetPipelineState1`, `DispatchRays`, root/sample overflow. Completed aliasing barrier and finished queries are OK.
- Continuation seed: viewport/scissor/PSO/roots/heaps/OM + IA/SO/VRS + root binds + sample/depth. Decay updates our book only — not game barriers.
- Harness: `tools/test-lmxxf-list-split|create-execute|list1-wrap|evaluate-cut.cmd`.
